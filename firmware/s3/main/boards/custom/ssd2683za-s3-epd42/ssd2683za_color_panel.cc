#include "ssd2683za_color_panel.h"

#include <algorithm>
#include <cstring>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

#include "epaper_bus_lock.h"

namespace {
constexpr char kTag[] = "Ssd2683zaFpc2";
constexpr uint32_t kBusyTimeoutMs = 45000;
}

std::atomic<bool> Ssd2683zaColorPanel::refreshing_{false};

Ssd2683zaColorPanel::Ssd2683zaColorPanel(
    std::function<bool()> other_panel_busy)
    : other_panel_busy_(std::move(other_panel_busy)) {
    mutex_ = xSemaphoreCreateMutex();
    dma_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(
        kDmaChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!mutex_ || !dma_buffer_) {
        ESP_LOGE(kTag, "Failed to allocate driver resources");
        return;
    }

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

    // FPC1 initialized the shared SPI3 bus first. Only add a second device;
    // hardware CS is manual because the two sockets have independent CS pins.
    spi_device_interface_config_t device = {};
    device.clock_speed_hz = 4 * 1000 * 1000;
    device.mode = 0;
    device.spics_io_num = -1;
    device.queue_size = 2;
    const esp_err_t err = spi_bus_add_device(EPD_SPI_NUM, &device, &spi_);
    if (err != ESP_OK) {
        spi_ = nullptr;
        ESP_LOGE(kTag, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    }
}

Ssd2683zaColorPanel::~Ssd2683zaColorPanel() {
    if (spi_) spi_bus_remove_device(spi_);
    if (mutex_) vSemaphoreDelete(mutex_);
    if (dma_buffer_) heap_caps_free(dma_buffer_);
}

void Ssd2683zaColorPanel::WaitForOtherPanelIdle() {
    if (!other_panel_busy_ || !other_panel_busy_()) return;
    ESP_LOGI(kTag, "Shared RST: waiting for FPC1 refresh");
    const TickType_t start = xTaskGetTickCount();
    while (other_panel_busy_() &&
           (xTaskGetTickCount() - start) * portTICK_PERIOD_MS < 30000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void Ssd2683zaColorPanel::HardwareReset() {
    WaitForOtherPanelIdle();
    // Match the SSD2683ZA/R Y683 reference reset pulse (active low).
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool Ssd2683zaColorPanel::WaitBusy(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD_COLOR_BUSY_PIN) == busy_active_level_) {
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            ESP_LOGE(kTag, "BUSY timeout after %lu ms (GPIO%d=%d)",
                     static_cast<unsigned long>(timeout_ms), EPD_COLOR_BUSY_PIN,
                     gpio_get_level(EPD_COLOR_BUSY_PIN));
            return false;
        }
        // pdMS_TO_TICKS(2) can round down to zero on configurations with a
        // 100 Hz FreeRTOS tick. A zero-tick delay is only a yield hint on some
        // ports and can leave CPU0's IDLE task starved while the panel holds
        // BUSY for its long waveform, triggering the task watchdog. Always
        // block for at least one scheduler tick while polling the pin.
        TickType_t poll_ticks = pdMS_TO_TICKS(10);
        if (poll_ticks == 0) poll_ticks = 1;
        vTaskDelay(poll_ticks);
    }
    return true;
}

void Ssd2683zaColorPanel::SendCommandRaw(uint8_t command) {
    gpio_set_level(EPD_DC_PIN, 0);
    spi_transaction_t transaction = {};
    transaction.length = 8;
    transaction.tx_buffer = &command;
    const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SPI command 0x%02X failed: %s", command,
                 esp_err_to_name(err));
    }
}

void Ssd2683zaColorPanel::SendDataRaw(const uint8_t* data, size_t length) {
    gpio_set_level(EPD_DC_PIN, 1);
    while (length > 0) {
        const size_t chunk = std::min(length, kDmaChunkBytes);
        std::memcpy(dma_buffer_, data, chunk);
        spi_transaction_t transaction = {};
        transaction.length = chunk * 8;
        transaction.tx_buffer = dma_buffer_;
        const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "SPI data transfer failed: %s", esp_err_to_name(err));
            break;
        }
        data += chunk;
        length -= chunk;
    }
}

