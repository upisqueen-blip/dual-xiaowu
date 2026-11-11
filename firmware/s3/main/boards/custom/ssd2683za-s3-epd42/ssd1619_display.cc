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

#if EPD_ENABLE_COLOR_SCREEN
#include "ssd2683za_color_panel.h"
#endif
// 三色驱动：A 座改插三色屏时作为上屏后端（见 SetBwrBackend）。
#include "uc1679_display.h"

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
    InitializeSpi();
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

void Ssd1619Display::RunColorCycleTest() {
#if EPD_A42_COLOR_TEST
    // 只回答一个问题：这块 4.2" 面板还有没有驱动电压。
    // 全刷波形 0xF7 一定会让整屏黑白翻转，肉眼可见；如果连这个都不闪，
    // 那么命令、BUSY、SPI 都正常也说明不了什么——是玻璃没被驱动。
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        ESP_LOGE(kTag, "纯色自检：内存不足，跳过");
        return;
    }
    // 引脚归位自证：把本驱动实际使用的 CS/BUSY 与另一块屏的引脚一起打出来。
    // 只要刷 A 屏时「另一块屏的 BUSY」也在忙，就说明 CS 选错了屏。
    ESP_LOGW(kTag,
             "===== A 屏(FPC1) 驱动自检：全黑/全白 各「同相 / 反相」一次 =====\n"
             "       本驱动 CS=GPIO%d BUSY=GPIO%d | 另一屏 CS=GPIO%d BUSY=GPIO%d",
             cs_pin_, busy_pin_, EPD_CS_PIN_FPC2, EPD_BUSY_PIN_FPC2);
    for (int k = 0; k < 4; ++k) {
        // 设计要点：相邻两张的「画面内容」完全相同，只有 0x26 的写入方式不同。
        //   第 1 张 全黑·同相 → 第 2 张 全黑·反相
        //   第 3 张 全白·同相 → 第 4 张 全白·反相
        // 这样肉眼只回答一件事：
        //   ① 四张全都不变      → 面板没有驱动电压（硬件/升压）
        //   ② 同相不变、反相变  → 0x26 必须写反相（差分行波形）
        //   ③ 四张都在变        → 数据链路正常，问题在别处（内容/时序）
        invert_old_plane_ = (k % 2 == 1);
        const uint8_t fill = (k < 2) ? 0x00 : 0xFF;
        std::memset(buf, fill, kFrameBytes);
        ESP_LOGW(kTag, "→ 第 %d/4 张：%s · 0x26 %s，开始下发", k + 1,
                 fill == 0x00 ? "全黑" : "全白",
                 invert_old_plane_ ? "反相" : "同相");
        const TickType_t t0 = xTaskGetTickCount();
        Refresh(buf, /*partial=*/false);
        ESP_LOGW(kTag,
                 "← 第 %d/4 张下发结束，耗时 %lu ms | 本屏 BUSY(GPIO%d)=%d "
                 "另一屏 BUSY(GPIO%d)=%d",
                 k + 1,
                 static_cast<unsigned long>((xTaskGetTickCount() - t0) *
                                            portTICK_PERIOD_MS),
                 busy_pin_, gpio_get_level(busy_pin_),
                 EPD_BUSY_PIN_FPC2, gpio_get_level(EPD_BUSY_PIN_FPC2));
        vTaskDelay(pdMS_TO_TICKS(6000));
    }
    heap_caps_free(buf);
    invert_old_plane_ = true;  // 恢复正常运行：差分面板需要反相
    ESP_LOGW(kTag, "===== 纯色自检结束：玻璃上出现整屏黑白翻转 = 面板正常 =====");
#endif
}

