#include "sd_card_storage.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "audio_codec.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

namespace {
constexpr const char* kTag = "SdCardStorage";
constexpr const char* kRoot = "/sdcard/xiaowu";

// ---------------------------------------------------------------------------
// SPI 位翻转自检
//
// 卡座是自弹式、没有独立 CD 脚，因此"卡没插 / 卡插了但没接触 / 供电没到 /
// 走线断了"在 esp_vfs_fat_sdspi_mount 眼里全都只是一个 ESP_ERR_TIMEOUT，
// 后台和串口上完全分不清。挂载失败后这里用 GPIO 直接按 SPI 时序发一条
// CMD0，就能把结论分成两类：
//   * CMD0 有应答（0x01） → 卡是活的，问题在挂载参数/文件系统
//   * CMD0 无应答（0xFF） → 卡根本没被点亮，问题在卡座接触/供电/走线
// 同时读一次 DAT0 的空闲电平：有 10k 上拉时必须是 1，读到 0 说明这条线
// 被拉死在 GND 上（错件、虚焊、或者卡座引脚定义和网表不一致）。
// ---------------------------------------------------------------------------
struct SdProbe {
    int dat0_idle = -1;   // DAT0/MISO 空闲电平（上拉应为 1）
    int cs_idle = -1;     // CS 空闲电平（主机驱动为 1 才算正常）
    int cmd0_resp = -1;   // 0x01=卡已进入 idle，0xFF=无应答
    int cmd0_tries = 0;
    int cmd8_resp = -1;   // 0x01 说明是 SD v2（SDHC/SDXC）
    bool alive() const { return cmd0_resp == 0x01; }
};

void SdDelayTicks() { esp_rom_delay_us(3); }  // ≈ 160 kHz，远低于 400 kHz 上限

int SdTransfer(uint8_t out) {
    int in = 0;
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(SD_MOSI_PIN, (out >> (7 - i)) & 0x01);
        SdDelayTicks();
        gpio_set_level(SD_SCK_PIN, 1);
        SdDelayTicks();
        in = (in << 1) | gpio_get_level(SD_MISO_PIN);
        gpio_set_level(SD_SCK_PIN, 0);
        SdDelayTicks();
    }
    return in;
}

int SdReadResponse() {
    for (int i = 0; i < 8; ++i) {
        const int value = SdTransfer(0xFF);
        if ((value & 0x80) == 0) return value;  // 首字节 bit7=0 才是响应
    }
    return 0xFF;
}

SdProbe BitBangProbe() {
    SdProbe result;
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << SD_CS_PIN) | (1ULL << SD_SCK_PIN) | (1ULL << SD_MOSI_PIN);
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_DISABLE;
    out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out);

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << SD_MISO_PIN;
    in.mode = GPIO_MODE_INPUT;
    // 弱上拉只是为了不让悬空的 MISO 读出随机值；网表上真正的 10k 上拉在卡座侧。
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in);

    gpio_set_level(SD_CS_PIN, 1);
    gpio_set_level(SD_SCK_PIN, 0);
    gpio_set_level(SD_MOSI_PIN, 1);
    esp_rom_delay_us(5000);  // 等卡上电稳定
    result.cs_idle = gpio_get_level(SD_CS_PIN);
    result.dat0_idle = gpio_get_level(SD_MISO_PIN);

    // CS 拉高状态下打 80 个时钟，让卡完成上电同步。
    for (int i = 0; i < 10; ++i) SdTransfer(0xFF);

    const uint8_t cmd0[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    for (int attempt = 0; attempt < 5 && !result.alive(); ++attempt) {
        gpio_set_level(SD_CS_PIN, 0);
        SdDelayTicks();
        for (uint8_t byte : cmd0) SdTransfer(byte);
        result.cmd0_resp = SdReadResponse();
        gpio_set_level(SD_CS_PIN, 1);
        SdTransfer(0xFF);
        result.cmd0_tries = attempt + 1;
        if (!result.alive()) esp_rom_delay_us(2000);
    }

    if (result.alive()) {
        const uint8_t cmd8[6] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
        gpio_set_level(SD_CS_PIN, 0);
        SdDelayTicks();
        for (uint8_t byte : cmd8) SdTransfer(byte);
        result.cmd8_resp = SdReadResponse();
        gpio_set_level(SD_CS_PIN, 1);
        SdTransfer(0xFF);
    }

    gpio_reset_pin(SD_CS_PIN);
    gpio_reset_pin(SD_SCK_PIN);
    gpio_reset_pin(SD_MOSI_PIN);
    gpio_reset_pin(SD_MISO_PIN);
    return result;
}

