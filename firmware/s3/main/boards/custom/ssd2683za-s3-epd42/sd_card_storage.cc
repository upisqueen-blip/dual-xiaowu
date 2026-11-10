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
    // MISO 在整段探测里是否出现过低电平。全程恒 1 说明 DAT0 这条线上根本没有
    // 器件在驱动（卡没插到底 / 卡座该脚没接到 MCU / 卡没供电）；
    // 出现过 0 却仍无 CMD0 应答，则是卡在通信但握手失败（时序/时钟太慢）。
    bool miso_saw_zero = false;
    int miso_low_bits = 0;
    // 线路存在性：网表上 CS/CLK/CMD/DAT0 四根线各有 10k 上拉到 VDD3V3。
    // 把引脚配成"输入 + 内部下拉"后，线上有 10k 上拉时仍读 1（分压≈2.7V），
    // 走线断/卡座没焊/上拉没贴时内部下拉会把它拉到 0。这是唯一能区分
    // "线在但卡不应答" 与 "线根本不通" 的证据 —— 普通读法开着内部上拉，
    // 断线也读 1，会把人误导向卡的问题。
    int line_cs = -1;
    int line_clk = -1;
    int line_cmd = -1;
    int line_dat0 = -1;
    bool alive() const { return cmd0_resp == 0x01; }
    bool lines_ok() const {
        return line_cs == 1 && line_clk == 1 && line_cmd == 1 && line_dat0 == 1;
    }
};

// 输入+内部下拉读一次四根线的电平，判断 10k 上拉是否在线。
void CheckLinePresence(SdProbe* probe) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << SD_CS_PIN) | (1ULL << SD_SCK_PIN) |
                       (1ULL << SD_MOSI_PIN) | (1ULL << SD_MISO_PIN);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    esp_rom_delay_us(3000);
    probe->line_cs = gpio_get_level(SD_CS_PIN);
    probe->line_clk = gpio_get_level(SD_SCK_PIN);
    probe->line_cmd = gpio_get_level(SD_MOSI_PIN);
    probe->line_dat0 = gpio_get_level(SD_MISO_PIN);
}

// ≈ 40 kHz。SD 卡上电初始化阶段只要求 ≤400 kHz，放慢能显著提高老旧卡、
// 长走线、卡座接触电阻偏大时的握手成功率（快时钟下边沿抖动会被当成错帧）。
void SdDelayTicks() { esp_rom_delay_us(8); }

int SdTransfer(uint8_t out, SdProbe* probe = nullptr) {
    int in = 0;
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(SD_MOSI_PIN, (out >> (7 - i)) & 0x01);
        SdDelayTicks();
        gpio_set_level(SD_SCK_PIN, 1);
        SdDelayTicks();
        const int bit = gpio_get_level(SD_MISO_PIN);
        if (probe && bit == 0) {
            probe->miso_saw_zero = true;
            ++probe->miso_low_bits;
        }
        in = (in << 1) | bit;
        gpio_set_level(SD_SCK_PIN, 0);
        SdDelayTicks();
    }
    return in;
}

int SdReadResponse(SdProbe* probe = nullptr) {
    for (int i = 0; i < 8; ++i) {
        const int value = SdTransfer(0xFF, probe);
        if ((value & 0x80) == 0) return value;  // 首字节 bit7=0 才是响应
    }
    return 0xFF;
}

