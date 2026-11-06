#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"
#include "epaper_dashboard.h"
#include "lcd_display.h"

class Uc1679Display;

class Ssd1619Display : public LcdDisplay {
public:
    Ssd1619Display();
    ~Ssd1619Display() override;

    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowActivationUi(const char* message, const char* code) override;
    void ShowProvisioningGuide(const char* ssid, const char* url) override;
    void SetXiaowuPairCode(const char* code);
    void HideActivationUi() override;
    void ClearChatMessages() override;
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void ShowNotification(const char* notification,
                          int duration_ms = 3000) override;
    EpaperDashboard& dashboard() { return dashboard_; }

    // 开机首页菜单（口语/阅读…）。默认在构造函数里调用，但 board 可在
    // 已配置 InkSight/双屏时跳过它，让网络图成为开机第一屏，避免双闪。
    void ShowBootHome();

    // 直接用 caller 提供的帧缓冲刷新画面。
    // frame: 50*300=15000 字节，1-bit/行/MSB 左白 1/黑 0。
    // partial=true: 差分快刷；false: 全刷清残影。
    // 刷新成功后，这块外部帧"占住"屏幕：LVGL 仪表盘的周期性重绘不会再刷
    // 上来把它盖掉（实机上表现为"刷完就被仪表盘覆盖，看着像没刷新"）。
    // 按键导航或语音会话会调 ReleaseExternalFrameHold() 交还控制权。
    void RawFrameRefresh(const uint8_t* frame, bool partial);
    void ReleaseExternalFrameHold();

    // 现场诊断：连刷「全黑 / 全白 / 横向黑白条纹」三张纯色图。
    // 三张图肉眼可见地变化 → 面板与波形正常，问题在上游取图/解码；
    // 三张图纹丝不动 → 面板没有驱动电压（升压/供电/换屏），与软件无关。
    void RunPatternSelfTest();

    bool IsPanelBusy() const {
        // BUSY 线已通过上下拉测试判定为浮空时，GPIO 电平不代表面板忙闲。
        // 双屏初始化本身持有 EpaperBusMutex，确保轮到彩屏复位时 A 屏传输已结束；
        // 此处若仍按 GPIO 恒高判断，会让 FPC2 无意义等待 25 秒。
        if (!busy_driven_ || busy_idle_level_ < 0 || panel_faulted_) return false;
        return gpio_get_level(busy_pin_) != busy_idle_level_;
    }

private:
    static constexpr size_t kStride = (DISPLAY_WIDTH + 7) / 8;
    static constexpr size_t kFrameBytes = kStride * DISPLAY_HEIGHT;