std::string DescribeProbe(const SdProbe& probe) {
    char text[320];
    if (probe.alive()) {
        std::snprintf(text, sizeof(text),
                      "总线自检：DAT0 空闲=%d，CMD0 应答 0x%02X（第 %d 次），CMD8 应答 0x%02X",
                      probe.dat0_idle, probe.cmd0_resp, probe.cmd0_tries, probe.cmd8_resp);
        return text;
    }
    std::snprintf(text, sizeof(text),
                  "总线自检：DAT0 空闲=%d（正常应为 1），CMD0 尝试 %d 次无应答(0x%02X)",
                  probe.dat0_idle, probe.cmd0_tries, probe.cmd0_resp);
    std::string why = text;
    if (probe.dat0_idle == 0) {
        why += "；DAT0 被拉死在 0，检查卡座该脚是否虚焊/短地";
    } else {
        why += "；卡未应答，重点查卡座是否插到底、3V3 是否到卡座、CLK/CMD 走线";
    }
    return why;
}

struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t sample_rate = 24000;
    uint32_t byte_rate = 48000;
    uint16_t block_align = 2;
    uint16_t bits = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};

void EnsureDirectories() {
    mkdir(kRoot, 0775);
    mkdir("/sdcard/xiaowu/images", 0775);
    mkdir("/sdcard/xiaowu/audio", 0775);
    mkdir("/sdcard/xiaowu/recordings", 0775);
    mkdir("/sdcard/xiaowu/books", 0775);
    mkdir("/sdcard/xiaowu/cache", 0775);
}
}  // namespace

SdCardStorage::~SdCardStorage() {
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount("/sdcard", static_cast<sdmmc_card_t*>(card_));
        spi_bus_free(SD_SPI_NUM);
    }
}