void Ssd1619Display::RunPanelProbe() {
#if EPD_A42_PROBE_TEST
    // 这块面板走的是"完全不信 BUSY"的路线：现有驱动逻辑里，只要
    // IsBusyLineDriven() 判出 BUSY 浮空，WaitRefreshComplete() 就会直接
    // 返回 false，于是每次刷新都被判失败、重试、翻转极性、最后锁存 30 s。
    // 结果就是"面板可能能刷，但驱动一次都没让它刷完"。
    // 本测试刻意不看 BUSY：初始化、写 RAM、触发波形全靠固定延时，
    // 唯一的判据是肉眼和 BUSY 采样轨迹。
    EpaperBusGuard bus_guard;
    ESP_LOGW(kTag, "===== A 屏（FPC1）SSD1619 纯定时自证：全程不依赖 BUSY =====");
    ESP_LOGW(kTag, "CS=GPIO%d DC=GPIO%d RST=GPIO%d BUSY=GPIO%d  配置忙电平=%d（空闲应为 %d）",
             EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN, EPD_BUSY_LEVEL,
             EPD_BUSY_LEVEL ? 0 : 1);

    // ① BUSY 这条线上有没有东西在驱动（两条同时测，互为对照）
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << EPD_BUSY_PIN) | (1ULL << EPD_BUSY_PIN_FPC2);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int busy_up = gpio_get_level(EPD_BUSY_PIN);
    const int busy_up_o = gpio_get_level(EPD_BUSY_PIN_FPC2);
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));
    const int busy_down = gpio_get_level(EPD_BUSY_PIN);
    const int busy_down_o = gpio_get_level(EPD_BUSY_PIN_FPC2);
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    ESP_LOGW(kTag,
             "① 本屏 BUSY(GPIO%d) 上拉读=%d 下拉读=%d → %s", busy_pin_,
             busy_up, busy_down,
             busy_up == busy_down ? "被面板驱动（这条线是通的）"
                                  : "浮空（面板没有驱动这条线：没插实 / 该脚是 NC）");
    ESP_LOGW(kTag,
             "① 对侧 BUSY(GPIO%d) 上拉读=%d 下拉读=%d → %s",
             EPD_BUSY_PIN_FPC2, busy_up_o, busy_down_o,
             busy_up_o == busy_down_o ? "被面板驱动（这条线是通的）"
                                      : "浮空（面板没有驱动这条线）");

    // ② 复位瞬间 BUSY 有没有掉到忙电平
    // RST(GPIO11) 是两块屏共用的，所以这一次复位会同时打到两个座子上。
    // 谁掉低谁就是在线的面板：两边都掉 = RST 通、两块屏都活；只有对侧掉 =
    // 本屏对复位毫无反应（屏或它那根排线断）。
    const int before = gpio_get_level(EPD_BUSY_PIN);
    const int before_o = gpio_get_level(EPD_BUSY_PIN_FPC2);
    HardwareReset();
    int low_at = -1;
    int low_at_o = -1;
    for (int i = 0; i < 40; ++i) {  // 2 s
        if (gpio_get_level(EPD_BUSY_PIN) == 0 && low_at < 0) low_at = i * 50;
        if (gpio_get_level(EPD_BUSY_PIN_FPC2) == 0 && low_at_o < 0) low_at_o = i * 50;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(kTag, "② 本屏 复位前 BUSY=%d；复位后 2 s 内%s", before,
             low_at >= 0 ? "出现过低电平（面板有回应复位）"
                         : "一直是高电平（面板对复位没有反应）");
    ESP_LOGW(kTag, "② 对侧 复位前 BUSY=%d；复位后 2 s 内%s", before_o,
             low_at_o >= 0 ? "出现过低电平（该座面板有回应复位）"
                           : "一直是高电平（该座也对复位没反应）");

    // ③ 三张图，纯定时
    auto fill_frame = [this](int kind) {
        for (int y = 0; y < DISPLAY_HEIGHT; ++y) {
            uint8_t value = 0xff;
            if (kind == 0) {
                value = 0x00;  // 全黑
            } else if (kind == 1) {
                value = 0xff;  // 全白
            } else {
                value = (y < DISPLAY_HEIGHT / 2) ? 0x00 : 0xff;  // 上黑下白
            }
            std::memset(frame_ + static_cast<size_t>(y) * kStride, value,
                        kStride);
        }
    };

    auto probe_one = [this](const char* name) {
        HardwareReset();
        vTaskDelay(pdMS_TO_TICKS(30));
        const uint8_t driver_output[] = {0x2b, 0x01, 0x00};
        Command(0x01, driver_output, sizeof(driver_output));
        const uint8_t booster[] = {0xd7, 0xd6, 0x9d};
        Command(0x0c, booster, sizeof(booster));
        const uint8_t temperature = 0x80;
        Command(0x18, &temperature, 1);
        const uint8_t entry_mode = 0x03;
        Command(0x11, &entry_mode, 1);
        const uint8_t border = 0x05;
        Command(0x3c, &border, 1);
        const uint8_t display_update[] = {0x40, 0x00};
        Command(0x21, display_update, sizeof(display_update));
        SetRamWindow();
        const uint8_t power_on = 0xc0;
        Command(0x22, &power_on, 1);
        Command(0x20);
        vTaskDelay(pdMS_TO_TICKS(500));

        SetRamWindow();
        Command(0x24);  // 新画面
        Data(frame_, kFrameBytes);
        for (size_t i = 0; i < kFrameBytes; ++i) {
            panel_frame_[i] = static_cast<uint8_t>(~frame_[i]);  // 旧画面取反
        }
        SetRamWindow();
        Command(0x26);  // 旧画面：与新的逐像素相反 → 每个像素都判为变化
        Data(panel_frame_, kFrameBytes);
        const uint8_t update = 0xf7;
        Command(0x22, &update, 1);
        Command(0x20);

        char trace[40] = {0};
        char trace_other[40] = {0};
        int toggles = 0;
        int prev = gpio_get_level(EPD_BUSY_PIN);
        for (int i = 0; i < 20; ++i) {  // 10 s，每 500 ms 一个采样点
            vTaskDelay(pdMS_TO_TICKS(500));
            const int level = gpio_get_level(EPD_BUSY_PIN);
            if (level != prev) {
                ++toggles;
                prev = level;
            }
            trace[i] = level ? '1' : '0';
            trace_other[i] = gpio_get_level(EPD_BUSY_PIN_FPC2) ? '1' : '0';
        }
        // 两条线同时采样：同一条 SPI、同一时刻、同一个 RST，唯一的差别是
        // 各自所在座子上的面板。这边不跳、那边也不跳 → 两块屏都没被喂到，
        // 是我这边的命令没出去；这边死平、那边有跳 → 只有这块屏不回应。
        ESP_LOGW(kTag, "③ [%s] 本屏(CS=GPIO%d BUSY=GPIO%d) 采样=%s 跳变=%d 次",
                 name, cs_pin_, busy_pin_, trace, toggles);
        ESP_LOGW(kTag, "③ [%s] 对侧(CS=GPIO%d BUSY=GPIO%d) 采样=%s",
                 name, EPD_CS_PIN_FPC2, EPD_BUSY_PIN_FPC2, trace_other);
    };

    fill_frame(0);
    probe_one("全黑");
    fill_frame(1);
    probe_one("全白");
    fill_frame(2);
    probe_one("上黑下白");
    ESP_LOGW(kTag, "===== 自证结束：请如实回答屏幕有无变化（有/无、哪一张、什么方向）=====");
#endif
}

