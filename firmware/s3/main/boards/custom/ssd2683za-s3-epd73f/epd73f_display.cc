#include "epd73f_display.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "esp_lvgl_port.h"

// 聊天 UI 开关（编译期）。默认 0：屏幕内容只来自 InkSight 看板/清屏/色条，
// 与 SSD1619 板同为事件式无 UI 模式。置 1 启用 LcdDisplay 标准聊天界面。
#ifndef EPD73F_ENABLE_CHAT_UI
#define EPD73F_ENABLE_CHAT_UI 0
#endif

#if EPD73F_ENABLE_CHAT_UI
#include "assets/lang_config.h"
#endif

namespace {
constexpr char kTag[] = "Epd73fDisplay";

// UI 刷新去抖：LVGL 一次整页重构会拆成多条 strip flush，等它们全部落地。
constexpr uint32_t kUiDebounceMs = 3000;

// 7 色调色板（RGB 参考值，与 Waveshare 7.3" (F) 实测色接近；
// 量化为最近邻欧氏距离。若实机观感偏差大，在此调整参考值）。
struct PaletteEntry {
    uint8_t index;
    uint8_t r, g, b;
};
constexpr PaletteEntry kPalette[7] = {
    {Epd7in3fPanel::kColorBlack, 0, 0, 0},
    {Epd7in3fPanel::kColorWhite, 255, 255, 255},
    {Epd7in3fPanel::kColorGreen, 0, 140, 60},
    {Epd7in3fPanel::kColorBlue, 25, 60, 180},
    {Epd7in3fPanel::kColorRed, 205, 35, 35},
    {Epd7in3fPanel::kColorYellow, 220, 195, 20},
    {Epd7in3fPanel::kColorOrange, 230, 115, 15},
};
}  // namespace

uint8_t Epd73fDisplay::Quantize(uint8_t r5, uint8_t g6, uint8_t b5) {
    const int r = r5 * 255 / 31;
    const int g = g6 * 255 / 63;
    const int b = b5 * 255 / 31;
    const uint8_t* best = nullptr;
    int best_dist = 1 << 30;
    for (const auto& entry : kPalette) {
        const int dr = r - entry.r;
        const int dg = g - entry.g;
        const int db = b - entry.b;
        const int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = &entry.index;
        }
    }
    return best ? *best : Epd7in3fPanel::kColorWhite;
}