void Ssd2683zaColorPanel::SendCommandOnly(uint8_t command) {
    gpio_set_level(EPD_COLOR_CS_PIN, 0);
    SendCommandRaw(command);
    gpio_set_level(EPD_COLOR_CS_PIN, 1);
}

void Ssd2683zaColorPanel::SendCommandWithByte(uint8_t command, uint8_t value) {
    gpio_set_level(EPD_COLOR_CS_PIN, 0);
    SendCommandRaw(command);
    SendDataRaw(&value, 1);
    gpio_set_level(EPD_COLOR_CS_PIN, 1);
}

void Ssd2683zaColorPanel::SendCommandWithBuffer(uint8_t command,
                                                const uint8_t* data,
                                                size_t length) {
    gpio_set_level(EPD_COLOR_CS_PIN, 0);
    SendCommandRaw(command);
    SendDataRaw(data, length);
    gpio_set_level(EPD_COLOR_CS_PIN, 1);
}

void Ssd2683zaColorPanel::SendDataStream(const uint8_t* data, size_t length) {
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(EPD_COLOR_CS_PIN, 0);
    SendDataRaw(data, length);
    gpio_set_level(EPD_COLOR_CS_PIN, 1);
}

bool Ssd2683zaColorPanel::InitializePanel() {
    if (!spi_) return false;
    HardwareReset();
    // 复位后模组进入确定态，此时读到的 BUSY 即为「空闲电平」，取反即得有效电平。
    // 比复位前读更可靠：上电瞬间 BUSY 引脚可能浮空/抖动，单次读数会把高电平误判
    // 为空闲而选错极性（本板 Y683 实测「忙=高、空闲=低」，与参考驱动相反），
    // 选反后 WaitBusy 进去就死等超时，整块屏被判为不存在。复位后短延 + 连读去抖
    // 取稳定值，彻底消除该误判。
    vTaskDelay(pdMS_TO_TICKS(30));
    int idle_level = gpio_get_level(EPD_COLOR_BUSY_PIN);
    for (int i = 0; i < 3; ++i) {
        vTaskDelay(pdMS_TO_TICKS(8));
        const int now = gpio_get_level(EPD_COLOR_BUSY_PIN);
        if (now != idle_level) idle_level = now;  // 以最近稳定值为准
    }
    busy_active_level_ = idle_level == 0 ? 1 : 0;
    ESP_LOGI(kTag, "FPC2 post-reset BUSY(GPIO%d)=%d → BUSY 有效电平=%d%s",
             EPD_COLOR_BUSY_PIN, idle_level, busy_active_level_,
             busy_active_level_ == kBusyActiveLevel ? "" : " (与参考驱动相反，已自适应)");
    if (!WaitBusy(5000)) return false;

    // SSD2683ZA 4-color sequence, matching the known-good SSD2683ZA alias in
    // firmware/src/epd_driver.cpp (EPD_PANEL_42_DKE_RY683). In particular,
    // 0x11 is NOT a sleep-exit command here; it is the RAM data-entry mode.
    constexpr uint8_t booster[] = {0x0F, 0x8B, 0x9C, 0x96};
    SendCommandWithBuffer(0x06, booster, sizeof(booster));
    constexpr uint8_t panel_setting[] = {0x2F, 0x69};
    SendCommandWithBuffer(0x00, panel_setting, sizeof(panel_setting));
    if (!WaitBusy(5000)) return false;

    constexpr uint8_t power_setting[] = {0x07, 0xF0};
    SendCommandWithBuffer(0x01, power_setting, sizeof(power_setting));
    SendCommandWithByte(0x50, 0x37);
#if FPC2_WIDTH == 648
    // 5.83-inch (648x480) panel resolution. The SSD2683ZA controller itself
    // drives larger panels (it also runs the 7.3-inch board), so 0x61 just
    // needs the new dimensions. The 0x62 power waveform and 0x00 panel setting
    // are panel-specific and may need tuning for a particular 5.83" module — TBD.
    constexpr uint8_t resolution[] = {0x02, 0x88, 0x01, 0xE0};
#else
    // 4.2-inch (400x300) panel resolution (default).
    constexpr uint8_t resolution[] = {0x01, 0x90, 0x01, 0x2C};
#endif
    SendCommandWithBuffer(0x61, resolution, sizeof(resolution));
    constexpr uint8_t power_waveform[] = {0x64, 0x53};
    SendCommandWithBuffer(0x62, power_waveform, sizeof(power_waveform));
    constexpr uint8_t scan_setting[] = {0x00, 0x00, 0x00, 0x00};
    SendCommandWithBuffer(0x65, scan_setting, sizeof(scan_setting));
    SendCommandWithByte(0x30, 0x08);
    SendCommandWithByte(0xE9, 0x01);
    ESP_LOGI(kTag, "SSD2683ZA initialized: %dx%d BWRY, BUSY active=%d, FPC2 CS=GPIO%d BUSY=GPIO%d",
             FPC2_WIDTH, FPC2_HEIGHT, busy_active_level_,
             EPD_COLOR_CS_PIN, EPD_COLOR_BUSY_PIN);
    return true;
}