SdProbe BitBangProbe() {
    SdProbe result;
    // 先判断四根线的 10k 上拉是否在线（必须在把引脚改成输出之前做）。
    CheckLinePresence(&result);

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
    // SD 规范要求 VDD 稳定后再等 1 ms 才能发命令；实测部分大容量卡（尤其
    // 卡座上有 LC 滤波、上电斜率偏慢的板子）需要更久才肯应答。这里给足 100 ms。
    esp_rom_delay_us(100000);
    result.cs_idle = gpio_get_level(SD_CS_PIN);
    result.dat0_idle = gpio_get_level(SD_MISO_PIN);

    // CS 拉高状态下打 160 个时钟，让卡完成上电同步（规范要求 ≥74 个）。
    for (int i = 0; i < 20; ++i) SdTransfer(0xFF, &result);

    const uint8_t cmd0[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    for (int attempt = 0; attempt < 10 && !result.alive(); ++attempt) {
        gpio_set_level(SD_CS_PIN, 0);
        SdDelayTicks();
        SdTransfer(0xFF, &result);  // CS 下降沿后的一个哑字节，给卡一点建立时间
        for (uint8_t byte : cmd0) SdTransfer(byte, &result);
        result.cmd0_resp = SdReadResponse(&result);
        gpio_set_level(SD_CS_PIN, 1);
        SdTransfer(0xFF, &result);
        result.cmd0_tries = attempt + 1;
        if (!result.alive()) esp_rom_delay_us(5000);
    }

    if (result.alive()) {
        const uint8_t cmd8[6] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
        gpio_set_level(SD_CS_PIN, 0);
        SdDelayTicks();
        for (uint8_t byte : cmd8) SdTransfer(byte, &result);
        result.cmd8_resp = SdReadResponse(&result);
        gpio_set_level(SD_CS_PIN, 1);
        SdTransfer(0xFF, &result);
    }

    gpio_reset_pin(SD_CS_PIN);
    gpio_reset_pin(SD_SCK_PIN);
    gpio_reset_pin(SD_MOSI_PIN);
    gpio_reset_pin(SD_MISO_PIN);
    return result;
}

std::string DescribeProbe(const SdProbe& probe) {
    char text[520];
    // 线路存在性优先报：这是唯一能区分"线断了"和"卡不应答"的证据。
    std::snprintf(text, sizeof(text),
                  "线路自检(下拉读到高=该线有10k上拉): CS=%d CLK=%d CMD=%d DAT0=%d（1=正常）",
                  probe.line_cs, probe.line_clk, probe.line_cmd, probe.line_dat0);
    std::string why = text;

    if (!probe.lines_ok()) {
        why += "；读 0 的脚：";
        if (probe.line_cs != 1) why += " CS(GPIO1-卡座2)";
        if (probe.line_clk != 1) why += " CLK(GPIO4-卡座5)";
        if (probe.line_cmd != 1) why += " CMD(GPIO5-卡座3)";
        if (probe.line_dat0 != 1) why += " DAT0(GPIO2-卡座7)";
        why += "。可能虚焊/断路，也可能只是内部下拉偏强把它拉低了，用万用表复测确认。";
    }

    char buf[260];
    if (probe.alive()) {
        std::snprintf(buf, sizeof(buf),
                      "；CMD0 应答 0x%02X（第 %d 次），CMD8 应答 0x%02X，卡是活的",
                      probe.cmd0_resp, probe.cmd0_tries, probe.cmd8_resp);
        why += buf;
        return why;
    }

    std::snprintf(buf, sizeof(buf),
                  "；CMD0 尝试 %d 次无应答(0x%02X)，DAT0 低电平位数=%d",
                  probe.cmd0_tries, probe.cmd0_resp, probe.miso_low_bits);
    why += buf;
    if (probe.dat0_idle == 0) {
        why += "；DAT0 被拉死在 0，查卡座该脚虚焊/短地";
    } else if (probe.lines_ok() && !probe.miso_saw_zero) {
        why += "；四线上拉都在、但卡完全不应答 —— 问题在卡本身或卡座弹片接触"
               "（万用表通断测的是焊盘到 MCU，测不到卡片与弹片这一层）："
               "先换一张 FAT32 卡试，再确认卡是否插到底，并量插卡瞬间卡座 VDD 是否跌破 3.0V";
    } else if (!probe.miso_saw_zero) {
        why += "；DAT0 全程无低电平且线路读数异常，优先按上面标出的脚复测通断";
    } else {
        why += "；DAT0 有跳变但无合法响应，偏时序/时钟问题，或 CS 实际接到了卡座其它脚";
    }
    return why;
}

// ---------------------------------------------------------------------------
// 引脚组合扫描
//
// 万用表通断只能证明"某个脚接到了某个 GPIO"，无法证明"接的是哪个信号"。
// 卡座脚序与原理图符号不一致、或焊了不同型号的卡座时，四根线全通，但
// CLK/CMD/DATA 之间相互交叉，SD 卡永远收不到合法命令，表现就是 CMD0 恒 0xFF
// —— 这时无论怎么排查通断和供电都找不到问题。
// 这里把四个候选脚做全排列，逐个试一条 CMD0：只要有一种组合能应答，就说明
// 实际接线就是它，按日志把 config.h 里四个 SD_*_PIN 改成对应值即可。
// ---------------------------------------------------------------------------
struct WiringResult {
    bool found = false;
    int cs = -1;
    int clk = -1;
    int mosi = -1;
    int miso = -1;
    int tried = 0;
};

// gpio_set_level / gpio_get_level 的形参是 gpio_num_t（强枚举），
// 这里统一在入口处转换一次，避免每个调用点都写 static_cast。
int WireXfer(int clk, int mosi, int miso, uint8_t out) {
    const gpio_num_t g_clk = static_cast<gpio_num_t>(clk);
    const gpio_num_t g_mosi = static_cast<gpio_num_t>(mosi);
    const gpio_num_t g_miso = static_cast<gpio_num_t>(miso);
    int in = 0;
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(g_mosi, (out >> (7 - i)) & 0x01);
        SdDelayTicks();
        gpio_set_level(g_clk, 1);
        SdDelayTicks();
        in = (in << 1) | gpio_get_level(g_miso);
        gpio_set_level(g_clk, 0);
        SdDelayTicks();
    }
    return in;
}

