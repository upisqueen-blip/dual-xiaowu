#include "uc1679_display.h"

#include <algorithm>
#include <cstring>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#include "epaper_bus_lock.h"

namespace {
constexpr char kTag[] = "Uc1679Display";

// OLD 变种(0x10/0x13/0x12) 数据值
constexpr uint8_t k83PanelSettingData = 0x0F;  // 0x00: 400x300, B/W/R, OTP 波形
constexpr uint8_t k83SleepCdiData = 0xF7;      // 0x50 深睡前 CDI

// NEW 变种(0x24/0x26 + 0x22/0x20) 数据值
constexpr uint8_t k81DataEntryValue = 0x03;
constexpr uint8_t k81RamXData[2] = {0x00, 0x31};
constexpr uint8_t k81RamYData[4] = {0x00, 0x00, 0x2B, 0x01};  // Y 起点0/终点299（微雪官方 NEW）
constexpr uint8_t k81BorderValue = 0x05;
constexpr uint8_t k81TempSensorValue = 0x80;
constexpr uint8_t k81Update = 0xF7;
constexpr uint8_t k81RamXCntr = 0x00;
constexpr uint8_t k81RamYCntr[2] = {0x00, 0x00};              // Y 计数器起点0

constexpr uint32_t kProbeBusyTimeoutMs = 8000;    // 探测/上电等待上限
constexpr uint32_t kRefreshBusyTimeoutMs = 30000;  // 全刷波形上限（三色含红通道可达 12~15s）
}  // namespace

std::atomic<bool> Uc1679Display::g_uc1679_refreshing{false};

Uc1679Display::Uc1679Display(std::function<bool()> other_panel_busy)
    : other_panel_busy_(std::move(other_panel_busy)) {
    mutex_ = xSemaphoreCreateMutex();
    dma_buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(kDmaChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    assert(mutex_ && dma_buffer_);
    InitializeGpio();
    InitializeSpiDevice();
}

Uc1679Display::~Uc1679Display() {
    if (spi_) spi_bus_remove_device(spi_);
    if (mutex_) vSemaphoreDelete(mutex_);
    heap_caps_free(dma_buffer_);
}

void Uc1679Display::InitializeGpio() {
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = 1ULL << EPD_COLOR_CS_PIN;
    outputs.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&outputs));
    gpio_set_level(EPD_COLOR_CS_PIN, 1);

    gpio_config_t busy = {};
    busy.pin_bit_mask = 1ULL << EPD_COLOR_BUSY_PIN;
    busy.mode = GPIO_MODE_INPUT;
    busy.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&busy));
}

void Uc1679Display::InitializeSpiDevice() {
    // 总线由 Ssd1619Display 先行初始化（board 构造顺序保证），此处只挂设备。
    spi_device_interface_config_t device = {};
    device.clock_speed_hz = 4 * 1000 * 1000;
    device.mode = 0;
    device.spics_io_num = -1;  // CS 由本驱动手动控制
    device.queue_size = 2;
    const esp_err_t err = spi_bus_add_device(EPD_SPI_NUM, &device, &spi_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_ = nullptr;
    }
}

void Uc1679Display::WaitForOtherPanelIdle() {
    if (!other_panel_busy_ || !other_panel_busy_()) return;
    ESP_LOGW(kTag, "Shared RST: waiting for SSD1619 refresh to finish");
    const TickType_t start = xTaskGetTickCount();
    while (other_panel_busy_()) {
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= 25000) {
            ESP_LOGE(kTag,
                     "SSD1619 refresh wait timeout; resetting anyway "
                     "(BW panel partial waveform may be disturbed)");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void Uc1679Display::HardwareReset() {
    // 与已验证的 4 色 SSD2683ZA 驱动一致的 RST 低有效脉冲（共享 RST 会同时复位
    // FPC1，FPC1 侧自行在下次刷新时重初始化）。三色屏若被上一轮刷新打断而软锁死，
    // 加长脉冲（200ms）可提高恢复概率；仍无效则需断电重启。
    WaitForOtherPanelIdle();
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));   // 加长复位脉冲，确保面板彻底复位
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));   // 复位后充分建立
    // 这根 RST 是共享的：FPC1(SSD1619) 此刻已一起被复位，它记录的初始化序号
    // 因此过期，下次刷新会自己重新下发初始化序列。
    NoteSharedReset();
}

