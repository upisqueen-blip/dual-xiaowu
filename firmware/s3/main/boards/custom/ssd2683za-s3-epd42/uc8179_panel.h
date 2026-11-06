#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/spi_master.h>

// 5.83" 电子纸（648x480）UC8179 驱动。
//
// 适用面板（2026-11-06 实机替换）：汉朔价签拆机屏
//   A 屏 FPC 丝印 WF0583?Z09 / 黑白快刷，648x480，24pin 0.5mm FPC
//   B 屏 FPC 丝印 FPC-8601B / 黑白红三色，648x480，24pin 0.5mm FPC
//   两颗都是威峰（WF）模组，驱动 IC = UC8179（同规格公开料号：
//   Waveshare 5.83inch e-Paper(B) = WF0583CZ09、Good Display GDEY0583Z31、
//   Seeed 5.83" Mono，均标注 UC8179）。
//
// 与旧 SSD1619/SSD1683 驱动的根本区别（**命令集完全不兼容**）：
//   上电      0x04(PON)                  （旧：0x22=0xC0 + 0x20）
//   写图像    0x10(旧/黑白) 0x13(红)      （旧：0x24 / 0x26）
//   触发刷新  0x12                       （旧：0x20）
//   分辨率    0x61 <- 02 88 01 E0          （旧：0x01 MUX）
//   BUSY      低=忙、高=空闲               （旧 SSD1619 是 高=忙）
//   LUT       用面板内置 OTP 波形，不写 LUT（官方例程同样不写）
//
// 帧格式（与项目现有约定一致）：
//   bw  平面：1=白、0=黑，行优先，MSB 在左，648/8=81 字节/行，共 38880 字节
//   red 平面：默认 1=红、0=非红（可用 SetRedPlaneInvert 翻转）——UC8179 官方
//             例程里红平面是取反后发送的，两种极性在不同模组上都出现过，
//             故做成可配置，并用开机自检一次性判定。
class Uc8179Panel {
public:
    enum class Mode {
        kMono,      // 黑白单色（只写 0x10/0x13 同一份数据）
        kTriColor,  // 黑白红三色（0x13 写红色平面）
    };

    static constexpr int kWidth = 648;
    static constexpr int kHeight = 480;
    static constexpr size_t kStride = (kWidth + 7) / 8;        // 81
    static constexpr size_t kPlaneBytes = kStride * kHeight;   // 38880

    Uc8179Panel(gpio_num_t cs_pin, gpio_num_t busy_pin, Mode mode);
    ~Uc8179Panel();

    // 配置 GPIO + 在共享 SPI 总线上挂设备。可在已有总线时安全调用。
    bool Begin(const char* tag);

    // 硬复位 + 面板设置 + 上电（0x04），等待 BUSY 回空闲。
    bool Init();

    // 全刷一帧。red 为 nullptr 时按"无红色"处理（kTriColor 下填非红）。
    bool Show(const uint8_t* bw, const uint8_t* red);

    // 红平面极性：true 表示发送前取反（0=红）。
    void SetRedPlaneInvert(bool invert) { red_invert_ = invert; }
    bool red_plane_invert() const { return red_invert_; }

    // 面板深睡（画面保持，功耗归零）。
    void Sleep();

    int idle_level() const { return idle_level_; }
    bool busy_driven() const { return busy_driven_; }
    // 上一帧波形实测：拉忙延迟 / 忙态时长（ms）。
    uint32_t last_assert_ms() const { return last_assert_ms_; }
    uint32_t last_busy_ms() const { return last_busy_ms_; }
    const char* tag() const { return tag_; }

private:
    gpio_num_t cs_pin_;
    gpio_num_t busy_pin_;
    Mode mode_;
    spi_device_handle_t spi_ = nullptr;
    uint8_t* chunk_ = nullptr;  // DMA 可用的中转缓冲
    const char* tag_ = "UC8179";
    bool initialized_ = false;
    bool red_invert_ = false;

    int idle_level_ = 1;       // UC8179：BUSY 低=忙，高=空闲
    bool busy_driven_ = true;
    uint32_t last_assert_ms_ = 0;
    uint32_t last_busy_ms_ = 0;

    void Reset();
    int DetectIdleLevel();
    bool ProbeBusyDriven();
    bool WaitIdle(uint32_t timeout_ms);
    void Command(uint8_t command);
    void Command(uint8_t command, const uint8_t* data, size_t length);
    void Data(const uint8_t* data, size_t length);
    // 写数据平面；invert=true 时逐字节取反后发送。
    void DataPlane(const uint8_t* data, size_t length, bool invert);
    // 下发 0x12 并统计波形时长；返回 true 表示观察到了 BUSY 忙态。
    bool ActivateAndWait(uint32_t fallback_ms);
};