bool WireTryCmd0(int cs, int clk, int mosi, int miso) {
    const gpio_num_t g_cs = static_cast<gpio_num_t>(cs);
    const gpio_num_t g_clk = static_cast<gpio_num_t>(clk);
    const gpio_num_t g_mosi = static_cast<gpio_num_t>(mosi);

    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << cs) | (1ULL << clk) | (1ULL << mosi);
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_DISABLE;
    out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out);

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << miso;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in);

    gpio_set_level(g_cs, 1);
    gpio_set_level(g_clk, 0);
    gpio_set_level(g_mosi, 1);
    esp_rom_delay_us(8000);  // 要试 24 种组合，上电等待不能像正式挂载那样给 100ms

    for (int i = 0; i < 10; ++i) WireXfer(clk, mosi, miso, 0xFF);  // 80 个上电同步时钟

    const uint8_t cmd0[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    gpio_set_level(g_cs, 0);
    SdDelayTicks();
    WireXfer(clk, mosi, miso, 0xFF);
    for (uint8_t b : cmd0) WireXfer(clk, mosi, miso, b);
    int resp = 0xFF;
    for (int i = 0; i < 8; ++i) {
        const int v = WireXfer(clk, mosi, miso, 0xFF);
        if ((v & 0x80) == 0) { resp = v; break; }
    }
    gpio_set_level(g_cs, 1);
    WireXfer(clk, mosi, miso, 0xFF);
    return resp == 0x01;
}

void WireRelease() {
    gpio_reset_pin(SD_CS_PIN);
    gpio_reset_pin(SD_SCK_PIN);
    gpio_reset_pin(SD_MOSI_PIN);
    gpio_reset_pin(SD_MISO_PIN);
}