Epd73fDisplay::Epd73fDisplay()
    : LcdDisplay(nullptr, nullptr, DISPLAY_WIDTH, DISPLAY_HEIGHT) {
    mutex_ = xSemaphoreCreateMutex();
    color_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
        Epd7in3fPanel::kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    tx_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
        Epd7in3fPanel::kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    staging_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
        Epd7in3fPanel::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    staging_yellow_ = static_cast<uint8_t*>(heap_caps_malloc(
        Epd7in3fPanel::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    staging_red_ = static_cast<uint8_t*>(heap_caps_malloc(
        Epd7in3fPanel::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(mutex_ && color_frame_ && tx_frame_ && staging_bw_ &&
           staging_yellow_ && staging_red_);
    // 初始画面 = 全白（0x11 打包）。不在构造时物理刷新：面板保持旧画面，
    // 首帧内容（InkSight 看板）由 board 在网络就绪后推送。
    std::memset(color_frame_, 0x11, Epd7in3fPanel::kFrameBytes);
    std::memcpy(tx_frame_, color_frame_, Epd7in3fPanel::kFrameBytes);

    panel_.Init();

    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_lock(0);
    display_ = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_default(display_);
    lv_display_set_user_data(display_, this);
    lv_display_set_flush_cb(display_, FlushCallback);
    auto* lvgl_buffer = static_cast<uint8_t*>(
        heap_caps_malloc(kLvglBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(lvgl_buffer);
    lv_display_set_buffers(display_, lvgl_buffer, nullptr, kLvglBufferBytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lvgl_port_unlock();

    // 物理刷新全部走后台任务（35s 全刷串行化）。
    // 优先级 4（低于 AFE 内部任务的 5）：全刷是SPI 阻塞长任务，
    // 与 AFE 同在 core 1 时若同级/更高会把唤醒词引擎饿死，
    // 表现为 "AFE: Ringbuffer of AFE(FEED) is full" 持续刷屏。
    xTaskCreatePinnedToCore(RefreshTaskEntry, "epd73f_refresh", 4096, this, 4,
                            &refresh_task_, 1);
}

Epd73fDisplay::~Epd73fDisplay() {
    if (refresh_task_) vTaskDelete(refresh_task_);
    heap_caps_free(color_frame_);
    heap_caps_free(tx_frame_);
    heap_caps_free(staging_bw_);
    heap_caps_free(staging_yellow_);
    heap_caps_free(staging_red_);
    heap_caps_free(staging_frame_);
    if (mutex_) vSemaphoreDelete(mutex_);
}

// ---------------------------------------------------------------------------
// Display 接口。聊天 UI 开启时走 LcdDisplay 标准实现（内容量化为 7 色后
// 由刷新任务节流慢刷）；关闭时全部 no-op（Ssd1619Display 同款策略，
// 避免基类方法触碰从未创建的 LVGL 对象）。
// ---------------------------------------------------------------------------
#if EPD73F_ENABLE_CHAT_UI

void Epd73fDisplay::SetStatus(const char* status) {
    // LcdDisplay 层没有实现 SetStatus，状态栏在 LvglDisplay。
    LvglDisplay::SetStatus(status);
}

void Epd73fDisplay::SetEmotion(const char* emotion) {
    LcdDisplay::SetEmotion(emotion);
}

void Epd73fDisplay::SetChatMessage(const char* role, const char* content) {
    LcdDisplay::SetChatMessage(role, content);
}

void Epd73fDisplay::ShowActivationUi(const char* message, const char* code) {
    // Display 基类实现 = SetStatus + system 聊天气泡，落在 LcdDisplay UI 上。
    Display::ShowActivationUi(message, code);
}

void Epd73fDisplay::HideActivationUi() { Display::HideActivationUi(); }

void Epd73fDisplay::ClearChatMessages() { LcdDisplay::ClearChatMessages(); }

void Epd73fDisplay::SetupUI() { LcdDisplay::SetupUI(); }

void Epd73fDisplay::SetTheme(Theme* theme) { LcdDisplay::SetTheme(theme); }

void Epd73fDisplay::ShowNotification(const char* notification, int duration_ms) {
    LvglDisplay::ShowNotification(notification, duration_ms);
}

#else

void Epd73fDisplay::SetStatus(const char* status) {
    // 状态转换不打断看板画面，只进日志。
    if (status && status[0] != '\0') {
        ESP_LOGI(kTag, "Status: %s", status);
    }
}

void Epd73fDisplay::SetEmotion(const char* emotion) { (void)emotion; }

void Epd73fDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role;
    (void)content;
}

void Epd73fDisplay::ShowActivationUi(const char* message, const char* code) {
    ESP_LOGI(kTag, "Activation UI: %s (code=%s)", message ? message : "",
             code ? code : "");
}

void Epd73fDisplay::HideActivationUi() {}

void Epd73fDisplay::ClearChatMessages() {}

void Epd73fDisplay::SetupUI() {
    // 不创建 LcdDisplay 的动画 UI（时钟/状态条持续失效会触发无意义全刷，
    // 磨损七彩屏）。屏幕内容只来自 InkSight 看板与 MCP 工具。
    setup_ui_called_ = true;
}

void Epd73fDisplay::SetTheme(Theme* theme) { current_theme_ = theme; }

void Epd73fDisplay::ShowNotification(const char* notification, int duration_ms) {
    // 与 SSD1619 板一致：系统任务的瞬时 toast 只进日志（见 Ssd1619Display
    // 关于栈溢出的注释），持久画面由 InkSight 流程负责。
    if (notification && notification[0] != '\0') {
        ESP_LOGI(kTag, "Notification: %s", notification);
    }
    (void)duration_ms;
}

#endif  // EPD73F_ENABLE_CHAT_UI

// ---------------------------------------------------------------------------
// LVGL flush：RGB565 局部区域 -> 7 色打包帧。
// ---------------------------------------------------------------------------
void Epd73fDisplay::FlushCallback(lv_display_t* display, const lv_area_t* area,
                                  uint8_t* pixels) {
    auto* self = static_cast<Epd73fDisplay*>(lv_display_get_user_data(display));
    if (!self || !self->refresh_task_) {
        lv_display_flush_ready(display);
        return;
    }
    const auto* source = reinterpret_cast<const uint16_t*>(pixels);
    const int source_width = area->x2 - area->x1 + 1;
    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    for (int y = std::max(0, static_cast<int>(area->y1));
         y <= std::min(DISPLAY_HEIGHT - 1, static_cast<int>(area->y2)); ++y) {
        uint8_t* out_row =
            self->color_frame_ + static_cast<size_t>(y) * Epd7in3fPanel::kStride;
        for (int x = std::max(0, static_cast<int>(area->x1));
             x <= std::min(DISPLAY_WIDTH - 1, static_cast<int>(area->x2)); ++x) {
            const uint16_t color =
                source[(y - area->y1) * source_width + x - area->x1];
            const uint8_t index = Quantize((color >> 11) & 0x1f,
                                           (color >> 5) & 0x3f, color & 0x1f);
            const size_t byte_index = static_cast<size_t>(x) >> 1;
            if (x & 1) {
                out_row[byte_index] = static_cast<uint8_t>(
                    (out_row[byte_index] & 0xF0) | (index & 0x0F));
            } else {
                out_row[byte_index] = static_cast<uint8_t>(
                    (out_row[byte_index] & 0x0F) | (index << 4));
            }
        }
    }
    xSemaphoreGive(self->mutex_);
    lv_display_flush_ready(display);
#if EPD73F_ENABLE_CHAT_UI
    self->RequestUiRefresh();
#endif
}

void Epd73fDisplay::RequestUiRefresh() {
    if (!refresh_task_) return;
    ui_dirty_.store(true);
    PendingMode expected = PendingMode::kNone;
    if (pending_mode_.compare_exchange_strong(expected, PendingMode::kUi)) {
        xTaskNotifyGive(refresh_task_);
    }
}

bool Epd73fDisplay::ShowInkSightPlanes(const uint8_t* bw_plane,
                                       const uint8_t* yellow_plane,
                                       const uint8_t* red_plane) {
    if (!refresh_task_ || !bw_plane) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::memcpy(staging_bw_, bw_plane, Epd7in3fPanel::kPlaneBytes);
    has_yellow_ = yellow_plane != nullptr;
    has_red_ = red_plane != nullptr;
    if (has_yellow_) {
        std::memcpy(staging_yellow_, yellow_plane, Epd7in3fPanel::kPlaneBytes);
    }
    if (has_red_) {
        std::memcpy(staging_red_, red_plane, Epd7in3fPanel::kPlaneBytes);
    }
    xSemaphoreGive(mutex_);
    pending_mode_.store(PendingMode::kInkSight);
    pending_force_.store(true);
    xTaskNotifyGive(refresh_task_);
    return true;
}

bool Epd73fDisplay::ShowInkSightFrame(const uint8_t* norm_frame) {
    if (!refresh_task_ || !norm_frame) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!staging_frame_) {
        staging_frame_ = static_cast<uint8_t*>(
            heap_caps_malloc(Epd7in3fPanel::kFrameBytes,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    const bool allocated = staging_frame_ != nullptr;
    if (allocated) {
        std::memcpy(staging_frame_, norm_frame, Epd7in3fPanel::kFrameBytes);
    }
    xSemaphoreGive(mutex_);
    if (!allocated) {
        ESP_LOGE(kTag, "PSRAM OOM for 4bpp staging frame");
        return false;
    }
    pending_mode_.store(PendingMode::kInkSightFrame);
    pending_force_.store(true);
    xTaskNotifyGive(refresh_task_);
    return true;
}

bool Epd73fDisplay::ClearScreen(uint8_t color_index) {
    if (!refresh_task_) return false;
    // 借用 UI 帧通道：清屏就是"全白（或指定色）的 UI 帧 + 强制刷"。
    const uint8_t pair = static_cast<uint8_t>(
        (color_index << 4) | (color_index & 0x0F));
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::memset(color_frame_, pair, Epd7in3fPanel::kFrameBytes);
    xSemaphoreGive(mutex_);
    pending_mode_.store(PendingMode::kClear);
    pending_force_.store(true);
    xTaskNotifyGive(refresh_task_);
    return true;
}

bool Epd73fDisplay::ShowColorBars() {
    if (!refresh_task_) return false;
    pending_mode_.store(PendingMode::kColorBars);
    pending_force_.store(true);
    xTaskNotifyGive(refresh_task_);
    return true;
}

void Epd73fDisplay::RefreshTaskEntry(void* arg) {
    static_cast<Epd73fDisplay*>(arg)->RefreshTask();
}

bool Epd73fDisplay::WaitForRefreshComplete(uint32_t timeout_ms) const {
    const int64_t deadline = esp_timer_get_time() +
                             static_cast<int64_t>(timeout_ms) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (pending_mode_.load() == PendingMode::kNone &&
            !refresh_in_progress_.load() && !IsPanelBusy()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

void Epd73fDisplay::RefreshTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const PendingMode mode = pending_mode_.load();

        if (mode == PendingMode::kInkSight) {
            refresh_in_progress_.store(true);
            pending_mode_.store(PendingMode::kNone);
            xSemaphoreTake(mutex_, portMAX_DELAY);
            const uint8_t* yellow = has_yellow_ ? staging_yellow_ : nullptr;
            const uint8_t* red = has_red_ ? staging_red_ : nullptr;
            xSemaphoreGive(mutex_);
            // ShowPlanes 内部完成 2bpp -> 7 色映射 + 全刷（阻塞 ~35s），
            // 并把结果同步到 tx_frame_（通过其内部 packed 缓冲）。这里把
            // color_frame_/tx_frame_ 保持为白底，避免 UI 帧误判无变化。
            if (panel_.ShowPlanes(staging_bw_, yellow, red)) {
                ESP_LOGI(kTag, "XiaowuAI 7-color frame refreshed");
            } else {
                ESP_LOGW(kTag, "XiaowuAI 7-color refresh failed");
            }
            last_ui_refresh_ms_ = esp_timer_get_time() / 1000;
            refresh_in_progress_.store(false);
            continue;
        }

        if (mode == PendingMode::kInkSightFrame) {
            refresh_in_progress_.store(true);
            pending_mode_.store(PendingMode::kNone);
            xSemaphoreTake(mutex_, portMAX_DELAY);
            const uint8_t* frame = staging_frame_;
            xSemaphoreGive(mutex_);
            if (!frame) {
                ESP_LOGW(kTag, "4bpp frame missing, skip refresh");
                refresh_in_progress_.store(false);
                continue;
            }
            // ShowNormalizedFrame 内部把规范索引换算成面板索引 + 全刷
            // （六色约 12s，七彩约 35s，均为阻塞）。
            if (panel_.ShowNormalizedFrame(frame)) {
                ESP_LOGI(kTag, "XiaowuAI 6-color (4bpp) frame refreshed");
            } else {
                ESP_LOGW(kTag, "XiaowuAI 6-color (4bpp) refresh failed");
            }
            last_ui_refresh_ms_ = esp_timer_get_time() / 1000;
            refresh_in_progress_.store(false);
            continue;
        }

        if (mode == PendingMode::kClear) {
            refresh_in_progress_.store(true);
            pending_mode_.store(PendingMode::kNone);
            xSemaphoreTake(mutex_, portMAX_DELAY);
            std::memcpy(tx_frame_, color_frame_, Epd7in3fPanel::kFrameBytes);
            xSemaphoreGive(mutex_);
            if (!panel_.ShowFrame(tx_frame_)) {
                ESP_LOGW(kTag, "Clear refresh failed");
            }
            last_ui_refresh_ms_ = esp_timer_get_time() / 1000;
            refresh_in_progress_.store(false);
            continue;
        }

        if (mode == PendingMode::kColorBars) {
            refresh_in_progress_.store(true);
            pending_mode_.store(PendingMode::kNone);
            if (panel_.ShowColorBars()) {
                std::memset(tx_frame_, 0x11, Epd7in3fPanel::kFrameBytes);
            } else {
                ESP_LOGW(kTag, "Color bars refresh failed");
            }
            last_ui_refresh_ms_ = esp_timer_get_time() / 1000;
            refresh_in_progress_.store(false);
            continue;
        }

        // ---- kUi：LVGL 界面帧（去抖 + 节流 + 变化检测）----
        vTaskDelay(pdMS_TO_TICKS(kUiDebounceMs));
        if (pending_mode_.load() != PendingMode::kUi) {
            continue;  // 去抖期间来了更高优先级请求，下一轮处理
        }
        pending_mode_.store(PendingMode::kNone);
        if (!ui_dirty_.exchange(false)) {
            continue;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_ui_refresh_ms_ <
            static_cast<int64_t>(EPD73F_UI_REFRESH_MIN_INTERVAL_MS)) {
            ESP_LOGD(kTag, "UI refresh throttled");
            continue;
        }
        bool should_refresh = false;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (std::memcmp(tx_frame_, color_frame_, Epd7in3fPanel::kFrameBytes) != 0) {
            std::memcpy(tx_frame_, color_frame_, Epd7in3fPanel::kFrameBytes);
            should_refresh = true;
        }
        xSemaphoreGive(mutex_);
        if (should_refresh && panel_.ShowFrame(tx_frame_)) {
            last_ui_refresh_ms_ = esp_timer_get_time() / 1000;
            ESP_LOGI(kTag, "UI frame refreshed (7-color full refresh)");
        }
    }
}