    spi_device_handle_t spi_ = nullptr;
    // FPC1 改装为三色面板时，LVGL/dashboard 继续复用此类作画，物理刷新转交 UC1679。
    Uc1679Display* fpc1_color_driver_ = nullptr;
    uint8_t* frame_ = nullptr;
    uint8_t* tx_frame_ = nullptr;
    uint8_t* panel_frame_ = nullptr;
    uint8_t* dma_buffer_ = nullptr;
    // 仅自检使用：为 true 时全刷前多发一条 0x04（Power ON）。
    bool self_test_pon_ = false;
    // 全刷时 0x26（旧画面参考平面）的写法：false = 与 0x24 同一份数据，
    // true = 写新画面的反相。现场自检用它四选一定论：这块 4.2" 的 OTP 全刷
    // 波形到底是绝对驱动（0x26 不参与）还是差分驱动（只驱动 0x24≠0x26 的像素）。
    bool invert_old_plane_ = false;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t refresh_task_ = nullptr;
    bool refresh_pending_ = false;
    bool refresh_partial_pending_ = false;
    bool refresh_force_pending_ = false;
    bool panel_initialized_ = false;
    bool panel_faulted_ = false;
    // 故障锁存的时刻，用于 kPanelFaultRetryMs 后自动复位重试（见 Refresh）。
    int64_t panel_faulted_at_ms_ = 0;
    bool base_frame_valid_ = false;
    uint16_t partial_refresh_count_ = 0;
    bool assistant_response_active_ = false;
    std::string activation_message_;
    std::string xiaozhi_activation_code_;
    std::string xiaowu_pair_code_;
    int busy_idle_level_ = -1;
    // BUSY 引脚在一段时间内完全没有跳变（既不为忙也不回空闲）。这通常不是
    // 极性判反，而是面板根本没驱动这条线：FPC 没插好 / 屏插在另一个座上
    // （CS 与 BUSY 对不上同一块屏）/ 该屏不支持 SSD1619 命令集。置位后不再
    // 做极性翻转，避免在 0↔1 之间来回震荡、每轮白等几秒。
    bool busy_stuck_ = false;
    // BUSY 引脚是否真的被面板驱动（由 DetectPanelConnector 的 IsBusyLineDriven
    // 判定）。浮空=false 时说明 FPC 没插好/断线，此时 WaitRefreshComplete 的
    // 定时兜底只是空转，应判失败让上层重试自愈，而不是假装刷成功把屏钉死。
    bool busy_driven_ = true;
    // 实际在用的 CS / BUSY。DIN/CLK/DC/RST 两个座共用，只有 CS 和 BUSY 是
    // 每个座一份，所以"插错座"等价于这两个引脚选错：CS 不对则面板收不到命令，
    // BUSY 不对则读到的是浮空脚。启动时探测后填真值。
    gpio_num_t cs_pin_ = EPD_CS_PIN;
    gpio_num_t busy_pin_ = EPD_BUSY_PIN;
    bool panel_probed_ = false;
    // 外部帧（InkSight / 双屏 A 便签）已上屏，仪表盘不得抢占，直到按键或会话。
    bool hold_external_frame_ = false;
    EpaperDashboard dashboard_;

    static void FlushCallback(lv_display_t* display, const lv_area_t* area,
                              uint8_t* pixels);
    static void RefreshTaskEntry(void* arg);
    void RefreshTask();
    void RequestRefresh(bool partial, bool force = false);
    void InitializeSpi();
    void HardwareReset();
    bool WaitBusy(uint32_t timeout_ms = 10000);
    bool WaitRefreshComplete(int idle_level, uint32_t timeout_ms = 60000,
                             uint32_t busyless_total_ms = 0);
    void Command(uint8_t command, const uint8_t* data = nullptr, size_t length = 0);
    void Data(const uint8_t* data, size_t length);
    bool InitializePanel();
    // 采样 BUSY 引脚的"空闲电平"。必须等它稳定后再取，否则会把复位期间的
    // 忙电平当成空闲，导致后续 idle->busy->idle 判定整体反向。
    int DetectBusyIdleLevel();
    // 判断屏实际插在哪个座上：主判据是"这条 BUSY 有没有被驱动"（带上拉读一次、
    // 改下拉再读一次，浮空脚会跟着弱上下拉翻转，被面板驱动的脚不会变），
    // 次判据才是复位窗口内的电平跳变。
    //
    // 为什么不能只靠跳变：硬件复位后很多面板的 BUSY 直接就停在空闲电平，600 ms
    // 内根本没有任何边沿可看（历史测试中 FPC2/GPIO12 恒为 0，而 FPC1/GPIO40 因上拉
    // 恒为 1）。只认跳变会把一块好屏判成"没插屏"并锁存，屏幕从此不刷新。
    // 选错座是最常见也最难自查的故障（CS 不对 → 面板收不到命令；BUSY 不对 →
    // 读到浮空脚的恒定电平）。双屏板按硬件定义固定将 SSD1619 驱动到 FPC1。
    bool DetectPanelConnector();
    // 该 BUSY 脚是被面板驱动着，还是浮空（座上没插屏）。
    bool IsBusyLineDriven(gpio_num_t pin);
    // 统一的故障入口：打一条 E 级日志并记录时刻，便于定位"屏幕刷不动"。
    void MarkPanelFaulted();
    void SetRamWindow(int x_start = 0, int x_end = kStride - 1,
                      int y_start = 0, int y_end = DISPLAY_HEIGHT - 1);
    // 单次刷新尝试，返回 false 表示面板没按预期完成波形（不负责故障锁存）。
    bool RefreshOnce(const uint8_t* data, bool partial);
    void Refresh(const uint8_t* data, bool partial);
};