Ssd1619Display::~Ssd1619Display() {
    if (refresh_task_) vTaskDelete(refresh_task_);
    if (spi_) spi_bus_remove_device(spi_);
    if (mutex_) vSemaphoreDelete(mutex_);
    heap_caps_free(frame_);
    heap_caps_free(tx_frame_);
    heap_caps_free(panel_frame_);
    heap_caps_free(dma_buffer_);
    heap_caps_free(bwr_red_plane_);
    heap_caps_free(bwr_last_frame_);
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
    dashboard_.ShowActivation(message ? message : "", code ? code : "");
}

void Ssd1619Display::ShowProvisioningGuide(const char* ssid, const char* url) {
    dashboard_.ShowProvisioning(ssid ? ssid : "", url ? url : "");
}

void Ssd1619Display::HideActivationUi() { dashboard_.HideActivation(); }

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

    // 引脚回读自检（诊断已通过，保留关闭状态）。GPIO39 是 ESP32-S3 的 JTAG
    // MTCK，必须确认能正常输出；实机验证拉低读 0 / 拉高读 1，CS 通路正常。
    // 需要复测时把下面的 0 改成 1。
#if 0
    {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << cs_pin_;
        io.mode = GPIO_MODE_INPUT_OUTPUT;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io);

        gpio_set_level(cs_pin_, 0);
        esp_rom_delay_us(200);
        const int cs_low = gpio_get_level(cs_pin_);
        gpio_set_level(cs_pin_, 1);
        esp_rom_delay_us(200);
        const int cs_high = gpio_get_level(cs_pin_);
        gpio_set_level(cs_pin_, 1);

        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);

        if (cs_low != 0 || cs_high != 1) {
            ESP_LOGE(kTag,
                     "CS 自检失败 GPIO%d：拉低读=%d 拉高读=%d ← 该脚无法正常"
                     "输出，SPI 命令到不了面板（检查是否被 JTAG/外设占用）",
                     cs_pin_, cs_low, cs_high);
        } else {
            ESP_LOGI(kTag, "CS 自检正常 GPIO%d（拉低读 0 / 拉高读 1）",
                     cs_pin_);
        }
    }