bool SdCardStorage::Mount() {
    mount_error_.clear();
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI_PIN;
    bus.miso_io_num = SD_MISO_PIN;
    bus.sclk_io_num = SD_SCK_PIN;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_NUM;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_NUM;
    slot.gpio_cs = SD_CS_PIN;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 6;
    mount.allocation_unit_size = 16 * 1024;

    // 卡座是自弹式、没有独立 CD 脚，空座只能靠握手超时发现；而"卡插到底了
    // 但第一次上电时序没走完"和"卡坏/没接触"在第一次尝试时长得一模一样。
    // 因此重试 3 次（每次重新初始化总线并留出上电间隔），全部失败才判定为
    // 卡不在位，再走下面的 CMD0 自检给出原因。
    esp_err_t err = ESP_FAIL;
    sdmmc_card_t* card = nullptr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(150));
        const esp_err_t bus_err = spi_bus_initialize(SD_SPI_NUM, &bus, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(kTag, "SPI bus init failed: %s", esp_err_to_name(bus_err));
            mount_error_ = std::string("SPI2 总线初始化失败：") + esp_err_to_name(bus_err);
            return false;
        }
        // 空座是正常的可选设备状态，压掉底层反复输出的 error 级日志。
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
        err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
        if (err == ESP_OK) break;
        ESP_LOGW(kTag, "TF mount attempt %d/3 failed: %s", attempt + 1, esp_err_to_name(err));
        card = nullptr;
        spi_bus_free(SD_SPI_NUM);
    }

    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_INVALID_RESPONSE) {
            // 先把 SPI 总线还回去（引脚要归 GPIO 才能位翻转），再跑一次
            // CMD0 自检，把结论写进上报给后台的原因里。
            spi_bus_free(SD_SPI_NUM);
            const SdProbe probe = BitBangProbe();
            const std::string detail = DescribeProbe(probe);
            ESP_LOGW(kTag, "TF card not responding: %s", detail.c_str());
            mount_error_ = "未检测到 TF 卡。" + detail;
            return false;
        }
        if (err == ESP_FAIL) {
            mount_error_ =
                "TF 卡已被识别但文件系统挂载失败：请格式化为 FAT32（不支持 exFAT/NTFS）";
            ESP_LOGW(kTag, "TF card mount failed (filesystem): %s", esp_err_to_name(err));
        } else {
            mount_error_ = std::string("TF 卡挂载失败：") + esp_err_to_name(err);
            ESP_LOGW(kTag, "TF card unavailable: %s", esp_err_to_name(err));
        }
        spi_bus_free(SD_SPI_NUM);
        return false;
    }
    card_ = card;
    // The ESP-IDF FatFs has no exFAT support, yet an exFAT boot sector shares
    // just enough structure with FAT32 that f_mount may still succeed and all
    // later directory reads return unusable '?' names (and writes corrupt the
    // card). Detect the signature explicitly so the user gets an actionable
    // message instead of silent garbage.
    uint8_t boot[512];
    if (sdmmc_read_sectors(card, boot, 0, 1) == ESP_OK &&
        std::memcmp(boot + 3, "EXFAT", 5) == 0) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        mounted_ = false;
        card_ = nullptr;
        spi_bus_free(SD_SPI_NUM);
        mount_error_ =
            "TF 卡是 exFAT 格式，无法读取\n\n请在电脑上将卡格式化为 FAT32\n再拷入电子书";
        ESP_LOGE(kTag, "TF card is exFAT; ESP-IDF only supports FAT32. "
                       "Reformat the card as FAT32.");
        return false;
    }
    mounted_ = true;
    EnsureDirectories();
    ESP_LOGI(kTag, "TF card mounted at /sdcard");
    return true;
}

bool SdCardStorage::EnsureMounted() {
    if (mounted_) return true;
    return Mount();
}

bool SdCardStorage::SafeName(const std::string& name) {
    return !name.empty() && name.size() <= 96 && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.find("..") == std::string::npos;
}

std::string SdCardStorage::Directory(const std::string& category) {
    if (category == "images") return std::string(kRoot) + "/images";
    if (category == "audio") return std::string(kRoot) + "/audio";
    if (category == "recordings") return std::string(kRoot) + "/recordings";
    if (category == "books") return std::string(kRoot) + "/books";
    return {};
}

std::string SdCardStorage::Path(const std::string& category, const std::string& name) {
    const auto dir = Directory(category);
    return dir.empty() || !SafeName(name) ? std::string() : dir + "/" + name;
}

std::string SdCardStorage::Status() const {
    if (!mounted_) return "TF card not mounted";
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &free) != ESP_OK) return "TF card mounted";
    char text[128];
    std::snprintf(text, sizeof(text), "TF mounted; total=%llu MB, free=%llu MB",
                  total / 1048576ULL, free / 1048576ULL);
    return text;
}

std::string SdCardStorage::List(const std::string& category) const {
    if (!mounted_) return "TF card not mounted";
    const auto dir = Directory(category);
    if (dir.empty()) return "invalid category";
    DIR* handle = opendir(dir.c_str());
    if (!handle) return "directory unavailable";
    std::string result;
    while (auto* item = readdir(handle)) {
        if (item->d_name[0] == '.') continue;
        if (!result.empty()) result += "\n";
        result += item->d_name;
        if (result.size() > 1800) { result += "\n..."; break; }
    }
    closedir(handle);
    return result.empty() ? "(empty)" : result;
}

