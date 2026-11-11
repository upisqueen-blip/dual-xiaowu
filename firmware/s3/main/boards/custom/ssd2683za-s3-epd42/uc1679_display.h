#pragma once

#include <atomic>
#include <functional>

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"

// 三色屏（FPC2）控制器驱动：微雪 4.2" e-Paper B（SSD1683 / UC8176，400x300 BWR）。
//
// 官方手册(4.2"B V2)与 UC8176 数据手册确认：
//   - 控制器 = SSD1683；24-pin FPC 标准脚位 BUSY=9/RES#=10/D/C=11/CS=12/SCK=13/
//     MOSI=14/VDD=15,16/GND=17，与 4 色 SSD2683ZA 完全一致 → 同一座子电气兼容。
//   - 该面板依赖内置波形（OTP LUT），官方驱动**不下发任何 LUT 字节**；之前"显式
//     LUT"反而冲掉内置波形，故本驱动不写 LUT。
//   - 红色 RAM 极性为「1=红、0=非红」。ConvertJpegToBwrPlanes 产出 red 平面已是
//     1=红，故写 RED 平面直接发送、不取反（取反会整屏泛红）；B/W 平面 1=白同理不取反。
//   - 两 IC 变种（OLD 0x10/0x13/0x12 与 NEW 0x24/0x26+0x22/0x20）官方靠读 0x2F
//     区分；本板 SPI 无 MISO 无法读，故运行时先用宏指定变种、再回退另一种。实测本
//     板三色屏用 NEW(0x24/0x26/0x22/0x20) 能成功刷新（OLD 的 0x12 在该面板上不触发
//     刷新），故默认优先 NEW。
//   - **关键坑**：本板三色屏 PON(0x04) 后 BUSY 不一定翻转（与同座 4 色屏不同），它
//     只在刷新波形执行期间才翻转。因此**绝不能**以「PON 后 BUSY 翻转」判面板存在，
//     否则刷新命令永不被发送、屏永远不闪（此前一版即因此回退）。
//
// 驱动模式宏（s3/main/Kconfig.projbuild 或此处覆盖）：
//   0 = 优先 OLD 变种；1 = 优先 NEW 变种（默认，实测可刷新）；二者均会在首选无响应
//   时回退另一种。
#ifndef FPC2_DRIVER_MODE
#define FPC2_DRIVER_MODE 1
#endif

// 硬件接线（docs/eda/Netlist_schematic_main_1_2026-09-15.net）：
//   与 SSD1619 快刷屏共享 SPI(DIN=GPIO6/CLK=GPIO8)、DC=GPIO10、RST=GPIO11；
//   本屏独立 CS=GPIO9、BUSY=GPIO12。两路座子各有独立 GDR/RESE 升压泵，
//   由面板驱动芯片自管理，MCU 不需要电源使能脚。
//
// 使用模型：三色屏无局部快刷波形，每次 ShowFrame() 都执行
// "硬复位 -> 配置(PON+PSR) -> 写 BW/RED -> 全刷 -> 等 BUSY -> 深睡"。深睡后
// 刷新不影响已显示画面，功耗为零。
//
// 帧格式：
//   bw_plane/red_plane 各 15000 字节（400/8 × 300），行优先 MSB 左。
//   bw_plane:  1=白, 0=黑
//   red_plane: 1=红, 0=非红
class Uc1679Display {
public:
    static constexpr int kWidth = FPC2_WIDTH;    // 彩色屏(FPC2)宽
    static constexpr int kHeight = FPC2_HEIGHT;  // 彩色屏(FPC2)高
    static constexpr size_t kStride = (kWidth + 7) / 8;        // 50
    static constexpr size_t kPlaneBytes = kStride * kHeight;   // 15000

    // 引脚可传入：默认驱动 FPC2 座（EPD_COLOR_CS_PIN / EPD_COLOR_BUSY_PIN）。
    // 两块座子插同型号三色屏时，传 EPD_CS_PIN / EPD_BUSY_PIN 即可用同一份驱动
    // 刷另一个座，从而把「座子问题」和「屏问题」彻底分开。
    explicit Uc1679Display(
        std::function<bool()> other_panel_busy = nullptr,
        gpio_num_t cs_pin = EPD_COLOR_CS_PIN,
        gpio_num_t busy_pin = EPD_COLOR_BUSY_PIN);
    ~Uc1679Display();

    // 探测面板是否在座（初始化 + 自动探测命令集是否成功）。
    bool Init();

    bool Available() const { return available_; }

    // 刷新一帧（全刷，阻塞等待 BUSY，期间置位 g_uc1679_refreshing）。
    bool ShowFrame(const uint8_t* bw_plane, const uint8_t* red_plane);

    // 进入深睡（保留 RAM，显示保持）。
    void Sleep();