WiringResult ScanPinout() {
    WiringResult r;
    // 先按 config.h 的名义组合试一次，命中就不必穷举。
    if (WireTryCmd0(SD_CS_PIN, SD_SCK_PIN, SD_MOSI_PIN, SD_MISO_PIN)) {
        r.found = true;
        r.cs = SD_CS_PIN;
        r.clk = SD_SCK_PIN;
        r.mosi = SD_MOSI_PIN;
        r.miso = SD_MISO_PIN;
        r.tried = 1;
        WireRelease();
        return r;
    }
    const int cand[4] = {SD_CS_PIN, SD_MISO_PIN, SD_SCK_PIN, SD_MOSI_PIN};
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            if (b == a) continue;
            for (int c = 0; c < 4; ++c) {
                if (c == a || c == b) continue;
                for (int d = 0; d < 4; ++d) {
                    if (d == a || d == b || d == c) continue;
                    ++r.tried;
                    const int cs = cand[a], miso = cand[b], clk = cand[c], mosi = cand[d];
                    if (WireTryCmd0(cs, clk, mosi, miso)) {
                        r.found = true;
                        r.cs = cs;
                        r.clk = clk;
                        r.mosi = mosi;
                        r.miso = miso;
                        WireRelease();
                        return r;
                    }
                }
            }
        }
    }
    WireRelease();
    return r;
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

    // 挂载顺序与上游基线保持一致：先初始化总线、直接交给 SDSPI 挂载，
    // 只有挂载失败后才做 GPIO 级自检来解释原因。反过来（先自检、不过就
    // 不挂载）会把自检自身的时序缺陷放大成"永远挂不上"的硬故障。
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
    // 因此只保留一次延时重试（够覆盖上电偏慢的卡），全部失败才判定为不在位，
    // 再走下面的 CMD0 自检给出原因。
    esp_err_t err = ESP_FAIL;
    sdmmc_card_t* card = nullptr;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(200));
        const esp_err_t bus_err = spi_bus_initialize(SD_SPI_NUM, &bus, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK) {
            ESP_LOGE(kTag, "SPI bus init failed: %s", esp_err_to_name(bus_err));
            mount_error_ = std::string("SPI2 总线初始化失败：") + esp_err_to_name(bus_err);
            return false;
        }
        // 空座是正常的可选设备状态，压掉底层反复输出的 error 级日志。
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
        err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
        if (err == ESP_OK) break;
        card = nullptr;
        spi_bus_free(SD_SPI_NUM);
        ESP_LOGW(kTag, "TF mount attempt %d/2 failed: %s", attempt + 1, esp_err_to_name(err));
        // 只有握手类失败值得重试；文件系统级错误重试不会变好。
        if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_INVALID_RESPONSE) {
            break;
        }
    }

    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND ||
            err == ESP_ERR_INVALID_RESPONSE) {
            // 总线已在循环里释放（引脚归 GPIO 才能位翻转），这里跑一次
            // CMD0 自检，把结论写进上报给后台的原因里。
            const SdProbe probe = BitBangProbe();
            std::string detail = DescribeProbe(probe);
            if (!probe.alive()) {
                // CMD0 无应答时再跑一次引脚组合扫描：这是唯一能发现
                // "四线全通但信号交叉"（卡座脚序与原理图不符）的手段。
                const WiringResult wiring = ScanPinout();
                char wbuf[260];
                if (wiring.found) {
                    std::snprintf(wbuf, sizeof(wbuf),
                                  "；【引脚扫描】CS=GPIO%d CLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d "
                                  "时 CMD0 才有应答 → 卡座脚序与 config.h 不符，按此改 SD_*_PIN 即可",
                                  wiring.cs, wiring.clk, wiring.mosi, wiring.miso);
                } else {
                    std::snprintf(wbuf, sizeof(wbuf),
                                  "；【引脚扫描】%d 种组合均无应答 → 四根线接线正确，"
                                  "问题在卡片本身或卡座弹片接触，优先换一张 FAT32 卡",
                                  wiring.tried);
                }
                detail += wbuf;
            }
            if (probe.alive()) {
                // 卡能被寻址，说明引脚/供电/卡座都是通的，卡住的是 SDSPI 的
                // 挂载（初始化波特率或卡上电偏慢）。不能再报「未检测到卡」，
                // 否则现场会去乱查硬件。
                ESP_LOGW(kTag, "TF bus OK but SDSPI mount timed out: %s", detail.c_str());
                mount_error_ =
                    "TF 卡能被寻址（CMD0 已应答）但 SPI 挂载超时，多为卡座接触不稳"
                    "或卡上电较慢，请重新插拔后再试。" + detail;
            } else {
                ESP_LOGW(kTag, "TF card not responding: %s", detail.c_str());
                mount_error_ = "未检测到 TF 卡。" + detail;
            }
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
        // 总线已在重试循环里释放，这里不需要再 free。
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

bool SdCardStorage::Capacity(uint32_t& total_mb, uint32_t& free_mb) const {
    if (!mounted_) return false;
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &free) != ESP_OK) return false;
    total_mb = static_cast<uint32_t>(total / 1048576ULL);
    free_mb = static_cast<uint32_t>(free / 1048576ULL);
    return true;
}

std::string SdCardStorage::Status() const {
    if (!mounted_) return "TF card not mounted";
    // 不能用 %llu 拼字符串：本工程开了 CONFIG_LIBC_NEWLIB_NANO_FORMAT，
    // nano printf 不支持 ll 长度修饰符，会把 "total=%llu" 原样吐成
    // "total=lu"（后台面板上那句看不懂的 "lu MB" 就是这里来的）。
    // 用 std::to_string 绕开格式化差异。
    uint32_t total_mb = 0, free_mb = 0;
    if (!Capacity(total_mb, free_mb)) return "TF card mounted";
    return "TF mounted; total=" + std::to_string(total_mb) + " MB, free=" +
           std::to_string(free_mb) + " MB";
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
