#include "uc8179_panel.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "epaper_bus_lock.h"

namespace {
// SPI 单次传输上限。总线由别的组件初始化时 max_transfer_sz 可能只有 4 KB，
// 这里按 2 KB 分块，任何配置下都不会被截断。
constexpr size_t kChunkBytes = 2048;
constexpr uint32_t kBusyAssertWindowMs = 1500;
}  // namespace

Uc8179Panel::Uc8179Panel(gpio_num_t cs_pin, gpio_num_t busy_pin, Mode mode)
    : cs_pin_(cs_pin), busy_pin_(busy_pin), mode_(mode) {}

Uc8179Panel::~Uc8179Panel() {
    if (spi_ != nullptr) {
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
    }
    if (chunk_ != nullptr) {
        heap_caps_free(chunk_);
        chunk_ = nullptr;
    }
}

bool Uc8179Panel::Begin(const char* tag) {
    if (tag != nullptr) tag_ = tag;

    // 两个座共用 DIN/CLK/DC/RST，只有 CS 与 BUSY 独立。
    gpio_config_t out = {};
    out.pin_bit_mask =
        (1ULL << cs_pin_) | (1ULL << EPD_DC_PIN) | (1ULL << EPD_RST_PIN);
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_DISABLE;
    out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&out) != ESP_OK) {
        ESP_LOGE(tag_, "GPIO 配置失败（CS=GPIO%d）", cs_pin_);
        return false;
    }
    gpio_set_level(cs_pin_, 1);
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(EPD_RST_PIN, 1);

    gpio_config_t in = {};
    in.pin_bit_mask = (1ULL << busy_pin_);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = EPD_MOSI_PIN;
    bus.miso_io_num = -1;
    bus.sclk_io_num = EPD_SCK_PIN;
    bus.max_transfer_sz = static_cast<int>(kChunkBytes + 8);
    const esp_err_t bus_err = spi_bus_initialize(EPD_SPI_NUM, &bus, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(tag_, "SPI 总线初始化失败: %s", esp_err_to_name(bus_err));
        return false;
    }

    spi_device_interface_config_t dev = {};
    // 4 MHz：与旧驱动同速，排线较长时更稳。一帧 38 KB 约 78 ms，可接受。
    dev.clock_speed_hz = 4 * 1000 * 1000;
    dev.mode = 0;
    // CS 由 GPIO 手动驱动（两个座共用一条总线，且 CS 是普通 GPIO）。
    dev.spics_io_num = -1;
    dev.queue_size = 2;
    const esp_err_t dev_err = spi_bus_add_device(EPD_SPI_NUM, &dev, &spi_);
    if (dev_err != ESP_OK) {
        ESP_LOGE(tag_, "SPI 设备挂载失败: %s", esp_err_to_name(dev_err));
        return false;
    }

    chunk_ = static_cast<uint8_t*>(
        heap_caps_malloc(kChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (chunk_ == nullptr) {
        ESP_LOGE(tag_, "DMA 缓冲分配失败");
        return false;
    }

    ESP_LOGI(tag_,
             "UC8179 %s %dx%d，CS=GPIO%d BUSY=GPIO%d（共用 DIN=GPIO%d CLK=GPIO%d "
             "DC=GPIO%d RST=GPIO%d）",
             mode_ == Mode::kTriColor ? "三色" : "黑白", kWidth, kHeight, cs_pin_,
             busy_pin_, EPD_MOSI_PIN, EPD_SCK_PIN, EPD_DC_PIN, EPD_RST_PIN);
    return true;
}

void Uc8179Panel::Command(uint8_t command) {
    gpio_set_level(EPD_DC_PIN, 0);
    gpio_set_level(cs_pin_, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &command;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(cs_pin_, 1);
    gpio_set_level(EPD_DC_PIN, 1);
}

void Uc8179Panel::Command(uint8_t command, const uint8_t* data, size_t length) {
    Command(command);
    if (data != nullptr && length > 0) Data(data, length);
}

void Uc8179Panel::Data(const uint8_t* data, size_t length) {
    DataPlane(data, length, /*invert=*/false);
}

void Uc8179Panel::DataPlane(const uint8_t* data, size_t length, bool invert) {
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(cs_pin_, 0);
    size_t offset = 0;
    while (offset < length) {
        const size_t chunk = (length - offset) > kChunkBytes ? kChunkBytes
                                                             : (length - offset);
        const uint8_t* src = data + offset;
        if (invert) {
            for (size_t i = 0; i < chunk; ++i) {
                chunk_[i] = static_cast<uint8_t>(~src[i]);
            }
            src = chunk_;
        }
        spi_transaction_t t = {};
        t.length = chunk * 8;
        t.tx_buffer = src;
        spi_device_polling_transmit(spi_, &t);
        offset += chunk;
    }
    gpio_set_level(cs_pin_, 1);
}

void Uc8179Panel::Reset() {
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

int Uc8179Panel::DetectIdleLevel() {
    // UC8179 常态是「高=空闲、低=忙」，但见过把 BUSY 反相折到 GPIO 的模组，
    // 所以不写死：取复位后稳定下来的电平当空闲。
    int last = gpio_get_level(busy_pin_);
    int stable = 0;
    for (int i = 0; i < 300; ++i) {  // 最多 3 s
        const int level = gpio_get_level(busy_pin_);
        if (level == last) {
            if (++stable >= 20) break;  // 连续 200 ms 不变即认为稳定
        } else {
            last = level;
            stable = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return last;
}

bool Uc8179Panel::ProbeBusyDriven() {
    // 面板驱动着 BUSY 时，弱上下拉都拉不动它；浮空脚会跟着上下拉翻转。
    gpio_set_pull_mode(busy_pin_, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int low = gpio_get_level(busy_pin_);
    gpio_set_pull_mode(busy_pin_, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int high = gpio_get_level(busy_pin_);
    return low == high;
}

bool Uc8179Panel::WaitIdle(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (gpio_get_level(busy_pin_) == idle_level_) return true;
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool Uc8179Panel::Init() {
    if (spi_ == nullptr) return false;
    Reset();
    idle_level_ = DetectIdleLevel();
    busy_driven_ = ProbeBusyDriven();
    ESP_LOGI(tag_, "复位后 BUSY=%d → 空闲电平=%d，线路被面板驱动=%s", idle_level_,
             idle_level_, busy_driven_ ? "是" : "否（浮空，可能没插好）");

    if (mode_ == Mode::kTriColor) {
        // 官方 EPD_5in83b_V2_Init()：先电源设置，再 PON，再 PSR/TRES/VCOM/TCON。
        const uint8_t power[] = {0x07, 0x07, 0x3f, 0x3f};  // VGH=20V VGL=-20V VDH/VDL=15V
        Command(0x01, power, sizeof(power));
        Command(0x04);  // POWER ON
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!WaitIdle(8000)) {
            ESP_LOGW(tag_, "0x04(PON) 后 BUSY 未回空闲（当前=%d）",
                     gpio_get_level(busy_pin_));
        }
        const uint8_t psr = 0x0f;  // BWROTP 0f：走面板内置三色波形
        Command(0x00, &psr, 1);
        const uint8_t tres[] = {0x02, 0x88, 0x01, 0xe0};  // 648 x 480
        Command(0x61, tres, sizeof(tres));
        const uint8_t cdi_vcom = 0x00;
        Command(0x15, &cdi_vcom, 1);
        const uint8_t cdi[] = {0x11, 0x07};
        Command(0x50, cdi, sizeof(cdi));
        const uint8_t tcon = 0x22;
        Command(0x60, &tcon, 1);
    } else {
        // 官方 EPD_5in83_V2_Init()：PSR -> VCOM/数据间隔 -> PON。
        const uint8_t psr = 0x1f;
        Command(0x00, &psr, 1);
        const uint8_t tres[] = {0x02, 0x88, 0x01, 0xe0};  // 648 x 480
        Command(0x61, tres, sizeof(tres));
        const uint8_t cdi[] = {0x21, 0x07};
        Command(0x50, cdi, sizeof(cdi));
        Command(0x04);  // POWER ON
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!WaitIdle(8000)) {
            ESP_LOGW(tag_, "0x04(PON) 后 BUSY 未回空闲（当前=%d）",
                     gpio_get_level(busy_pin_));
        }
    }

    initialized_ = true;
    ESP_LOGI(tag_, "UC8179 初始化完成（%s，内置 OTP 波形，不写 LUT）",
             mode_ == Mode::kTriColor ? "三色" : "黑白");
    return true;
}

bool Uc8179Panel::ActivateAndWait(uint32_t fallback_ms) {
    const TickType_t t_start = xTaskGetTickCount();
    Command(0x12);  // DISPLAY REFRESH
    vTaskDelay(pdMS_TO_TICKS(100));  // 手册要求 ≥200us

    // 先确认面板真的拉忙了。拉不忙说明命令没被接受（CS/BUSY 选错座）或
    // BUSY 没接出来——这两种情况都必须在日志里说清楚，而不是静默等超时。
    bool asserted = false;
    uint32_t wait_ms = 0;
    while (wait_ms < kBusyAssertWindowMs) {
        if (gpio_get_level(busy_pin_) != idle_level_) {
            asserted = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        wait_ms += 5;
    }
    last_assert_ms_ = wait_ms;

    if (!asserted) {
        ESP_LOGW(tag_,
                 "0x12 后 %u ms 内 BUSY 未拉忙（当前=%d idle=%d 驱动=%s）→ 无法确认"
                 "面板是否执行；按 %u ms 定时等待",
                 static_cast<unsigned>(kBusyAssertWindowMs),
                 gpio_get_level(busy_pin_), idle_level_,
                 busy_driven_ ? "是" : "否", static_cast<unsigned>(fallback_ms));
        vTaskDelay(pdMS_TO_TICKS(fallback_ms));
        last_busy_ms_ = fallback_ms;
        return false;
    }

    while (gpio_get_level(busy_pin_) != idle_level_) {
        if ((xTaskGetTickCount() - t_start) * portTICK_PERIOD_MS >
            fallback_ms + kBusyAssertWindowMs) {
            ESP_LOGW(tag_, "波形超时（>%u ms）仍未回空闲，当前=%d",
                     static_cast<unsigned>(fallback_ms),
                     gpio_get_level(busy_pin_));
            last_busy_ms_ =
                static_cast<uint32_t>((xTaskGetTickCount() - t_start) *
                                      portTICK_PERIOD_MS);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    last_busy_ms_ = static_cast<uint32_t>(
        (xTaskGetTickCount() - t_start) * portTICK_PERIOD_MS);
    ESP_LOGI(tag_, "波形实跑：拉忙延迟 %u ms，忙态总时长 %u ms",
             static_cast<unsigned>(last_assert_ms_),
             static_cast<unsigned>(last_busy_ms_));
    return true;
}

bool Uc8179Panel::Show(const uint8_t* bw, const uint8_t* red) {
    if (bw == nullptr) return false;
    if (spi_ == nullptr && !Begin(tag_)) return false;
    if (!initialized_ && !Init()) return false;

    EpaperBusGuard bus_guard;

    // 0x10 = 旧/黑白平面。UC8179 的 0x10 参与差分，必须写当前画面；
    // 0x13 = 新/红平面：黑白屏写同一份数据（官方例程如此），三色屏写红色。
    Command(0x10);
    DataPlane(bw, kPlaneBytes, /*invert=*/false);
    Command(0x13);
    if (mode_ == Mode::kTriColor) {
        if (red != nullptr) {
            DataPlane(red, kPlaneBytes, red_invert_);
        } else {
            // 没有红色内容：填"非红"。1=红时填 0x00；0=红时填 0xFF。
            static const uint8_t kZero[kChunkBytes] = {0};
            static const uint8_t kOnes[kChunkBytes] = {0xff};
            const uint8_t* blank = red_invert_ ? kOnes : kZero;
            size_t left = kPlaneBytes;
            while (left > 0) {
                const size_t n = left > kChunkBytes ? kChunkBytes : left;
                DataPlane(blank, n, /*invert=*/false);
                left -= n;
            }
        }
    } else {
        DataPlane(bw, kPlaneBytes, /*invert=*/false);
    }

    const uint32_t fallback =
        (mode_ == Mode::kTriColor) ? 26000u : 8000u;
    return ActivateAndWait(fallback);
}

void Uc8179Panel::Sleep() {
    if (spi_ == nullptr || !initialized_) return;
    EpaperBusGuard bus_guard;
    const uint8_t cdi = 0xf7;
    Command(0x50, &cdi, 1);
    Command(0x02);  // POWER OFF
    WaitIdle(5000);
    Command(0x07);  // DEEP SLEEP
    const uint8_t magic = 0xa5;
    Data(&magic, 1);
    initialized_ = false;
}
