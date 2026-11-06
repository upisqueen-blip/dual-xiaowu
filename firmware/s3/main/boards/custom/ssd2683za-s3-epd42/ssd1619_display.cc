#include "ssd1619_display.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "esp_lvgl_port.h"
#include "epaper_bus_lock.h"

#if EPD_ENABLE_COLOR_SCREEN || FPC1_COLOR_COUNT == 3
#include "uc1679_display.h"
#endif

#if EPD_ENABLE_COLOR_SCREEN
#include "ssd2683za_color_panel.h"
#endif

#if !CONFIG_EPD_PANEL_UC8253_37

namespace {
constexpr char kTag[] = "Ssd1619Display";
constexpr size_t kLvglBufferBytes = DISPLAY_WIDTH * 24 * sizeof(uint16_t);
constexpr size_t kDmaChunkBytes = 4096;
// 面板故障（BUSY 超时 / SPI 失败）后的自愈间隔。旧实现把 panel_faulted_ 当成
// 终身判决：一次偶发的 BUSY 采样抖动就会让屏幕从此再也不刷新，用户侧的表现
// 就是"长按 OK 返回不了首页"（状态和按键其实都正常，只是像素刷不出去），
// 只能断电重启。
constexpr int64_t kPanelFaultRetryMs = 700;
// 硬件复位后观察 BUSY 是否有电平跳变的窗口。复位一定会让面板拉 BUSY 再放
// 开，所以窗口内没有跳变即可断定面板没有驱动这条线。
constexpr uint32_t kBusyActivityWindowMs = 600;
// 线都死了还每 700 ms 复位重试一次只会刷出一屏 EMBED endless 日志并空转
// CPU，把退避拉长到一个 meaningful 的量级。
constexpr int64_t kPanelStuckRetryMs = 30000;
// 发出 0x20（Master Activation）后，等 BUSY 离开空闲电平的宽限时间。
// 原值 500ms 太激进：刷新任务被音频/LVGL 抢占时，轮询本身就会迟到几百毫秒，
// 于是面板明明在正常工作却被判成"没响应"并错误锁存。一次全刷的 BUSY 建立
// 时间在几十毫秒量级，3s 足够宽松又不会明显拖慢故障上报。
constexpr uint32_t kBusyAssertTimeoutMs = 3000;

// SSD1619 partial waveform — 2 驱动相位，比原版 1 相位强得多，
// 墨水粒子被充分翻转，残影大幅减少同时不会闪烁。
// LUT0/LUT3 (unchanged) = VSS 全程，不变像素不驱动不闪烁。
// LUT1 (B→W): VSH→VSS 两相位，黑粒子翻到白侧
// LUT2 (W→B): VSL→VSS 两相位，白粒子翻到黑侧
// 0x32 寄存器正好 70 字节（5×7 LUT + 7×5 timing + VCOM）。
constexpr uint8_t kPartialLut[70] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT0: unchanged black
    0xa0,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT1: B→W (phase1=VSH, phase2=VSS)
    0x50,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT2: W→B (phase1=VSL, phase2=VSS)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT3: unchanged white
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // LUT4: VCOM
    // group timing (7 groups × 5 bytes each)
    0x0a,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
};
}