#endif
    ESP_LOGI(kTag,
             "SSD1619 ready: %dx%d, frame=%u bytes, %s 座 "
             "(DIN=GPIO%d CLK=GPIO%d CS=GPIO%d DC=GPIO%d RST=GPIO%d "
             "BUSY=GPIO%d, 忙电平=%d)",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, static_cast<unsigned>(kFrameBytes),
             "FPC1",
             EPD_MOSI_PIN, EPD_SCK_PIN, cs_pin_, EPD_DC_PIN, EPD_RST_PIN,
             busy_pin_, EPD_BUSY_LEVEL);
}

void Ssd1619Display::HardwareReset() {
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
    // RST 与 FPC2 共用：本次脉冲同样把对方复位了，两边序号一起前进。
    // 自己复位后紧接着会重新初始化，所以这不算「被别人复位」。
    NoteSharedReset();
}

bool Ssd1619Display::WaitBusy(uint32_t timeout_ms) {
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
    // Some SSD1619 carrier boards expose BUSY directly, while others invert it.
    // Use the level immediately before 0x20 as idle and require a complete
    // idle -> busy -> idle transition instead of assuming a fixed polarity.
    const TickType_t assert_start = xTaskGetTickCount();
    // BUSY 已确认浮空：等不到「离开空闲」是必然的，别白等 3 秒断言窗口，
    // 直接进下面按固定时长兜底的分支。
    const uint32_t assert_window_ms =
        (!busy_driven_ && busyless_total_ms != 0) ? 0 : kBusyAssertTimeoutMs;
    while (gpio_get_level(busy_pin_) == idle_level) {
        if ((xTaskGetTickCount() - assert_start) * portTICK_PERIOD_MS >=
            assert_window_ms) {
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
                         "SSD1619 BUSY stayed at idle level %d after 0x20 "
                         "(CS=GPIO%d BUSY=GPIO%d); using %lu ms timed "
                         "completion instead of resetting the panel",
                         idle_level, cs_pin_, busy_pin_,
                         static_cast<unsigned long>(busyless_total_ms));
                if (elapsed < busyless_total_ms) {
                    vTaskDelay(pdMS_TO_TICKS(busyless_total_ms - elapsed));
                }
                // BUSY 线未被面板驱动（浮空/该座没引 BUSY）时，以前这里返回失败，
                // 上层会复位重试 → 翻转极性 → 最后锁存，结果是"面板能刷，但驱动
                // 一次都没让它刷完"，屏幕永远停在开机欢迎页。波形已经按固定时长
                // 跑完，就应当算这一次刷新完成；排线问题用日志提示，不再靠锁屏表达。
                if (!busy_driven_) {
                    ESP_LOGW(kTag,
                             "BUSY 未被面板驱动（CS=GPIO%d BUSY=GPIO%d 浮空），"
                             "本次按 %lu ms 定时判定完成；若画面仍无变化请检查 FPC1 排线",
                             cs_pin_, busy_pin_,
                             static_cast<unsigned long>(busyless_total_ms));
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
    int run = 0;
    for (int i = 0; i < 100; ++i) {  // 最多 100 * 10 ms = 1 s
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

    // 一秒都没等到配置推导的空闲电平：要么是面板一直忙（故障），要么是转接
    // 板把 BUSY 反相了（本文件早有注释提到 FPC 转接板可能反相）。采信当前
    // 实测电平；若真是故障，后面还有 Refresh() 里的翻转重试兜底。
    const int observed = gpio_get_level(busy_pin_);
    ESP_LOGW(kTag,
             "BUSY 未按配置极性回到空闲(%d)，实测电平 %d；按反相处理",
             cfg_idle, observed);
    return observed;
}

void Ssd1619Display::InitializePanel() {
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
    // 自校验：SWRESET 一定会让面板先忙、后回到空闲。若等不到空闲，说明上面
    // 采到的"空闲电平"其实是忙电平（极性判反），翻转后再复位重试一次。
    // 这里用 1 秒短超时——默认 10 秒一旦判反会把启动整整卡住 10 秒。
    if (!WaitBusy(1000)) {
        busy_idle_level_ = busy_idle_level_ ? 0 : 1;
        ESP_LOGW(kTag, "SWRESET 后等不到空闲电平，翻转 BUSY 极性为 %d 并重试",
                 busy_idle_level_);
        HardwareReset();
        Command(0x12);
        vTaskDelay(pdMS_TO_TICKS(20));
        WaitBusy(1000);
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
    Command(0x22, &power_on, 1);
    Command(0x20);
    // 严格保持 2026-11-04 (eafe223) 的写法：固定等 20 ms，再等 BUSY 回空闲。
    // 不要在这里加「超时即 MarkPanelFaulted / return」—— BUSY 极性判反或该脚
    // 浮空都会让等待超时，但那不代表屏坏，锁存只会把一次超时放大成静默不刷。
    vTaskDelay(pdMS_TO_TICKS(20));
    // 同上，避免默认 10 秒超时把初始化卡死。
    WaitBusy(2000);
    panel_initialized_ = true;
    // 记下初始化完成时的共享 RST 序号，供 RefreshOnce 判断自己是否被别人复位过。
    rst_epoch_seen_ = SharedResetEpoch();
    ESP_LOGI(kTag, "SSD1619 analogue power/init sequence complete");
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

// 三色后端：把这一帧当作三色屏的 B/W 平面下发，红平面恒 0（不显示红）。
// frame 的位含义与三色的 bw_plane 一致：1=白、0=黑，行优先 MSB 左。
void Ssd1619Display::RefreshViaBwr(const uint8_t* data) {
    if (bwr_backend_ == nullptr || data == nullptr) return;

    if (bwr_red_plane_ == nullptr || bwr_last_frame_ == nullptr) {
        bwr_red_plane_ = static_cast<uint8_t*>(
            heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        bwr_last_frame_ = static_cast<uint8_t*>(
            heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (bwr_red_plane_ == nullptr || bwr_last_frame_ == nullptr) {
            ESP_LOGE(kTag, "三色后端缓冲分配失败（%u 字节 ×2）",
                     static_cast<unsigned>(kFrameBytes));
            return;
        }
        std::memset(bwr_red_plane_, 0x00, kFrameBytes);  // 全 0 = 不显示红
    }

    // 三色屏没有局部快刷波形，一次全刷要跑满 20~30 秒。LVGL 每次重绘都会走到
    // 这里，若不去重，刷新请求会无限积压、屏永远停在半路。
    // 内容没变就什么都不做——这是让 28 秒全刷与 LVGL 高频重绘共存的前提。
    if (bwr_frame_valid_ && std::memcmp(bwr_last_frame_, data, kFrameBytes) == 0) {
        return;
    }
    std::memcpy(bwr_last_frame_, data, kFrameBytes);
    bwr_frame_valid_ = true;

    ESP_LOGI(kTag, "A 座三色屏全刷开始（约 20~30 s）");
    const bool ok = bwr_backend_->ShowFrame(data, bwr_red_plane_);
    if (ok) {
        ESP_LOGI(kTag, "A 座三色屏全刷完成");
    } else {
        ESP_LOGE(kTag, "A 座三色屏全刷失败");
        // 失败时作废副本，同一画面下次请求还能重试。
        bwr_frame_valid_ = false;
    }
}

void Ssd1619Display::Refresh(const uint8_t* data, bool partial) {
    // 三色后端优先：A 座上的控制器已经不是 SSD1619 了，下面所有
    // InitializePanel / 0x24 / 0x26 / 0x22=0xF7 都不能发出去，
    // 必须在这把 EpaperBusGuard 之前转走（ShowFrame 内部自己加锁）。
    if (bwr_backend_ != nullptr) {
        RefreshViaBwr(data);
        return;
    }
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

    // 两次都失败：高度怀疑 BUSY 空闲电平判反（症状就是"发 0x20 后 BUSY 纹丝
    // 不动"）。翻转极性后再赌最后一次，让这类板子能自愈而不是直接锁存。
    // 翻转后仍失败才真正判故障，正常板子不会走到这里。
    busy_idle_level_ = busy_idle_level_ >= 0 ? (busy_idle_level_ ? 0 : 1)
                                             : (EPD_BUSY_LEVEL ? 0 : 1);
    ESP_LOGW(kTag, "BUSY 空闲电平疑似判反，翻转为 %d 后重试最后一次",
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
    // 到这里才是"这条 BUSY 线确实没有任何响应"的实锤：一次硬复位重试 + 一次
    // 极性翻转都失败过。置位后进入长退避，避免每 700 ms 空转复位一次。
    busy_stuck_ = true;
    MarkPanelFaulted();
}

bool Ssd1619Display::RefreshOnce(const uint8_t* data, bool partial) {
    // RST(GPIO11) 与 FPC2 共用：FPC2 每次初始化/刷新都会把本屏一起复位，
    // 0x22/0x0C/0x01 写进去的升压与 LUT 配置随之清空。此时若仍然认为
    // panel_initialized_ == true 就直接发 0x24/0x26/0x20，波形只会空转：
    // BUSY 照翻、日志照打 "Full refresh complete"，玻璃一个墨点都不动。
    // 这正是「A 屏只有上电首帧可见、之后永远停在旧图」的真正原因。
    if (panel_initialized_ && SharedResetEpoch() != rst_epoch_seen_) {
        ESP_LOGW(kTag,
                 "共享 RST 被 FPC2 改动（epoch %u→%u），重新初始化 A 屏后再刷",
                 rst_epoch_seen_, SharedResetEpoch());
        panel_initialized_ = false;
        base_frame_valid_ = false;
        partial_refresh_count_ = 0;
    }
    if (!panel_initialized_) InitializePanel();
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
        // 本次是自己拉的复位，紧接着会重发 partial LUT，不算「被 FPC2 复位」。
        rst_epoch_seen_ = SharedResetEpoch();
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
        InitializePanel();
        if (panel_faulted_) return false;
    }

    // Restore the full-refresh settings after a partial update.
    const uint8_t full_border = 0x05;
    Command(0x3c, &full_border, 1);
    const uint8_t full_ram_option[] = {0x40, 0x00};
    Command(0x21, full_ram_option, sizeof(full_ram_option));
    SetRamWindow();
    Command(0x24);
    Data(data, kFrameBytes);
    // 0x26 是控制器的「旧画面」参考平面。
    //   同相（写 data）：若面板是差分波形，整屏判为「无变化」，波形照跑、
    //                    BUSY 照翻、日志照打 Full refresh complete，玻璃不动。
    //                    只有上电第一帧能看见（0x26 还是残留值，全屏算变化）。
    //   反相（写 ~data）：每个像素都判为变化，按目标极性全量驱动。
    // invert_old_plane_ 默认反相；纯色自检会交替两种写法做现场判定。
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
    ESP_LOGI(kTag, "全刷 0x26 旧平面使用%s数据",
             invert_old_plane_ ? "反相" : "同相");
    const uint8_t update = 0xf7;
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
    ESP_LOGI(kTag, "Full refresh complete");
    return true;
}

void Ssd1619Display::RawFrameRefresh(const uint8_t* frame, bool partial) {
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
            ESP_LOGI(kTag, "LVGL frame ready: black_pixels=%u/%u",
                     static_cast<unsigned>(black_bits),
                     static_cast<unsigned>(DISPLAY_WIDTH * DISPLAY_HEIGHT));
            // Cross-page navigation performs the cleaning full refresh. Keep
            // all same-page conversation/menu/book updates partial so a long
            // response or reading session never flashes every N updates.
            use_partial = use_partial && base_frame_valid_;
            Refresh(tx_frame_, use_partial);
        }
    }
}

#endif  // !CONFIG_EPD_PANEL_UC8253_37