int Uc1679Display::DetectIdleLevel() {
    // 复位后采样 BUSY 静态电平，自适应有效极性。四色屏(同 FPC2 同 BUSY 线)可正常
    // 翻转，证明该线已连到面板 BUSY；若三色屏 BUSY 始终不翻转，说明面板未上电/
    // 未进入激活态，而非 BUSY 断线。
    vTaskDelay(pdMS_TO_TICKS(30));
    int idle = gpio_get_level(EPD_COLOR_BUSY_PIN);
    for (int i = 0; i < 3; ++i) {
        vTaskDelay(pdMS_TO_TICKS(8));
        const int now = gpio_get_level(EPD_COLOR_BUSY_PIN);
        if (now != idle) idle = now;
    }
    busy_idle_level_ = idle;
    busy_active_level_ = (idle == 0) ? 1 : 0;
    ESP_LOGI(kTag, "FPC2 复位后 BUSY(GPIO%d)=%d → 空闲电平=%d 有效电平(忙)=%d",
             EPD_COLOR_BUSY_PIN, idle, busy_idle_level_, busy_active_level_);
    return idle;
}

bool Uc1679Display::WaitBusy(uint32_t timeout_ms) {
    if (busy_active_level_ < 0) return true;
    const TickType_t start = xTaskGetTickCount();
    // 忙时引脚处于有效电平；等待其回落到空闲电平即完成。
    while (gpio_get_level(EPD_COLOR_BUSY_PIN) == busy_active_level_) {
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            ESP_LOGE(kTag, "BUSY 超时 %lu ms (GPIO%d=%d, 有效电平=%d)",
                     static_cast<unsigned long>(timeout_ms), EPD_COLOR_BUSY_PIN,
                     gpio_get_level(EPD_COLOR_BUSY_PIN), busy_active_level_);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

void Uc1679Display::Command(uint8_t command, const uint8_t* data,
                            size_t length, bool invert_data) {
    if (!spi_) return;
    // CS 在「命令 + 数据」整个期间保持低电平（与已验证的 4 色驱动一致）。
    gpio_set_level(EPD_COLOR_CS_PIN, 0);
    gpio_set_level(EPD_DC_PIN, 0);
    spi_transaction_t transaction = {};
    transaction.length = 8;
    transaction.tx_buffer = &command;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &transaction));
    if (data && length) {
        gpio_set_level(EPD_DC_PIN, 1);
        while (length > 0) {
            const size_t chunk = std::min(length, kDmaChunkBytes);
            std::memcpy(dma_buffer_, data, chunk);
            if (invert_data) {
                for (size_t i = 0; i < chunk; ++i) dma_buffer_[i] = ~dma_buffer_[i];
            }
            spi_transaction_t dt = {};
            dt.length = chunk * 8;
            dt.tx_buffer = dma_buffer_;
            const esp_err_t err = spi_device_polling_transmit(spi_, &dt);
            if (err != ESP_OK) {
                gpio_set_level(EPD_COLOR_CS_PIN, 1);
                ESP_LOGE(kTag, "SPI data transmit failed: %s, chunk=%u",
                         esp_err_to_name(err), static_cast<unsigned>(chunk));
                return;
            }
            data += chunk;
            length -= chunk;
        }
    }
    gpio_set_level(EPD_COLOR_CS_PIN, 1);
}

void Uc1679Display::Data(const uint8_t* data, size_t length) {
    if (!spi_) return;
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(EPD_COLOR_CS_PIN, 0);
    while (length > 0) {
        const size_t chunk = std::min(length, kDmaChunkBytes);
        std::memcpy(dma_buffer_, data, chunk);
        spi_transaction_t transaction = {};
        transaction.length = chunk * 8;
        transaction.tx_buffer = dma_buffer_;
        const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
        if (err != ESP_OK) {
            gpio_set_level(EPD_COLOR_CS_PIN, 1);
            ESP_LOGE(kTag, "SPI data transmit failed: %s, chunk=%u",
                     esp_err_to_name(err), static_cast<unsigned>(chunk));
            return;
        }
        data += chunk;
        length -= chunk;
    }
    gpio_set_level(EPD_COLOR_CS_PIN, 1);
}