Ssd1619Display::Ssd1619Display()
    : LcdDisplay(nullptr, nullptr, DISPLAY_WIDTH, DISPLAY_HEIGHT) {
    // 全刷 0x26 旧平面的写法，由 config.h 的 EPD_INVERT_OLD_PLANE 决定。
    invert_old_plane_ = (EPD_INVERT_OLD_PLANE != 0);
#if FPC1_COLOR_COUNT != 3
    InitializeSpi();
#endif
#if FPC1_COLOR_COUNT == 3
    static_assert(FPC1_WIDTH == FPC2_WIDTH && FPC1_HEIGHT == FPC2_HEIGHT,
                  "FPC1 and FPC2 three-color drivers currently require equal panel sizes");
    fpc1_color_driver_ =
        new Uc1679Display(EPD_CS_PIN_FPC1, EPD_BUSY_PIN_FPC1);
#else
    InitializeSpi();
#endif
    mutex_ = xSemaphoreCreateMutex();
    frame_ = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    tx_frame_ = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    panel_frame_ = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    dma_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(
        kDmaChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    assert(mutex_ && frame_ && tx_frame_ && panel_frame_ && dma_buffer_);
    std::memset(frame_, 0xff, kFrameBytes);  // SSD1619: 1=white, 0=black.
    std::memcpy(tx_frame_, frame_, kFrameBytes);
    std::memcpy(panel_frame_, frame_, kFrameBytes);

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

    // 优先级 3 -> 6：BUSY 轮询是按 1ms 粒度计时的，任务被音频/LVGL 饿上几百
    // 毫秒就会被误判成"面板没响应"从而错误地锁死面板（实机日志：会话中刷新
    // 报 "BUSY did not leave idle level"，随后 3 秒锁存，长按返回首页因此刷不
    // 出画面）。刷新任务绝大部分时间在 vTaskDelay 里等通知，提高优先级不会
    // 长期占用 CPU。
    xTaskCreatePinnedToCore(RefreshTaskEntry, "ssd1619_refresh", 4096, this, 6,
                            &refresh_task_, 1);
    // Do not physically refresh the initial all-white buffer. The dashboard
    // below must be the first RAM image sent to the panel; otherwise a failed
    // or overlapping second update leaves a perfectly valid but blank screen.
    dashboard_.SetRefreshCallback(
        [this](bool partial, bool force) { RequestRefresh(partial, force); });
    // 开机首页菜单不再在构造函数里渲染：已配置 InkSight/双屏时 board 会跳过它，
    // 让网络图成为开机第一屏（消除双闪）；未配置时才由 board 调 ShowBootHome()。
}

void Ssd1619Display::ShowBootHome() {
    // 配网成功后默认进入欢迎页（引导用户去后台配置屏幕内容），
    // 按 OK 再进入功能菜单。
    dashboard_.Show(EpaperDashboard::Page::kWelcome);
}

Ssd1619Display::~Ssd1619Display() {
    if (refresh_task_) vTaskDelete(refresh_task_);
    delete fpc1_color_driver_;
    fpc1_color_driver_ = nullptr;
    if (spi_) spi_bus_remove_device(spi_);
    if (mutex_) vSemaphoreDelete(mutex_);
    heap_caps_free(frame_);
    heap_caps_free(tx_frame_);
    heap_caps_free(panel_frame_);
    heap_caps_free(dma_buffer_);
}

void Ssd1619Display::SetEmotion(const char* emotion) { (void)emotion; }

void Ssd1619Display::SetStatus(const char* status) {
    if (status && std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        // LISTENING also follows the end of TTS. That transition must not
        // reopen the conversation after the user has returned to the menu.
        if (assistant_response_active_)
            assistant_response_active_ = false;
        else
            dashboard_.BeginConversation();
    } else if (status && std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        assistant_response_active_ = false;
    }
    // CONNECTING/SPEAKING/STANDBY and clock/status updates intentionally do
    // not invalidate e-paper. Conversation text updates arrive separately.
}

void Ssd1619Display::SetChatMessage(const char* role, const char* content) {
    if (!role || !content || content[0] == '\0') return;
    if (std::strcmp(role, "user") == 0 ||
        std::strcmp(role, "assistant") == 0) {
        // TTS delivers complete sentences. Each visible sentence is committed
        // using the same-page SSD1619 partial waveform. If the user returned
        // home, cache late packets without allowing them to steal the menu.
        const bool visible = dashboard_.IsConversation();
        dashboard_.SetConversationMessage(role, content, visible);
        if (std::strcmp(role, "assistant") == 0) {
            assistant_response_active_ = true;
            if (visible)
                ESP_LOGI(kTag, "Assistant sentence queued for partial refresh");
        }
    }
}

void Ssd1619Display::ClearChatMessages() {
    // Idle transitions in Application clear LCD bubbles. Preserve the last
    // completed e-paper conversation until the user returns to the menu.
}

void Ssd1619Display::SetupUI() {
    // Never create LcdDisplay's animated status/chat UI on e-paper. It has a
    // clock and status widgets that invalidate continuously and would both
    // cover the dashboard and wear the panel with repeated full refreshes.
    setup_ui_called_ = true;
}

void Ssd1619Display::SetTheme(Theme* theme) {
    // Assets refreshes the selected theme after Wi-Fi connects. The inherited
    // LcdDisplay implementation styles mute/status/battery widgets, but those
    // widgets intentionally do not exist on this event-driven e-paper UI.
    // Keep the theme selection without touching null LCD objects or scheduling
    // a physical display refresh.
    current_theme_ = theme;
}

void Ssd1619Display::ShowNotification(const char* notification,
                                      int duration_ms) {
    // Wi-Fi Scanning/Connecting/Connected notifications are emitted directly
    // by ESP-IDF's small `sys_evt` task. Entering LVGL/dashboard rendering here
    // overflows that task's stack and causes a reboot loop. Provisioning and
    // activation use ShowActivationUi(), which is dispatched on the
    // application's scheduler and persists on e-paper; ordinary transient
    // LCD toasts therefore stay in the serial log only.
    if (notification && notification[0] != '\0') {
        ESP_LOGI(kTag, "Notification: %s", notification);
    }
    (void)duration_ms;
}

void Ssd1619Display::ShowActivationUi(const char* message, const char* code) {
    activation_message_ = message ? message : "";
    xiaozhi_activation_code_ = code ? code : "";
    dashboard_.ShowActivation(activation_message_, xiaozhi_activation_code_,
                              xiaowu_pair_code_);
}

void Ssd1619Display::ShowProvisioningGuide(const char* ssid, const char* url) {
    dashboard_.ShowProvisioning(ssid ? ssid : "", url ? url : "");
}

void Ssd1619Display::SetXiaowuPairCode(const char* code) {
    xiaowu_pair_code_ = code ? code : "";
    if (activation_message_.empty()) {
        activation_message_ =
            "小悟后台绑定码用于绑定设备，打开 dualcn.xiaowuai.vip 输入。\n\n"
            "小智后台激活码用于启用对话，打开 xiaozhi.me 输入。";
    }
    dashboard_.ShowActivation(activation_message_, xiaozhi_activation_code_,
                              xiaowu_pair_code_);
    // 现场排查用：绑定码为空时页面会切过去但只剩正文（白底黑字在这块屏上
    // 几乎不出墨），看起来就像"没切页"。把两个码都打出来，一眼能区分
    // 「没切页」和「切了但码是空的」。
    ESP_LOGI(kTag,
             "A 屏请求切到绑定码页: 小悟码=[%s] 小智码=[%s] 正文=%u 字",
             xiaowu_pair_code_.empty() ? "空" : xiaowu_pair_code_.c_str(),
             xiaozhi_activation_code_.empty() ? "空"
                                              : xiaozhi_activation_code_.c_str(),
             static_cast<unsigned>(activation_message_.size()));
}

void Ssd1619Display::HideActivationUi() {
    if (xiaowu_pair_code_.empty()) {
        dashboard_.HideActivation();
        return;
    }
    activation_message_ =
        "小智后台已完成激活。\n\n"
        "小悟设备绑定请打开 dualcn.xiaowuai.vip 输入下方设备绑定码。";
    xiaozhi_activation_code_.clear();
    dashboard_.ShowActivation(activation_message_, "", xiaowu_pair_code_);
}

void Ssd1619Display::InitializeSpi() {
    // 两个座的 CS 都初始化为输出并拉高（未选中）。没用到的那个座保持不被选中，
    // 因此即使座上插着屏也不会收到任何命令；这样探测阶段可以安全地同时看两条
    // BUSY 线，而不用预先知道屏在哪。
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << EPD_CS_PIN_FPC1) |
                           (1ULL << EPD_CS_PIN_FPC2) | (1ULL << EPD_DC_PIN) |
                           (1ULL << EPD_RST_PIN);
    outputs.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&outputs));
    gpio_set_level(EPD_CS_PIN_FPC1, 1);
    gpio_set_level(EPD_CS_PIN_FPC2, 1);
    gpio_set_level(cs_pin_, 1);
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(EPD_RST_PIN, 1);

    // 两条 BUSY 都配置成带上拉的输入：浮空的那个会稳稳读到 1，正是"没插屏"
    // 的判据；插着屏的那个在复位后会拉低再放开。
    gpio_config_t busy = {};
    busy.pin_bit_mask = (1ULL << EPD_BUSY_PIN_FPC1) |
                        (1ULL << EPD_BUSY_PIN_FPC2);
    busy.mode = GPIO_MODE_INPUT;
    busy.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&busy));

    spi_bus_config_t bus = {};
    bus.mosi_io_num = EPD_MOSI_PIN;
    bus.miso_io_num = -1;
    bus.sclk_io_num = EPD_SCK_PIN;
    bus.max_transfer_sz = kFrameBytes + 8;
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_NUM, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t device = {};
    device.clock_speed_hz = 4 * 1000 * 1000;
    device.mode = 0;
    // Drive FPC1 CS2 explicitly. This also makes CS timing observable and
    // avoids relying on the SPI GPIO matrix for GPIO39.
    device.spics_io_num = -1;
    device.queue_size = 2;
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_NUM, &device, &spi_));
#if FPC1_COLOR_COUNT == 3
    ESP_LOGI(kTag,
             "FPC1 三色 UC1679/SSD1683 ready: %dx%d, frame=%u bytes, "
             "CS=GPIO%d BUSY=GPIO%d (DIN=GPIO%d CLK=GPIO%d DC=GPIO%d RST=GPIO%d)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, static_cast<unsigned>(kFrameBytes),
             EPD_CS_PIN_FPC1, EPD_BUSY_PIN_FPC1,
             EPD_MOSI_PIN, EPD_SCK_PIN, EPD_DC_PIN, EPD_RST_PIN);
#else
    ESP_LOGI(kTag,
             "SSD1619 ready: %dx%d, frame=%u bytes, %s 座 "
             "(DIN=GPIO%d CLK=GPIO%d CS=GPIO%d DC=GPIO%d RST=GPIO%d "
             "BUSY=GPIO%d, 忙电平=%d)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, static_cast<unsigned>(kFrameBytes),
             "FPC1",
             EPD_MOSI_PIN, EPD_SCK_PIN, cs_pin_, EPD_DC_PIN, EPD_RST_PIN,
             busy_pin_, EPD_BUSY_LEVEL);
#endif
}