    // 诊断用：跳过「探测是否成功」的判定，直接允许下发刷新。
    // 本板三色屏 PON 后 BUSY 不一定翻转，探测会把好屏判成"未连接"，从而
    // 连刷新命令都不发。硬件自检必须绕开这个判定，只看玻璃有没有变化。
    void ForceAvailable() {
        available_ = true;
        if (variant_ < 0) variant_ = FPC2_DRIVER_MODE;
    }

    static bool IsRefreshing() { return g_uc1679_refreshing.load(); }

private:
    static constexpr size_t kDmaChunkBytes = 4096;

    // ---- SSD1683 / UC8176 命令（两变种共用 + 各自专属）----
    static constexpr uint8_t k83PanelSetting   = 0x00;  // PSR
    static constexpr uint8_t k83PowerOff       = 0x02;  // 关闭电荷泵
    static constexpr uint8_t k83PowerOn        = 0x04;  // 上电(PON)
    static constexpr uint8_t k83DeepSleep      = 0x07;  // 深睡
    static constexpr uint8_t k83DataStartBw    = 0x10;  // B/W RAM 数据传输(OLD)
    static constexpr uint8_t k83DataStartRed   = 0x13;  // RED RAM 数据传输(OLD)
    static constexpr uint8_t k83MasterActivate = 0x12;  // 启动刷新(OLD)
    static constexpr uint8_t k83Cdi            = 0x50;  // VCOM/数据间隔（深睡用）
    // 注：本驱动依赖面板内置 OTP 波形，不写任何 LUT，故无 LUT 常量。

    // ---- SSD1683 NEW 变种命令（0x24/0x26/0x22+0x20）----
    static constexpr uint8_t k81SwReset        = 0x12;  // 软复位（OLD 的 0x12 是刷新）
    static constexpr uint8_t k81Border         = 0x3C;  // 边框
    static constexpr uint8_t k81TempSensor     = 0x18;  // 内置温度传感器
    static constexpr uint8_t k81DataEntry      = 0x11;  // RAM 数据入口模式
    static constexpr uint8_t k81RamX           = 0x44;  // RAM X 地址窗口
    static constexpr uint8_t k81RamY           = 0x45;  // RAM Y 地址窗口
    static constexpr uint8_t k81UpdateCtrl2    = 0x22;  // 显示更新控制 2
    static constexpr uint8_t k81MasterActivate = 0x20;  // 主激活（启动波形）
    static constexpr uint8_t k81DataBw         = 0x24;  // B/W RAM 写
    static constexpr uint8_t k81DataRed        = 0x26;  // RED RAM 写
    static constexpr uint8_t k81SetRamXAddr    = 0x4E;  // RAM X 地址计数器
    static constexpr uint8_t k81SetRamYAddr    = 0x4F;  // RAM Y 地址计数器
    static constexpr uint8_t k81DeepSleep      = 0x10;  // 深睡（OLD 的 0x10 是 BW 数据）

    spi_device_handle_t spi_ = nullptr;
    uint8_t* dma_buffer_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    std::function<bool()> other_panel_busy_;
    gpio_num_t cs_pin_ = EPD_COLOR_CS_PIN;
    gpio_num_t busy_pin_ = EPD_COLOR_BUSY_PIN;
    bool available_ = false;
    bool panel_awake_ = false;
    int busy_idle_level_ = -1;   // 复位后 BUSY 静态电平
    int busy_active_level_ = -1;  // 忙时有效电平（= 空闲电平取反）
    int variant_ = -1;  // 0=OLD, 1=NEW, -1=未探测

    void InitializeGpio();
    void InitializeSpiDevice();
    void HardwareReset();
    // 复位后采样 BUSY 空闲电平并确定有效极性（自适应，对齐 4 色驱动）。
    int DetectIdleLevel();
    bool WaitBusy(uint32_t timeout_ms);
    // 复位前等待 SSD1619 快刷屏完成当前波形（共享 RST 仲裁）。
    void WaitForOtherPanelIdle();
    void Command(uint8_t command, const uint8_t* data = nullptr,
                 size_t length = 0, bool invert_data = false);
    void Data(const uint8_t* data, size_t length);

    // 复位 + 上电(PON) + 面板设置(PSR)，variant 选择 OLD/NEW 前缀。
    bool ConfigurePanel(int variant);
    // 写 B/W 与 RED 两平面（RED 取反）。返回 true 表示已下发。
    bool SendImagePlanes(int variant, const uint8_t* bw, const uint8_t* red);
    // 启动刷新并等待 BUSY 回落；返回 true 表示面板跑了波形。
    bool ActivateAndWait(int variant,
                         uint32_t busy_timeout_ms = 20000);

    static std::atomic<bool> g_uc1679_refreshing;
};