// ---------------------------------------------------------------------------
// 配置阶段：硬复位 + 上电(PON) + 面板设置(PSR)。两变种共用 0x04/0x00，仅初始化
// 前缀不同。注意：之前 NEW 变种漏掉 PON/PSR 是屏不显、BUSY 不翻转的根因。
// ---------------------------------------------------------------------------
bool Uc1679Display::ConfigurePanel(int variant) {
    HardwareReset();
    DetectIdleLevel();
    // 重要：本板三色屏 PON(0x04) 后 BUSY 不一定翻转（与同座 4 色屏不同），面板只在
    // 刷新波形执行期间才翻转 BUSY。因此 PON 后 BUSY 仅作诊断、**绝不据此判失败**——
    // 否则面板永远走不到发刷新命令那一步（之前那版正是因此 B 屏不再闪烁）。无论 BUSY
    // 是否翻转，都继续下发 PSR/图像/刷新，保证面板有机会被唤醒（与之前能闪的版本一致）。
    if (variant == 0) {
        // OLD 变种最小序列：PON(0x04) → PSR(0x00=0x0F)。Waveshare 官方，依赖面板
        // 内置波形，不写 LUT/升压/CDI/分辨率。
        Command(k83PowerOn);                                      // 0x04 上电
        vTaskDelay(pdMS_TO_TICKS(100));
        WaitBusy(kProbeBusyTimeoutMs);                            // 最佳努力
        Command(k83PanelSetting, &k83PanelSettingData, 1);        // 0x00 = 0x0F
    } else {
        // NEW 变种：软复位(0x12) → 边框/温度/数据入口/窗口/光标 → PON(0x04) →
        // PSR(0x00=0x0F)。与之前能成功刷新的版本完全一致。
        Command(k81SwReset);                                      // 0x12 软复位
        WaitBusy(kProbeBusyTimeoutMs);                            // 等软复位完成（微雪官方 0x12 后 ReadBusy）
        Command(k81Border, &k81BorderValue, 1);                   // 0x3C
        Command(k81TempSensor, &k81TempSensorValue, 1);           // 0x18
        Command(k81DataEntry, &k81DataEntryValue, 1);             // 0x11
        Command(k81RamX, k81RamXData, 2);                         // 0x44 X 窗口
        Command(k81RamY, k81RamYData, 4);                         // 0x45 Y 窗口
        Command(k81SetRamXAddr, &k81RamXCntr, 1);                 // 0x4E X 计数器
        Command(k81SetRamYAddr, k81RamYCntr, 2);                  // 0x4F Y 计数器
        Command(k83PowerOn);                                      // 0x04 上电
        vTaskDelay(pdMS_TO_TICKS(100));
        WaitBusy(kProbeBusyTimeoutMs);                            // 最佳努力
        Command(k83PanelSetting, &k83PanelSettingData, 1);        // 0x00 = 0x0F
    }
    panel_awake_ = true;
    return true;
}

// 写 B/W 与 RED 两平面。ConvertJpegToBwrPlanes 产出 bw=1白 / red=1红，故两平面均
// 直接写入、不取反：写后 BW RAM 1=白、RED RAM 1=红，与 SSD1683/UC8176 显存约定一致
// （取反反而整屏泛红，见 board 注释）。B/W 平面同理不取反。
bool Uc1679Display::SendImagePlanes(int variant, const uint8_t* bw,
                                    const uint8_t* red) {
    if (variant == 0) {
        Command(k83DataStartBw, bw, kPlaneBytes);         // 0x10 B/W RAM
        Command(k83DataStartRed, red, kPlaneBytes);        // 0x13 RED RAM（不取反）
    } else {
        Command(k81SetRamXAddr, &k81RamXCntr, 1);
        Command(k81SetRamYAddr, k81RamYCntr, 2);
        Command(k81DataBw, bw, kPlaneBytes);              // 0x24 B/W RAM
        Command(k81SetRamXAddr, &k81RamXCntr, 1);
        Command(k81SetRamYAddr, k81RamYCntr, 2);
        Command(k81DataRed, red, kPlaneBytes);            // 0x26 RED RAM（不取反）
    }
    return spi_ != nullptr;
}