void Ssd2683zaColorPanel::SleepPanel() {
    SendCommandWithByte(0x02, 0x00);  // Power off after refresh.
    if (!WaitBusy(10000)) return;
    panel_awake_ = false;
}

bool Ssd2683zaColorPanel::Init() {
    if (!spi_ || !mutex_ || !dma_buffer_) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = InitializePanel();
    if (ok) {
        SendCommandOnly(0x04);  // Power on.
        vTaskDelay(pdMS_TO_TICKS(100));
        ok = gpio_get_level(EPD_COLOR_BUSY_PIN) == busy_active_level_;
        if (ok) ok = WaitBusy(10000);
        else {
            ESP_LOGW(kTag,
                     "FPC2 panel did not assert BUSY after PON (GPIO%d=%d)",
                     EPD_COLOR_BUSY_PIN,
                     gpio_get_level(EPD_COLOR_BUSY_PIN));
        }
        if (ok) {
            panel_awake_ = true;
            SleepPanel();
        }
    }
    available_ = ok;
    ESP_LOGI(kTag, "FPC2 probe %s (CS=%d BUSY=%d level=%d)",
             available_ ? "passed" : "failed", EPD_COLOR_CS_PIN,
             EPD_COLOR_BUSY_PIN, gpio_get_level(EPD_COLOR_BUSY_PIN));
    xSemaphoreGive(mutex_);
    return available_;
}

bool Ssd2683zaColorPanel::ShowFrame(const uint8_t* packed_frame) {
    if (!available_ || !spi_ || !packed_frame) return false;
    if (refreshing_.exchange(true)) {
        ESP_LOGW(kTag, "FPC2 refresh already in progress");
        return false;
    }
    // 与 FPC1(SSD1619) 共享 SPI 总线/DC/RST：刷新任务已移出主循环，
    // 两块面板可能并发刷新，全程持锁互斥（含 BUSY 等待）。
    EpaperBusGuard bus_guard;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = InitializePanel();
    if (ok) {
        // SSD2683ZA uses one packed 2bpp B/W/Y/R RAM plane (30,000 bytes).
        SendCommandOnly(0x10);
        if (WaitBusy(5000)) {
            SendDataStream(packed_frame, kFrameBytes);
            SendCommandOnly(0x04);
            // A connected SSD2683ZA asserts BUSY after PON. The reference
            // implementation checks for this response before waiting for the
            // line to return to idle; a floating/unconnected FPC2 otherwise
            // looks like an instant successful refresh.
            vTaskDelay(pdMS_TO_TICKS(100));
            ok = gpio_get_level(EPD_COLOR_BUSY_PIN) == busy_active_level_;
            if (!ok) {
                ESP_LOGE(kTag,
                         "SSD2683ZA did not assert BUSY after PON (GPIO%d=%d); check FPC2 seating/power",
                         EPD_COLOR_BUSY_PIN,
                         gpio_get_level(EPD_COLOR_BUSY_PIN));
            } else {
                ok = WaitBusy(10000);
            }
            if (ok) {
                panel_awake_ = true;
                SendCommandWithByte(0x12, 0x00);
                ESP_LOGI(kTag, "FPC2 SSD2683ZA refresh started (%u bytes BWRY)",
                         static_cast<unsigned>(kFrameBytes));
                ok = WaitBusy(kBusyTimeoutMs);
                if (ok) ESP_LOGI(kTag, "FPC2 SSD2683ZA refresh complete");
            }
        } else {
            ok = false;
        }
    }
    if (panel_awake_) SleepPanel();
    xSemaphoreGive(mutex_);
    refreshing_.store(false);
    return ok;
}