void Ssd1619Display::HardwareReset() {
#if FPC1_COLOR_COUNT == 3
    // FPC1 三色屏由 Uc1679Display 管理复位；避免走旧 SSD1619 的复位流程。
    return;
#else
#if EPD_ENABLE_COLOR_SCREEN
    // FPC2 彩色屏正在全刷时（~16s），共享 RST 会打断其波形，须等待完成。
    // 驱动随 FPC2_PANEL_COLORS 切换（3-color→SSD1683，4-color→SSD2683ZA）。
#if FPC2_COLOR_COUNT == 3
    if (Uc1679Display::IsRefreshing()) {
        ESP_LOGW(kTag, "Shared RST: waiting for color refresh to finish");
        const TickType_t start = xTaskGetTickCount();
        while (Uc1679Display::IsRefreshing() &&
               (xTaskGetTickCount() - start) * portTICK_PERIOD_MS < 30000) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
#else
    if (Ssd2683zaColorPanel::IsRefreshing()) {
        ESP_LOGW(kTag, "Shared RST: waiting for color refresh to finish");
        const TickType_t start = xTaskGetTickCount();
        while (Ssd2683zaColorPanel::IsRefreshing() &&
               (xTaskGetTickCount() - start) * portTICK_PERIOD_MS < 30000) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
#endif
#endif
    // The 4.2-inch SSD1619 reference sequence uses three reset pulses.
    for (int i = 0; i < 3; ++i) {
        gpio_set_level(EPD_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(EPD_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
}

bool Ssd1619Display::WaitBusy(uint32_t timeout_ms) {
    // FPC1 的 BUSY 线有些装配/转接板没有引出，GPIO40 会呈现浮空。此时不能
    // 把恒定电平当成控制器仍忙并因此拒绝所有首帧；SSD1619 的 SPI 命令仍可
    // 正常发送，使用保守的固定等待完成复位/模拟电源启动，再由全刷阶段按
    // 完整波形时长等待。DetectPanelConnector() 已在初始化前记录此状态。
    if (!busy_driven_) {
        constexpr uint32_t kBusylessSettleMs = 250;
        const uint32_t settle_ms = std::min(timeout_ms, kBusylessSettleMs);
        ESP_LOGW(kTag,
                 "FPC1 BUSY(GPIO%d) 不可观测，按固定 %lu ms 等待控制器稳定",
                 busy_pin_, static_cast<unsigned long>(settle_ms));
        if (settle_ms > 0) vTaskDelay(pdMS_TO_TICKS(settle_ms));
        return true;
    }
    const TickType_t start = xTaskGetTickCount();
    const int idle_level = busy_idle_level_ >= 0
                               ? busy_idle_level_
                               : (EPD_BUSY_LEVEL ? 0 : 1);
    while (gpio_get_level(busy_pin_) != idle_level) {
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            ESP_LOGE(kTag, "BUSY timeout after %lu ms (level=%d)",
                     static_cast<unsigned long>(timeout_ms),
                     gpio_get_level(busy_pin_));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool Ssd1619Display::WaitRefreshComplete(int idle_level, uint32_t timeout_ms,
                                         uint32_t busyless_total_ms) {
    // GPIO40 被上下拉测试确认浮空时，电平既不能表示 idle，也不能表示 busy。
    // 直接按全刷/快刷的保守波形时长等待，避免误报超时后复位正在工作的面板。
    if (!busy_driven_ && busyless_total_ms != 0) {
        ESP_LOGW(kTag,
                 "FPC1 BUSY(GPIO%d) 不可观测，忽略电平并按 %lu ms 等待波形",
                 busy_pin_, static_cast<unsigned long>(busyless_total_ms));
        vTaskDelay(pdMS_TO_TICKS(busyless_total_ms));
        return true;
    }
    // Some SSD1619 carrier boards expose BUSY directly, while others invert it.
    // Use the level immediately before 0x20 as idle and require a complete
    // idle -> busy -> idle transition instead of assuming a fixed polarity.
    // 真的在跑波形的面板，0x20 之后几十毫秒内就会拉忙。窗口保持
    // kBusyAssertTimeoutMs：放宽到几百毫秒会让"拉忙慢一点"的刷新误入下面的
    // 定时兜底分支，兜底只按时间返回、不校验画面，属于掩盖问题。
    const uint32_t assert_window = kBusyAssertTimeoutMs;
    const TickType_t assert_start = xTaskGetTickCount();
    while (gpio_get_level(busy_pin_) == idle_level) {
        if ((xTaskGetTickCount() - assert_start) * portTICK_PERIOD_MS >=
            assert_window) {
            // A number of otherwise compatible 24-pin panels do not route
            // BUSY (or expose an open-drain BUSY that this carrier cannot
            // observe).  The old code immediately reset the controller here.
            // That reset landed while the waveform was still running and
            // guaranteed a permanently blank panel.  For the fixed FPC1
            // installation it is safer to let the waveform finish by time.
            if (busyless_total_ms != 0) {
                const uint32_t elapsed =
                    (xTaskGetTickCount() - assert_start) * portTICK_PERIOD_MS;
                ESP_LOGW(kTag,
                         "BUSY 在 0x20 后始终停空闲电平 %d（CS=GPIO%d BUSY=GPIO%d，"
                         "线路被面板驱动=%s）→ 等待 %lu ms 后再按可观测性判定",
                         idle_level, cs_pin_, busy_pin_,
                         busy_driven_ ? "是" : "否",
                         static_cast<unsigned long>(busyless_total_ms));
                if (elapsed < busyless_total_ms) {
                    vTaskDelay(pdMS_TO_TICKS(busyless_total_ms - elapsed));
                }
                // 只有确认 BUSY 不可观测时，固定等待才可作为兼容路径。若线路有
                // 驱动却没有观察到刷新波形，等待结束不能证明面板工作，必须报失败。
                if (busy_driven_) {
                    ESP_LOGE(kTag,
                             "固定等待结束但 FPC1 BUSY 线路有驱动、未观察到波形；"
                             "本次刷新判失败");
                    return false;
                }
                return true;
            }
            ESP_LOGE(kTag,
                     "SSD1619 BUSY did not leave idle level %d after 0x20 "
                     "(CS=GPIO%d BUSY=GPIO%d)",
                     idle_level, cs_pin_, busy_pin_);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    const TickType_t release_start = xTaskGetTickCount();
    while (gpio_get_level(busy_pin_) != idle_level) {
        if ((xTaskGetTickCount() - release_start) * portTICK_PERIOD_MS >=
            timeout_ms) {
            ESP_LOGE(kTag,
                     "SSD1619 BUSY timeout: idle=%d current=%d after %lu ms",
                     idle_level, gpio_get_level(busy_pin_),
                     static_cast<unsigned long>(timeout_ms));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // 把「真的跑过波形」这件事留成可核查的证据：拉忙的延迟 + 忙态持续时长。
    // 下一次现场日志里，这两行有值 = 面板在执行命令；没有这两行却报
    // "Full refresh complete" = 靠定时兜底蒙过去的，画面不一定会变。
    ESP_LOGI(kTag, "BUSY 波形实跑：拉忙延迟 %lu ms，忙态持续 %lu ms",
             static_cast<unsigned long>(
                 (release_start - assert_start) * portTICK_PERIOD_MS),
             static_cast<unsigned long>(
                 (xTaskGetTickCount() - release_start) * portTICK_PERIOD_MS));
    return true;
}

void Ssd1619Display::Command(uint8_t command, const uint8_t* data, size_t length) {
    gpio_set_level(EPD_DC_PIN, 0);
    gpio_set_level(cs_pin_, 0);
    spi_transaction_t transaction = {};
    transaction.length = 8;
    transaction.tx_buffer = &command;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &transaction));
    gpio_set_level(cs_pin_, 1);
    if (data && length) Data(data, length);
}

void Ssd1619Display::Data(const uint8_t* data, size_t length) {
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(cs_pin_, 0);
    while (length > 0) {
        const size_t chunk = std::min(length, kDmaChunkBytes);
        std::memcpy(dma_buffer_, data, chunk);
        spi_transaction_t transaction = {};
        transaction.length = chunk * 8;
        transaction.tx_buffer = dma_buffer_;
        const esp_err_t err = spi_device_polling_transmit(spi_, &transaction);
        if (err != ESP_OK) {
            gpio_set_level(cs_pin_, 1);
            ESP_LOGE(kTag, "SPI data transmit failed: %s, chunk=%u",
                     esp_err_to_name(err), static_cast<unsigned>(chunk));
            MarkPanelFaulted();
            return;
        }
        data += chunk;
        length -= chunk;
    }
    gpio_set_level(cs_pin_, 1);
}

void Ssd1619Display::SetRamWindow(int x_start, int x_end,
                                  int y_start, int y_end) {
    const uint8_t x_window[] = {static_cast<uint8_t>(x_start),
                                static_cast<uint8_t>(x_end)};
    const uint8_t y_window[] = {static_cast<uint8_t>(y_start & 0xff),
                                static_cast<uint8_t>(y_start >> 8),
                                static_cast<uint8_t>(y_end & 0xff),
                                static_cast<uint8_t>(y_end >> 8)};
    Command(0x44, x_window, sizeof(x_window));
    Command(0x45, y_window, sizeof(y_window));
    const uint8_t x = static_cast<uint8_t>(x_start);
    const uint8_t y[] = {static_cast<uint8_t>(y_start & 0xff),
                         static_cast<uint8_t>(y_start >> 8)};
    Command(0x4e, &x, 1);
    Command(0x4f, y, 2);
}

bool Ssd1619Display::IsBusyLineDriven(gpio_num_t pin) {
    // 判别"这条 BUSY 线上有没有东西在驱动"：先把脚配成带上拉的输入读一次，
    // 再改成带下拉读一次。浮空的脚会被几万欧的弱上下拉带着走（上拉读 1、
    // 下拉读 0），而面板的输出级足以顶住弱上下拉，两次读数相同。
    // 这条判据不要求面板有任何电平跳变，所以对"复位后 BUSY 直接停在空闲
    // 电平"的屏也不会误判。
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;

    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) return true;  // 配不了就别乱判
    vTaskDelay(pdMS_TO_TICKS(2));
    const int with_pull_up = gpio_get_level(pin);

    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    if (gpio_config(&cfg) != ESP_OK) return true;
    vTaskDelay(pdMS_TO_TICKS(2));
    const int with_pull_down = gpio_get_level(pin);

    // 复原成上拉输入（与 InitializeSpi 的配置保持一致）。
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);

    return with_pull_up == with_pull_down;
}

bool Ssd1619Display::DetectPanelConnector() {
    // 两条证据并用，选出真正接着屏的那个座：
    //   ① 驱动判定（主）：BUSY 被面板驱动 ⇒ 该座有屏；浮空 ⇒ 该座空着。
    //   ② 跳变判定（辅）：复位后窗口内出现过边沿也算有屏。
    // 绝不允许把"没看到跳变"直接当成"没有面板"——那正是上一版把好屏判死、
    // 锁存 30 s 不刷新的原因。
    panel_probed_ = true;
    busy_stuck_ = false;
    HardwareReset();

    constexpr int kCount = 2;
    const gpio_num_t busy[kCount] = {EPD_BUSY_PIN_FPC1, EPD_BUSY_PIN_FPC2};
    int first[kCount] = {gpio_get_level(busy[0]), gpio_get_level(busy[1])};
    bool toggled[kCount] = {false, false};

    const TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS <
           kBusyActivityWindowMs) {
        for (int i = 0; i < kCount; ++i) {
            if (!toggled[i] && gpio_get_level(busy[i]) != first[i]) {
                toggled[i] = true;
            }
        }
        if (toggled[0] && toggled[1]) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    const bool driven[kCount] = {IsBusyLineDriven(busy[0]),
                                 IsBusyLineDriven(busy[1])};
    // driven[0] 即本 SSD1619 显示（FPC1）的 BUSY 是否被面板真实驱动。
    // 浮空（没插屏/断线）时记下 false，供 WaitRefreshComplete 区分"真·无 BUSY
    // 兼容面板"与"线断了"——后者应判刷新失败并走锁存/重探测自愈。
    busy_driven_ = driven[0];


    ESP_LOGI(kTag,
             "屏座探测：FPC1/GPIO%d 电平=%d 驱动=%s 跳变=%s；"
             "FPC2/GPIO%d 电平=%d 驱动=%s 跳变=%s",
             busy[0], first[0], driven[0] ? "是" : "否",
             toggled[0] ? "是" : "否", busy[1], first[1],
             driven[1] ? "是" : "否", toggled[1] ? "是" : "否");

    // 双屏板角色固定：FPC1/CS2/BUSY2(GPIO39/40) 是 SSD1619 快刷屏，
    // FPC2/CS1/BUSY1(GPIO9/12) 是 SSD1683 三色屏。绝不根据 BUSY 探测结果
    // 把 SSD1619 驱动切到 FPC2，否则可能向三色屏发送黑白快刷命令。
    cs_pin_ = EPD_CS_PIN;
    busy_pin_ = EPD_BUSY_PIN;
    if (!(driven[0] || toggled[0])) {
        ESP_LOGW(kTag,
                 "快刷屏 FPC1 BUSY2 不可观测；仍固定使用 FPC1 CS=GPIO%d / "
                 "BUSY=GPIO%d（不会切到 FPC2）",
                 cs_pin_, busy_pin_);
        return false;
    }

    ESP_LOGI(kTag, "检测到 FPC1 SSD1619 快刷面板：CS=GPIO%d, BUSY=GPIO%d",
             cs_pin_, busy_pin_);
    return true;
}

int Ssd1619Display::DetectBusyIdleLevel() {
    // 硬件复位之后 BUSY 会先保持"忙"电平一小会儿再回到空闲。旧实现复位后只
    // 隔 5 ms 采两次就判定，几乎必然采到复位期间的忙电平（实机日志里采到了
    // 1，而配置 EPD_BUSY_LEVEL=1 意味着空闲应为 0），于是 WaitRefreshComplete
    // 的 idle->busy->idle 判定整体反向：发 0x20 后面板明明在忙，却被当成"没
    // 有离开空闲"，3 秒超时报 "BUSY did not leave idle level 1"。
    //
    // 注意不能用"连续几次读数相同就算稳定"来采样——复位后 BUSY 往往稳定地停
    // 在忙电平足够久，那样会在它跳回空闲之前就误判。必须**按配置极性主动等
    // 它落到空闲**。
    // 已经判定这条线是死的：再等一秒也没有商量余地，直接采当前电平，让后续
    // 流程走锁存分支，不再做无意义的等待与命令。
    if (busy_stuck_) return gpio_get_level(busy_pin_);

    const int cfg_idle = EPD_BUSY_LEVEL ? 0 : 1;

    // 复位刚结束时 BUSY 可能仍停在忙电平，先给它落定的时间。
    vTaskDelay(pdMS_TO_TICKS(120));

    // 按配置极性主动等它落到空闲，最多 5 s。
    //
    // 窗口必须够长：上一次现场日志里，开机自检把采样提前到 1.1 s，面板还在
    // 上电复位的忙态里，1 s 窗口内电平一次都没变，就被旧逻辑判成"极性相反"，
    // 于是整轮初始化都在 idle=1 上跑——每次刷新的 0x20 都发在面板正忙的时候
    // （直接被忽略），日志里全是"按 12000 ms 定时完成本次波形"。四张纯色图
    // 因此全都不出墨，看起来像面板坏了，其实是被自己的极性判定坑了。
    //
    // 所以：等不到就**采信配置**，绝不反相。真判反了下面 Refresh() 还有一次
    // 翻转重试兜底，不必在这里赌。
    int run = 0;
    const TickType_t t0 = xTaskGetTickCount();
    while ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS < 5000) {
        if (gpio_get_level(busy_pin_) == cfg_idle) {
            if (++run >= 5) {  // 连续 5 次（约 50 ms）稳定在空闲才算数
                ESP_LOGI(kTag, "BUSY 空闲电平采样: %d（按配置极性，连续 %d 次稳定）",
                         cfg_idle, run);
                return cfg_idle;
            }
        } else {
            run = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(kTag,
             "5 s 内 BUSY 未落到配置空闲 %d（当前 %d），仍按配置极性继续；"
             "若确实判反，Refresh() 会翻转一次重试",
             cfg_idle, gpio_get_level(busy_pin_));
    return cfg_idle;
}

bool Ssd1619Display::InitializePanel() {
    HardwareReset();
    // 先确认屏到底插在哪个座：这一条必须在采样极性之前做。断定"没有面板"或
    // 已在某座定位到面板之前，后面所有"等空闲 / 翻转极性 / 等 idle->busy->idle"
    // 都是在一条钉死的电平上空转，只会每轮白等几秒并把真正的病因掩盖成"极性
    // 不稳定"。
    if (!panel_probed_) {
        if (!DetectPanelConnector()) {
            // 探测不到面板响应绝不能直接锁存：这块屏复位后 BUSY 可能一直停在
            // 空闲电平（历史测试中 FPC2/GPIO12 恒为 0），窗口内没有边沿可看，锁存
            // 只会把一块好屏判死、静默 30 s 不刷新。按配置座继续初始化，真正
            // 的判定交给 Refresh() 里 0x20 前后的 idle -> busy -> idle 观测。
            ESP_LOGW(kTag,
                     "未探测到面板响应，仍按配置座（CS=GPIO%d / BUSY=GPIO%d）"
                     "继续初始化，由实际刷新结果判定",
                     cs_pin_, busy_pin_);
        }
    }
    // Sample the actual module's idle level. Do not rely on BUSY polarity
    // printed in a controller datasheet because FPC/adaptor boards may invert it.
    // 只在首次采样：每次全刷前都重采，会把硬件复位瞬间的抖动电平当成"空闲
    // 电平"，随后 WaitRefreshComplete 的 idle -> busy -> idle 判定整体反向
    // （"BUSY 没有离开空闲"直接超时），一次误采就锁死面板。
    // 采样策略见 DetectBusyIdleLevel()：按配置极性主动等它落到空闲，而不是
    // 复位后立刻读一次就采信。
    if (busy_idle_level_ < 0) {
        busy_idle_level_ = DetectBusyIdleLevel();
    }
    Command(0x12);  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(20));
    // 信任板级配置的 BUSY 极性。若 SWRESET 后仍未回到空闲，不能自动把
    // 正在工作的高电平改判为空闲，否则全刷会被判成"未启动"。
    if (!WaitBusy(10000)) {
        ESP_LOGE(kTag, "SWRESET 后 BUSY 未回到配置的空闲电平 %d",
                 busy_idle_level_);
        busy_stuck_ = true;
        MarkPanelFaulted();
        return false;
    }

    // Do not depend on OTP/power-on defaults. A controller can acknowledge
    // SPI and toggle BUSY while its analogue gate/source supplies are still
    // disabled, which looks exactly like a successful refresh with no image.
    // 300 gate lines: 299 = 0x012B, GD=0, SM=0, TB=0.
    const uint8_t driver_output[] = {0x2b, 0x01, 0x00};
    Command(0x01, driver_output, sizeof(driver_output));

    // SSD1619 4.2-inch reference booster soft-start values.
    const uint8_t booster[] = {0xd7, 0xd6, 0x9d};
    Command(0x0c, booster, sizeof(booster));

    // 保留屏厂 OTP 校准的 Gate/Source/VCOM 电压。通用的 0x03/0x04/0x2C 数值
    // 未针对当前 SSD1619 面板料号验证；它们可能让 BUSY 波形运行但墨水粒子不翻转。
    ESP_LOGI(kTag, "使用屏体 OTP 电压校准（不覆盖 0x03/0x04/0x2C）");

    // Use the controller's internal temperature sensor for OTP waveform
    // compensation. Without this selection some modules never start a LUT.
    const uint8_t temperature = 0x80;
    Command(0x18, &temperature, 1);

    const uint8_t entry_mode = 0x03;  // X+, Y+
    Command(0x11, &entry_mode, 1);
    const uint8_t border = 0x05;
    Command(0x3c, &border, 1);
    const uint8_t display_update[] = {0x40, 0x00};
    Command(0x21, display_update, sizeof(display_update));
    SetRamWindow();

    // Explicitly enable clock and analogue power before writing display RAM.
    // 0xC0 = enable clock + analogue. BUSY must return to the sampled idle
    // level before RAM traffic begins.
    const uint8_t power_on = 0xc0;
    const TickType_t pon_start = xTaskGetTickCount();
    Command(0x22, &power_on, 1);
    Command(0x20);
    // 上电升压需要时间建立。仅在 20 ms 后读到空闲，可能是 BUSY 尚未拉起，
    // 不能立即开始写 RAM；保守等待 150 ms 后再等待 BUSY 回到空闲。
    bool pon_busy_seen = false;
    const TickType_t pon_settle_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - pon_settle_start) * portTICK_PERIOD_MS < 150) {
        if (gpio_get_level(busy_pin_) != busy_idle_level_) pon_busy_seen = true;
        TickType_t poll_ticks = pdMS_TO_TICKS(5);
        if (poll_ticks == 0) poll_ticks = 1;
        vTaskDelay(poll_ticks);
    }
    // 这一行是「面板到底有没有升压」的唯一硬证据，别删。
    // 0x22=0xC0 打开时钟 + 模拟电源后，控制器的升压（VGH/VGL/VSH/VSL）需要
    // 一两百毫秒才能建立，期间 BUSY 一定是忙的。忙了 → 电源起来了；几乎是
    // 0 ms → 这条命令根本没触发升压，后面波形跑得再久也只是空转，玻璃不会
    // 出墨。判「屏坏了」之前先看这个数字。
    const bool pon_ok = WaitBusy(10000);
    const uint32_t pon_ms =
        static_cast<uint32_t>((xTaskGetTickCount() - pon_start) * portTICK_PERIOD_MS);
    if (!pon_ok) {
        ESP_LOGE(kTag, "模拟电源启动后 BUSY 未回到空闲电平 %d", busy_idle_level_);
        busy_stuck_ = true;
        MarkPanelFaulted();
        return false;
    }
    if (!busy_driven_) {
        ESP_LOGW(kTag,
                 "BUSY 不可观测：0x22/0x20 后仅完成固定等待 %lu ms，"
                 "无法据此确认模拟升压或面板响应",
                 static_cast<unsigned long>(pon_ms));
    } else if (!pon_busy_seen) {
        ESP_LOGW(kTag,
                 "升压后等待 %lu ms，但未观察到 BUSY 忙态；"
                 "模拟电源是否建立仍未确认",
                 static_cast<unsigned long>(pon_ms));
    } else {
        ESP_LOGI(kTag,
                 "升压 BUSY 波形已观察并回到空闲：0x22=0xC0 + 0x20，耗时 %lu ms",
                 static_cast<unsigned long>(pon_ms));
    }
    panel_initialized_ = true;
    ESP_LOGI(kTag, "SSD1619 analogue power/init sequence complete");
    return true;
}

void Ssd1619Display::MarkPanelFaulted() {
    if (!panel_faulted_) {
        ESP_LOGE(kTag,
                 "面板刷新故障，进入锁存；%d ms 后自动复位重试（期间刷新静默）",
                 busy_stuck_ ? (int)kPanelStuckRetryMs
                             : (int)kPanelFaultRetryMs);
    }
    panel_faulted_ = true;
    panel_faulted_at_ms_ = esp_timer_get_time() / 1000;
}

void Ssd1619Display::Refresh(const uint8_t* data, bool partial) {
#if FPC1_COLOR_COUNT == 3
    // FPC1 已改为与 FPC2 相同的 B/W/R 三色面板。LVGL 和 dashboard 继续输出
    // 1bpp 黑白帧；红色平面传 nullptr，由 UC1679 驱动补全为全白（无红色）。
    // UC1679 自己持有共享总线锁，因此这里不能再创建 SSD1619 的 EpaperBusGuard。
    if (fpc1_color_driver_ != nullptr) {
        if (!fpc1_color_driver_->Available() && !fpc1_color_driver_->Init()) {
            ESP_LOGE(kTag, "A 屏 FPC1 三色驱动初始化失败，帧未刷新");
            return;
        }
        if (!fpc1_color_driver_->ShowFrame(data, nullptr)) {
            ESP_LOGE(kTag, "A 屏 FPC1 三色全刷失败");
            return;
        }
        std::memcpy(panel_frame_, data, kFrameBytes);
        base_frame_valid_ = true;
        ESP_LOGI(kTag, "A 屏 FPC1 三色全刷完成（黑白帧，无红色）partial 请求=%d",
                 partial ? 1 : 0);
        return;
    }
#endif
    // 与 FPC2(SSD2683ZA) 共享 SPI 总线/DC/RST：本函数可能从本类的
    // RefreshTask 或双屏刷新任务(RawFrameRefresh)并发调用，必须互斥。
    EpaperBusGuard bus_guard;
    if (panel_faulted_) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        // BUSY 线被钉死时，重试只会重复"复位 -> 等 3 秒 -> 超时"。与其每
        // 700 ms 空转一轮刷日志，不如拉长退避；硬件（重新插好 FPC）修好后
        // 下一次窗口会清掉判定并重新探测。
        const int64_t retry_ms =
            busy_stuck_ ? kPanelStuckRetryMs : kPanelFaultRetryMs;
        if (panel_faulted_at_ms_ != 0 &&
            now_ms - panel_faulted_at_ms_ < retry_ms) {
            return;
        }
        if (busy_stuck_) {
            ESP_LOGW(kTag, "BUSY 无响应已 %d ms，重新探测屏座",
                     (int)(now_ms - panel_faulted_at_ms_));
            busy_stuck_ = false;
            // 允许重新选座：热插拔到另一个座也能自愈。
            panel_probed_ = false;
            busy_idle_level_ = -1;
        } else {
            ESP_LOGW(kTag, "面板锁存已 %d ms，复位后重试刷新",
                     (int)(now_ms - panel_faulted_at_ms_));
        }
        panel_faulted_ = false;
        panel_initialized_ = false;
        partial_refresh_count_ = 0;
        base_frame_valid_ = false;
    }
    if (RefreshOnce(data, partial)) return;
    if (busy_stuck_) {
        // 初始化时已确认 BUSY 没回配置空闲电平，避免立刻再复位一次并
        // 延长配网提示的首屏时间；等退避窗口后再做一次硬件重探测。
        MarkPanelFaulted();
        return;
    }
    // 第一次没生效：立刻硬复位整块面板再刷一次。偶发的 BUSY 采样抖动、SPI
    // 被抢占、上一帧波形残留都能这样救回来。旧实现是先锁存、静默 700 ms 再
    // 等下一次刷新请求——演示场景下"按了没反应"最致命，所以改成当场重试。
    ESP_LOGW(kTag, "刷新未生效，硬复位面板后立即重试一次");
    panel_initialized_ = false;
    partial_refresh_count_ = 0;
    base_frame_valid_ = false;
    if (RefreshOnce(data, partial)) {
        ESP_LOGI(kTag, "重试刷新成功");
        panel_faulted_ = false;
        panel_faulted_at_ms_ = 0;
        return;
    }

    // BUSY 这条线从头到尾没跳过：翻转极性没有任何意义（两边都钉死），只会
    // 让下一轮把判定的极性翻回去、形成 0↔1 震荡。直接认输并锁存，日志里
    // ProbeBusyActivity 已经说明了该查什么。
    if (busy_stuck_) {
        MarkPanelFaulted();
        return;
    }

    // 两次都无效：高度怀疑 BUSY 空闲电平判反。症状就是"0x20 之后 BUSY 纹丝
    // 不动"，WaitRefreshComplete 走定时兜底、日志照样报成功、玻璃不动。
    // 翻一次极性再赌最后一把——正常板子走不到这里，判反的板子能自愈。
    //
    // 上一轮把这段删掉是错的：DetectBusyIdleLevel 现在改成"采信配置"，
    // 如果某块模块真的反相，就彻底没有补救路径了。
    busy_idle_level_ = busy_idle_level_ ? 0 : 1;
    ESP_LOGW(kTag, "两次刷新无效，翻转 BUSY 空闲电平为 %d 后重试最后一次",
             busy_idle_level_);
    panel_initialized_ = false;
    partial_refresh_count_ = 0;
    base_frame_valid_ = false;
    panel_faulted_ = false;
    if (RefreshOnce(data, partial)) {
        ESP_LOGI(kTag, "翻转 BUSY 极性后刷新成功（idle=%d）", busy_idle_level_);
        panel_faulted_ = false;
        panel_faulted_at_ms_ = 0;
        return;
    }

    busy_stuck_ = true;
    MarkPanelFaulted();
}

bool Ssd1619Display::RefreshOnce(const uint8_t* data, bool partial) {
    if (!panel_initialized_ && !InitializePanel()) return false;
    if (panel_faulted_) return false;

    if (partial) {
        int x_start = static_cast<int>(kStride);
        int x_end = -1;
        int y_start = DISPLAY_HEIGHT;
        int y_end = -1;
        for (int y = 0; y < DISPLAY_HEIGHT; ++y) {
            for (int x = 0; x < static_cast<int>(kStride); ++x) {
                const size_t offset = static_cast<size_t>(y) * kStride + x;
                if (panel_frame_[offset] != data[offset]) {
                    x_start = std::min(x_start, x);
                    x_end = std::max(x_end, x);
                    y_start = std::min(y_start, y);
                    y_end = std::max(y_end, y);
                }
            }
        }
        if (x_end < x_start || y_end < y_start) return true;

        HardwareReset();
        vTaskDelay(pdMS_TO_TICKS(10));
        // 显式 2 秒：默认 10 秒在极性判反时会把整次刷新死死卡住，用户看到
        // 的就是"按了没反应"。复位后回空闲远不需要 10 秒。
        if (!WaitBusy(2000)) return false;
        const uint8_t partial_border = 0x80;
        Command(0x3c, &partial_border, 1);
        const uint8_t partial_ram_option[] = {0x00, 0x00};
        Command(0x21, partial_ram_option, sizeof(partial_ram_option));
        // 上传 2 相位 partial LUT（比原版 1 相位驱动强，残影少且不闪）。
        Command(0x32, kPartialLut, sizeof(kPartialLut));
        SetRamWindow(x_start, x_end, y_start, y_end);
        Command(0x24);
        const size_t row_bytes = static_cast<size_t>(x_end - x_start + 1);
        for (int y = y_start; y <= y_end; ++y) {
            Data(data + static_cast<size_t>(y) * kStride + x_start, row_bytes);
        }
        const uint8_t update = 0xc7;
        // 触发前先把 BUSY 排到空闲：面板还忙的时候发 0x20 会被忽略，表现为
        // "BUSY 没有离开空闲电平"，进而被误判成面板故障并锁存。
        WaitBusy(2000);
        Command(0x22, &update, 1);
        Command(0x20);
        // Some 4.2-inch modules keep BUSY asserted much longer than the
        // nominal fast LUT duration, especially immediately after power-up.
        // Never reset a panel that is actively driving BUSY high.
        if (!WaitRefreshComplete(busy_idle_level_, 30000, 3500)) return false;
        {
            ++partial_refresh_count_;
            // RED RAM is used as the previous-image reference by the
            // differential LUT. Synchronize only the updated window so the
            // next sentence compares against what is now physically visible.
            SetRamWindow(x_start, x_end, y_start, y_end);
            Command(0x26);
            for (int y = y_start; y <= y_end; ++y) {
                Data(data + static_cast<size_t>(y) * kStride + x_start,
                     row_bytes);
            }
            std::memcpy(panel_frame_, data, kFrameBytes);
            ESP_LOGI(kTag, "Partial refresh complete (%u), area=(%d,%d)-(%d,%d)",
                     static_cast<unsigned>(partial_refresh_count_),
                     x_start * 8, y_start, (x_end + 1) * 8 - 1, y_end);
        }
        return true;
    }

    // The reference partial sequence includes a hardware reset and programs
    // only the registers needed by the OTP partial waveform.  Re-run the
    // complete analogue/controller setup before returning to a full waveform.
    if (partial_refresh_count_ != 0) {
        panel_initialized_ = false;
        if (!InitializePanel()) return false;
    }

    // 这里曾经额外补一条 0x04(Power ON)，想验证"面板是不是缺升压"。实测这条
    // 是反效果：发完立刻读 BUSY 还是空闲（升压要几百毫秒才拉忙），WaitBusy
    // 当场返回 0 ms，紧接着的 0x24 15 KB 数据正好灌在面板 power-on 序列中间，
    // 波形改成 4.05 s 而画面依旧不动。init 里 0x22=0xC0 + 0x20 已经把模拟
    // 电源拉起来了，全刷前不需要再补一次。

    // Restore the full-refresh settings after a partial update.
    const uint8_t full_border = 0x05;
    Command(0x3c, &full_border, 1);
    const uint8_t full_ram_option[] = {0x40, 0x00};
    Command(0x21, full_ram_option, sizeof(full_ram_option));
    SetRamWindow();
    Command(0x24);
    Data(data, kFrameBytes);
    // 全刷写两个平面。0x26 是「旧画面参考平面」：
    //   同相（写 data）—— 若 LUT 是绝对驱动，0x26 不参与，照常出图；
    //                    若 LUT 是差分驱动，等于告诉控制器「整屏无变化」，
    //                    波形照跑、BUSY 照翻转、日志照报成功，玻璃不动。
    //   反相（写 ~data）—— 差分驱动下全屏都判为变化，全量出图；
    //                    但绝对驱动下可能把 RED 平面点亮成整片红/脏屏。
    // 这两种写法在实机上表现完全相反，且无法从日志区分，所以由
    // invert_old_plane_ 交给现场自检去选，正常运行保持在 config 里选定的值。
    ESP_LOGI(kTag, "全刷旧平面 0x26 使用%s数据",
             invert_old_plane_ ? "反相（强制全屏变化）" : "同相");
    SetRamWindow();
    Command(0x26);
    if (invert_old_plane_) {
        for (size_t i = 0; i < kFrameBytes; ++i) {
            panel_frame_[i] = static_cast<uint8_t>(~data[i]);
        }
        Data(panel_frame_, kFrameBytes);
    } else {
        Data(data, kFrameBytes);
    }
    const uint8_t update = 0xf7;
    if (self_test_pon_) {
        // 诊断变体：在触发波形前用 0x22=0xC0 + 0x20 再走一遍完整的 power-on
        // 序列，并把 BUSY 忙了多久打出来。忙了上百毫秒 = 升压真的建立了。
        const uint8_t pon = 0xc0;
        const TickType_t t0 = xTaskGetTickCount();
        Command(0x22, &pon, 1);
        Command(0x20);
        const bool ok = WaitBusy(3000);
        ESP_LOGW(kTag, "自检变体：强制 power-on %s，BUSY 忙了 %lu ms",
                 ok ? "成功" : "超时",
                 static_cast<unsigned long>(
                     (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
    }
    // 同上：触发前先把 BUSY 排到空闲，避免 0x20 被忽略。
    WaitBusy(2000);
    Command(0x22, &update, 1);
    Command(0x20);
    // Full OTP waveforms commonly need 8-12 seconds.  When BUSY is not
    // observable, never reset at the former 3-second assertion timeout;
    // wait long enough for the physical update to complete.
    // Some fitted panels have been measured holding BUSY for more than
    // 20 seconds on its first cold full refresh.  The former 20-second limit
    // reset it in the middle of that waveform, so neither the first frame nor
    // the queued provisioning/activation notification could ever complete.
    if (!WaitRefreshComplete(busy_idle_level_, 60000, 12000)) return false;
    base_frame_valid_ = true;
    partial_refresh_count_ = 0;
    std::memcpy(panel_frame_, data, kFrameBytes);
    if (busy_driven_) {
        ESP_LOGI(kTag, "Full refresh wait complete (FPC1 BUSY line detected)");
    } else {
        ESP_LOGW(kTag,
                 "全刷固定等待结束；BUSY(GPIO%d)不可观测，"
                 "此日志不代表屏体已响应或画面已改变",
                 busy_pin_);
    }
    return true;
}

void Ssd1619Display::RunPatternSelfTest() {
    // 现场诊断：连刷三张纯色图。目的只有一个——把「面板/波形能不能让墨水动」
    // 和「帧数据对不对」彻底分开。三张图肉眼可见地变化，说明 SSD1619 的
    // 升压、时序、波形全都正常，问题只可能在上游取图/解码；三张都纹丝不动，
    // 就是面板没有驱动电压，再怎么改软件也没用。
    if (frame_ == nullptr || panel_frame_ == nullptr) {
        ESP_LOGW(kTag, "自检跳过：帧缓冲未就绪");
        return;
    }
    // 从冷状态开始：让第一张图触发一次「硬复位 + 完整初始化」，避免沿用
    // 开机那一次可能已经脏掉的控制器状态。
    panel_initialized_ = false;
    panel_faulted_ = false;
    base_frame_valid_ = false;
    partial_refresh_count_ = 0;
    ESP_LOGW(kTag, "=== A 屏纯色自检开始：四张图，每张约 4 s ===");
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        ESP_LOGW(kTag, "自检跳过：内存不足");
        return;
    }
    // 前 3 张沿用现有初始化；第 4 张在 0x22/0x20 之前再强制走一遍
    // power-on（0xC0 + 激活），用来单独排除「升压没建立」这条分支。
    //
    // 注意：这里曾经发的是裸 0x04，注释还写成 "Power ON"——错的。SSD1680/1619
    // 家族 0x04 是 Source Driving Voltage Control（源极电压），不带参数发出去
    // 只会让控制器停在等参数状态。真正打开升压的是 0x22 的 bit6(analog) + 0x20。
    // 四张图 = 2×2 组合：{全黑, 全白} × {0x26 同相, 0x26 反相}。
    // 只变一个变量，肉眼就能读出结论：
    //   黑→白 有变化（前两张）  = 绝对驱动，0x26 不参与，同相可用；
    //   白→黑 有变化（后两张）  = 差分驱动，必须写反相；
    //   四张全都不变            = 面板没有驱动电压，与 0x26 无关，查硬件。
    static const char* kNames[4] = {"全黑 · 0x26 同相", "全白 · 0x26 同相",
                                    "全黑 · 0x26 反相", "全白 · 0x26 反相"};
    for (int k = 0; k < 4; ++k) {
        // 前两张同相（回到最初的写法），后两张反相（a94b9ab 的写法）。
        invert_old_plane_ = (k >= 2);
        // 全黑在前两张、全白在后两张：相邻两张一定是"黑↔白"，任何一次成功
        // 翻转都会在玻璃上留下极其明显的变化，不会和残影混淆。
        const uint8_t fill = (k % 2 == 0) ? 0x00 : 0xFF;
        size_t black = 0;
        for (size_t i = 0; i < kFrameBytes; ++i) {
            buf[i] = fill;
            if (fill != 0xFF) ++black;
        }
        const TickType_t t0 = xTaskGetTickCount();
        Refresh(buf, /*partial=*/false);
        const uint32_t elapsed_ms =
            static_cast<uint32_t>((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        ESP_LOGW(kTag,
                 "自检 %d/4【%s】已下发：黑像素=%u/%u，耗时 %u ms，"
                 "BUSY 当前=%d idle=%d，old_plane=%s",
                 k + 1, kNames[k], static_cast<unsigned>(black),
                 static_cast<unsigned>(kFrameBytes), static_cast<unsigned>(elapsed_ms),
                 gpio_get_level(busy_pin_), busy_idle_level_,
                 invert_old_plane_ ? "反相" : "同相");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    invert_old_plane_ = (EPD_INVERT_OLD_PLANE != 0);
    heap_caps_free(buf);
    ESP_LOGW(kTag,
             "自检结束：①有变化=面板与波形正常，查取图/解码；②只有第 4 张有变化="
             "面板需要 0x04 PON；③四张都没变化=面板无驱动电压（硬件）");
}

void Ssd1619Display::RawFrameRefresh(const uint8_t* frame, bool partial) {
    // 外部帧进来先统计"黑像素"数量：全 0 或全 1 的帧意味着上游解码/取图为空，
    // 屏上当然什么都不会变，但日志里的"已刷新"会骗人。这一行是区分
    // "取图为空" 和 "面板不驱动" 的最便宜证据。
    {
        size_t black = 0;
        for (size_t i = 0; i < kFrameBytes; ++i) {
            uint8_t v = static_cast<uint8_t>(~frame[i]);
            while (v) {
                black += static_cast<size_t>(v & 1u);
                v = static_cast<uint8_t>(v >> 1);
            }
        }
        ESP_LOGI(kTag, "外部帧：黑像素 %u/%u（%u%%）partial=%d",
                 static_cast<unsigned>(black),
                 static_cast<unsigned>(kFrameBytes * 8),
                 static_cast<unsigned>(black * 100 / (kFrameBytes * 8)),
                 partial ? 1 : 0);
    }
    // 占位此前被释放过（语音会话把仪表盘/对话文本刷上了屏）时，外部画面
    // 必须走全刷：partial 只驱动变化区域，会话文本的像素会残留在屏上。
    bool need_full = false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    need_full = !hold_external_frame_ && partial;
    xSemaphoreGive(mutex_);
    Refresh(frame, partial && !need_full);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!hold_external_frame_) {
        ESP_LOGI(kTag, "外部画面已上屏：暂停仪表盘重绘（按键/语音可调回）");
    }
    hold_external_frame_ = true;
    xSemaphoreGive(mutex_);
}

void Ssd1619Display::ReleaseExternalFrameHold() {
    bool release = false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    release = hold_external_frame_;
    hold_external_frame_ = false;
    xSemaphoreGive(mutex_);
    if (!release) return;
    ESP_LOGI(kTag, "外部画面占位已释放，交还仪表盘");
    RequestRefresh(true, /*force=*/true);
}

void Ssd1619Display::FlushCallback(lv_display_t* display, const lv_area_t* area,
                                   uint8_t* pixels) {
    auto* self = static_cast<Ssd1619Display*>(lv_display_get_user_data(display));
    if (!self || !self->refresh_task_) {
        lv_display_flush_ready(display);
        return;
    }
    const auto* source = reinterpret_cast<const uint16_t*>(pixels);
    const int source_width = area->x2 - area->x1 + 1;
    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    for (int y = std::max(0, static_cast<int>(area->y1));
         y <= std::min(DISPLAY_HEIGHT - 1, static_cast<int>(area->y2)); ++y) {
        for (int x = std::max(0, static_cast<int>(area->x1));
             x <= std::min(DISPLAY_WIDTH - 1, static_cast<int>(area->x2)); ++x) {
            const uint16_t color = source[(y - area->y1) * source_width + x - area->x1];
            const int r = ((color >> 11) & 0x1f) * 255 / 31;
            const int g = ((color >> 5) & 0x3f) * 255 / 63;
            const int b = (color & 0x1f) * 255 / 31;
            const bool white = (77 * r + 150 * g + 29 * b) >= (128 * 256);
            const size_t index = static_cast<size_t>(y) * kStride + x / 8;
            const uint8_t mask = 0x80 >> (x & 7);
            if (white) self->frame_[index] |= mask;
            else self->frame_[index] &= static_cast<uint8_t>(~mask);
        }
    }
    xSemaphoreGive(self->mutex_);
    lv_display_flush_ready(display);
}

void Ssd1619Display::RequestRefresh(bool partial, bool force) {
    if (!refresh_task_) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // If several LVGL events are coalesced, any requested full refresh wins.
    refresh_partial_pending_ = refresh_pending_
                                   ? (refresh_partial_pending_ && partial)
                                   : partial;
    refresh_force_pending_ = refresh_force_pending_ || force;
    refresh_pending_ = true;
    xSemaphoreGive(mutex_);
    xTaskNotifyGive(refresh_task_);
}

void Ssd1619Display::RefreshTaskEntry(void* arg) {
    static_cast<Ssd1619Display*>(arg)->RefreshTask();
}

void Ssd1619Display::RefreshTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // LVGL flushes a partial buffer in several strips. Wait until all
        // strips have landed, then perform one physical e-paper refresh.
        // 300ms was tuned for conversation / book pages with heavy full-screen
        // updates.  The speaking page only changes a short transcript label,
        // so we can safely commit faster and make the conversation feel snappier.
        vTaskDelay(pdMS_TO_TICKS(150));
        bool should_refresh = false;
        bool use_partial = false;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        // 外部画面占屏期间，仪表盘的周期重绘一律丢弃——否则刚刷出来的
        // InkSight/双屏 A 画面会被下一帧仪表盘立刻盖掉，表现为"刷了没反应"。
        // 语音会话例外：交互界面必须能实时上屏，此时顺带释放占位。
        if (hold_external_frame_ && !refresh_force_pending_) {
            if (dashboard_.IsConversation()) {
                ESP_LOGI(kTag, "会话界面需要屏幕，释放外部画面占位");
                hold_external_frame_ = false;
            } else {
                refresh_pending_ = false;
                refresh_partial_pending_ = false;
                should_refresh = false;
                xSemaphoreGive(mutex_);
                continue;
            }
        }
        if (refresh_pending_) {
            refresh_pending_ = false;
            const bool force_now = refresh_force_pending_;
            refresh_force_pending_ = false;
            // 强制刷新必须走全刷波形：它的用途就是把面板拉回应有画面。
            use_partial = refresh_partial_pending_ && !force_now;
            refresh_partial_pending_ = false;
            if (force_now || std::memcmp(tx_frame_, frame_, kFrameBytes) != 0) {
                std::memcpy(tx_frame_, frame_, kFrameBytes);
                should_refresh = true;
            } else {
                ESP_LOGD(kTag, "帧内容与上次推送一致，跳过物理刷新");
            }
        }
        xSemaphoreGive(mutex_);
        // Do not hold the framebuffer mutex during the several-second panel
        // waveform; LVGL may continue composing the next frame meanwhile.
        if (should_refresh) {
            size_t black_bits = 0;
            for (size_t i = 0; i < kFrameBytes; ++i) {
                black_bits += static_cast<size_t>(__builtin_popcount(
                    static_cast<unsigned>(~tx_frame_[i] & 0xff)));
            }
            ESP_LOGI(kTag, "LVGL frame ready: black_pixels=%u/%u page=%s",
                     static_cast<unsigned>(black_bits),
                     static_cast<unsigned>(DISPLAY_WIDTH * DISPLAY_HEIGHT),
                     dashboard_.CurrentPageName().c_str());
            // Cross-page navigation performs the cleaning full refresh. Keep
            // all same-page conversation/menu/book updates partial so a long
            // response or reading session never flashes every N updates.
            use_partial = use_partial && base_frame_valid_;
            Refresh(tx_frame_, use_partial);
        }
    }
}

#endif  // !CONFIG_EPD_PANEL_UC8253_37