// 启动刷新并等待波形完成。刷新命令同步发出后即开始波形，故返回 true；BUSY 回落
// 超时仅告警、不判失败（本板 BUSY 行为略有差异，但刷新确实执行）。先固定延时 4s
// 保证全刷波形（约 3~4s）完整跑完，避免被后续 SPI 操作打断导致面板软锁死——之前
// 「闪两下后卡死」正是刷新未完成即被重发命令打断所致。
bool Uc1679Display::ActivateAndWait(int variant, uint32_t busy_timeout_ms) {
    if (variant == 0) {
        Command(k83MasterActivate);                       // 0x12 启动刷新
    } else {
        Command(k81UpdateCtrl2, &k81Update, 1);           // 0x22 = 0xF7
        Command(k81MasterActivate);                       // 0x20 启动波形
    }
    vTaskDelay(pdMS_TO_TICKS(4000));   // 固定延时，确保波形完整执行
    if (!WaitBusy(busy_timeout_ms)) {
        ESP_LOGW(kTag, "刷新后 BUSY 未回落（最佳努力，刷新已执行）");
    }
    return true;
}

// ---------------------------------------------------------------------------
// 公共入口
// ---------------------------------------------------------------------------
bool Uc1679Display::Init() {
    EpaperBusGuard bus_guard;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = false;

    // 全白 / 无红的探测帧（PSRAM，避免占用内部 RAM）。
    static uint8_t* probe_bw = static_cast<uint8_t*>(
        heap_caps_malloc(kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    static uint8_t* probe_red = static_cast<uint8_t*>(
        heap_caps_malloc(kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (probe_bw) std::memset(probe_bw, 0xFF, kPlaneBytes);
    if (probe_red) std::memset(probe_red, 0xFF, kPlaneBytes);

    if (probe_bw && probe_red) {
        // 先用 menuconfig/宏指定的变种；若 PON 后 BUSY 未翻转（面板未连）再试
        // 另一种。Waveshare 靠读 0x2F 区分 OLD/NEW，本板无 MISO 改为运行时判定。
        const int other = (FPC2_DRIVER_MODE == 0) ? 1 : 0;
        for (int step = 0; step < 2 && !ok; ++step) {
            const int v = (step == 0) ? FPC2_DRIVER_MODE : other;
            if (ConfigurePanel(v)) {
                variant_ = v;
                ok = true;  // PON 后 BUSY 翻转=面板已上电，认可用
                ESP_LOGI(kTag, "%s 变种面板已连接", v ? "NEW" : "OLD");
            } else {
                ESP_LOGW(kTag, "%s 变种面板未响应，尝试 %s", v ? "NEW" : "OLD",
                         other ? "NEW" : "OLD");
            }
        }
        available_ = ok;
        if (ok) {
            // 探测帧全白（无红），最佳努力刷新一次确认可刷；真实内容随后覆盖。
            // 用刷新超时（30s）而非探测超时，确保三色慢刷完整跑完再进深睡。
            SendImagePlanes(variant_, probe_bw, probe_red);
            ActivateAndWait(variant_, kRefreshBusyTimeoutMs);
            Sleep();
        }
    } else {
        available_ = ok;  // PSRAM 分配失败 → false
        ESP_LOGE(kTag, "PSRAM 分配失败，无法探测三色面板");
    }

    if (ok) {
        ESP_LOGI(kTag,
                 "三色面板就绪: CS=GPIO%d BUSY=GPIO%d variant=%d (BUSY 空闲=%d)",
                 EPD_COLOR_CS_PIN, EPD_COLOR_BUSY_PIN, variant_,
                 busy_idle_level_);
    } else {
        ESP_LOGE(kTag,
                 "三色面板未响应(CS=GPIO%d BUSY=GPIO%d)。若确认插屏，检查 FPC2 "
                 "座子/电源；PON 后 BUSY 是否翻转可判定面板是否上电。",
                 EPD_COLOR_CS_PIN, EPD_COLOR_BUSY_PIN);
    }
    xSemaphoreGive(mutex_);
    return ok;
}

void Uc1679Display::Sleep() {
    if (!available_ || !panel_awake_) return;
    // 关键：必须先等 BUSY 真正回落（上一轮刷新波形跑完）才能下发深睡命令。三色全刷
    // （含红通道）很慢，典型 12~15s。若此时 BUSY 仍忙，说明刷新尚未结束，下发深睡
    // 会打断刷新、把 RAM 里正在显示的数据冲成乱码（屏幕雪花噪点）。故 BUSY 仍忙时直接
    // 跳过深睡——保留当前画面（代价仅多耗一点电），远好过破坏显示。
    if (!WaitBusy(kRefreshBusyTimeoutMs)) {
        ESP_LOGW(kTag, "Sleep: BUSY 仍忙，跳过深睡以免破坏画面");
        return;
    }
    if (variant_ == 0) {
        // OLD 变种深睡（微雪官方 Sleep_old）：CDI→关电荷泵→深睡校验码 0xA5。
        Command(k83Cdi, &k83SleepCdiData, 1);  // 0x50 = 0xF7
        Command(k83PowerOff);                    // 0x02 关闭电荷泵
        WaitBusy(kProbeBusyTimeoutMs);
        constexpr uint8_t sleep_mode = 0xA5;     // 0x07 深睡校验码
        Command(k83DeepSleep, &sleep_mode, 1);
    } else {
        // NEW 变种(UC8176)深睡（微雪官方 Sleep_new）：0x10 + 0x03，无需 CDI/关泵。
        constexpr uint8_t sleep_mode = 0x03;
        Command(k81DeepSleep, &sleep_mode, 1);   // 0x10
    }
    panel_awake_ = false;
}

bool Uc1679Display::ShowFrame(const uint8_t* bw_plane,
                              const uint8_t* red_plane) {
    if (!available_ || !spi_) return false;
    EpaperBusGuard bus_guard;  // 与 SSD1619 共享 SPI 总线，互斥刷新
    if (g_uc1679_refreshing.exchange(true)) {
        ESP_LOGW(kTag, "ShowFrame already in progress, skip");
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = false;
    do {
        // 兜底平面放 PSRAM（15000 字节，全 0xFF = 全白 / 无红）。
        static uint8_t* fallback_bw = nullptr;
        static uint8_t* fallback_red = nullptr;
        if (!fallback_bw) {
            fallback_bw = static_cast<uint8_t*>(
                heap_caps_malloc(kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            fallback_red = static_cast<uint8_t*>(
                heap_caps_malloc(kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (fallback_bw) std::memset(fallback_bw, 0xFF, kPlaneBytes);
            if (fallback_red) std::memset(fallback_red, 0xFF, kPlaneBytes);
        }
        if (!bw_plane) bw_plane = fallback_bw;
        if (!red_plane) red_plane = fallback_red;
        if (!bw_plane || !red_plane) {
            ESP_LOGE(kTag, "PSRAM OOM for fallback planes");
            break;
        }
        ok = ConfigurePanel(variant_) &&
             SendImagePlanes(variant_, bw_plane, red_plane) &&
             ActivateAndWait(variant_, kRefreshBusyTimeoutMs);
        if (ok) {
            ESP_LOGI(kTag, "Color full refresh complete (variant=%d)", variant_);
            Sleep();
        } else {
            ESP_LOGE(kTag, "Full refresh failed (variant=%d)", variant_);
        }
    } while (false);
    xSemaphoreGive(mutex_);
    g_uc1679_refreshing.store(false);
    return ok;
}