bool SdCardStorage::Load(const std::string& category, const std::string& name,
                         std::vector<uint8_t>& data, size_t limit, std::string& error) const {
    if (!mounted_) { error = "TF card not mounted"; return false; }
    const auto path = Path(category, name);
    if (path.empty()) { error = "invalid file name"; return false; }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { error = "file not found"; return false; }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    if (size <= 0 || static_cast<size_t>(size) > limit) {
        std::fclose(file); error = "file is empty or too large"; return false;
    }
    data.resize(size);
    const bool ok = std::fread(data.data(), 1, data.size(), file) == data.size();
    std::fclose(file);
    if (!ok) { data.clear(); error = "read failed"; }
    return ok;
}

bool SdCardStorage::RecordWav(AudioCodec* codec, const std::string& name, int seconds,
                              std::string& error) {
    if (!mounted_ || !codec) { error = "TF card or codec unavailable"; return false; }
    // Recover if the user inserted a prepared card without the application
    // directory or removed the recordings directory on a PC.
    mkdir(kRoot, 0775);
    mkdir("/sdcard/xiaowu/recordings", 0775);
    const auto path = Path("recordings", name);
    if (path.empty()) { error = "invalid file name"; return false; }
    const std::string temp = path + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "wb+");
    if (!file) {
        error = std::string("cannot create recording: ") + std::strerror(errno);
        ESP_LOGE(kTag, "fopen(%s) failed: errno=%d (%s)", temp.c_str(), errno,
                 std::strerror(errno));
        return false;
    }
    WavHeader header;
    header.sample_rate = codec->input_sample_rate();
    header.byte_rate = header.sample_rate * 2;
    std::fwrite(&header, 1, sizeof(header), file);
    codec->Start();
    codec->EnableInput(true);
    std::vector<int16_t> pcm(480);
    uint32_t samples = 0;
    const uint32_t target = header.sample_rate * seconds;
    while (samples < target) {
        pcm.resize(std::min<uint32_t>(480, target - samples));
        if (!codec->InputData(pcm)) { error = "microphone read failed"; break; }
        if (std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file) != pcm.size()) {
            error = "TF write failed"; break;
        }
        samples += pcm.size();
    }
    header.data_size = samples * sizeof(int16_t);
    header.file_size = header.data_size + sizeof(header) - 8;
    std::rewind(file);
    std::fwrite(&header, 1, sizeof(header), file);
    std::fflush(file);
    std::fclose(file);
    if (!error.empty()) { std::remove(temp.c_str()); return false; }
    std::remove(path.c_str());
    if (std::rename(temp.c_str(), path.c_str()) != 0) { error = "rename failed"; return false; }
    return true;
}

bool SdCardStorage::PlayWav(AudioCodec* codec, const std::string& name, std::string& error) const {
    if (!mounted_ || !codec) { error = "TF card or codec unavailable"; return false; }
    const auto path = Path("audio", name);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { error = "file not found"; return false; }
    WavHeader header;
    if (std::fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header.riff, "RIFF", 4) || std::memcmp(header.wave, "WAVE", 4) ||
        header.format != 1 || header.bits != 16 || header.channels != 1 ||
        header.sample_rate != static_cast<uint32_t>(codec->output_sample_rate())) {
        std::fclose(file); error = "requires mono 16-bit PCM WAV at codec sample rate"; return false;
    }
    codec->Start();
    codec->EnableOutput(true);
    std::vector<int16_t> pcm(480);
    while (true) {
        const size_t count = std::fread(pcm.data(), sizeof(int16_t), pcm.size(), file);
        if (!count) break;
        pcm.resize(count);
        codec->OutputData(pcm);
        pcm.resize(480);
    }
    std::fclose(file);
    return true;
}
