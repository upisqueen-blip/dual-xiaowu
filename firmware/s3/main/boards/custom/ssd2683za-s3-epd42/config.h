#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ES8311 + NS4150B pins from 8311_main_1.
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_13
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_14
#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_21
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_18
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_16
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_15
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_17
// PA (NS4150B CTRL) is connected to GPIO38 and pulled down by R27=100k.
// GPIO38 is a usable GPIO on ESP32-S3R8; Octal PSRAM uses GPIO33-GPIO37.
// Keep PA control outside esp_codec_dev and hold it high continuously. This
// matches the standalone board test and avoids the codec input state toggling
// the amplifier while WakeNet keeps the ADC open.
#define AUDIO_CODEC_PA_CTRL_PIN GPIO_NUM_38
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_NC
#define AUDIO_CODEC_PA_VOLTAGE  3.3f
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#elif CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
// MAX98357A speaker + ZTS6216/ICS-43434 digital microphone from 98357_main_3.
// The speaker and microphone use independent clocks/controllers. The
// microphone LR pin is tied low, selecting its left I2S slot.
#define AUDIO_SPK_I2S_GPIO_BCLK GPIO_NUM_15
#define AUDIO_SPK_I2S_GPIO_WS   GPIO_NUM_16
#define AUDIO_SPK_I2S_GPIO_DOUT GPIO_NUM_14
#define AUDIO_SPK_SD_MODE_PIN   GPIO_NUM_13
#define AUDIO_MIC_I2S_GPIO_SCK  GPIO_NUM_21
#define AUDIO_MIC_I2S_GPIO_WS   GPIO_NUM_17
#define AUDIO_MIC_I2S_GPIO_DIN  GPIO_NUM_18
#else
#error "Select one SSD2683ZA audio hardware configuration"
#endif

#define KEY_UP_GPIO       GPIO_NUM_0
#define KEY_DOWN_GPIO     GPIO_NUM_46
#define KEY_OK_GPIO       GPIO_NUM_45
#define BOOT_BUTTON_GPIO  KEY_UP_GPIO

// VBAT 经分压电阻网络采样到 GPIO7 = ADC1_CH6。
// 电池电压 = ADC引脚电压 * (上电阻 + 下电阻) / 下电阻
//   - 100k / 100k -> 2:1（4.2V 满电时引脚 2.1V）
//   - 200k / 100k -> 3:1（4.2V 满电时引脚 1.4V，更省电、更安全）
// 若实际板子用 200k 上电阻，把 BATTERY_UPPER_RESISTOR 改成 200.0f 即可。
// 开机后可看 BatteryAdc 日志（ADC pin=xxmV -> battery=xxmV -> xx%）核对。
#define BATTERY_ADC_UNIT       ADC_UNIT_1
#define BATTERY_ADC_CHANNEL    ADC_CHANNEL_6
#define BATTERY_UPPER_RESISTOR 100.0f
#define BATTERY_LOWER_RESISTOR 100.0f

// 24-pin e-paper connector mapping from the board schematic:
//   DIN  (MOSI) -> GPIO6
//   CLK  (SCK)  -> GPIO8
//   CS2 (FPC1)  -> GPIO39
//   DC          -> GPIO10
//   RST         -> GPIO11
//   BUSY2 (FPC1)-> GPIO40
//
// 双排座（docs/eda/Netlist_schematic_main_1_2026-09-15.net）：
//   两个 24pin 座共享 DIN/CLK/DC/RST，CS 与 BUSY 独立：
//     FPC1: CS2=GPIO39, BUSY2=GPIO40  -> 黑白快刷屏 SSD1619（对话信息屏）
//     FPC2: CS1=GPIO9,  BUSY1=GPIO12  -> 彩色屏（menuconfig 选 3-color SSD1683
//                                      BWR 或 4-color SSD2683ZA BWRY；相册/InkSight）
//   每路座子各配独立 GDR/RESE 升压泵（面板驱动芯片自管理，无需 MCU 控制）。
// GPIO6/8/9/10/11/12 and the PA control on GPIO38 are available GPIOs on
// this ESP32-S3R8 board configuration.
#define EPD_SPI_NUM   SPI3_HOST
#define EPD_MOSI_PIN  GPIO_NUM_6
#define EPD_SCK_PIN   GPIO_NUM_8
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_RST_PIN   GPIO_NUM_11

// ---- FPC1：黑白快刷屏 SSD1619（GetDisplay() 主屏，对话 + 学习信息）----
#define EPD_CS_PIN    GPIO_NUM_39
#define EPD_BUSY_PIN  GPIO_NUM_40

// ---- FPC2：彩色图像屏（menuconfig 选 3-color SSD1683 BWR 或 4-color SSD2683ZA
//      BWRY；相册 / InkSight，驱动随 FPC2_PANEL_COLORS 切换）----
#define EPD_COLOR_CS_PIN   GPIO_NUM_9
#define EPD_COLOR_BUSY_PIN GPIO_NUM_12

// Connector aliases used by the SSD1619 probe/recovery logic.
#define EPD_CS_PIN_FPC1    EPD_CS_PIN
#define EPD_BUSY_PIN_FPC1  EPD_BUSY_PIN
#define EPD_CS_PIN_FPC2    EPD_COLOR_CS_PIN
#define EPD_BUSY_PIN_FPC2  EPD_COLOR_BUSY_PIN

// 双屏配置开关：1 = FPC1 快刷屏 + FPC2 SSD2683ZA 四色屏同时驱动；
// 0 = 仅快刷屏（旧行为，三色屏代码不初始化）。
#ifndef EPD_ENABLE_COLOR_SCREEN
#define EPD_ENABLE_COLOR_SCREEN 1
#endif

// 保留历史选项：1 = 在面板不支持黄色时把黄色标记映射到红色；
// SSD2683ZA BWRY 驱动保留黄/红区别，不使用此降级开关。
#ifndef EPD_YELLOW_AS_RED
#define EPD_YELLOW_AS_RED 1
#endif
//
// Display and TF card use separate SPI hosts.
#define EPD_SPI_BUS_MAX_SZ 8192
// SSD1619A drives BUSY high while an operation is in progress.
#define EPD_BUSY_LEVEL 1

// XKTF-015-G microSD socket, verified from both schematic and PCB PAD_NET.
// The socket is wired for SDSPI (DAT1/DAT2 are intentionally unused).
#define SD_SPI_NUM   SPI2_HOST
#define SD_CS_PIN    GPIO_NUM_1
#define SD_MISO_PIN  GPIO_NUM_2
#define SD_SCK_PIN   GPIO_NUM_4
#define SD_MOSI_PIN  GPIO_NUM_5

// ---- 双屏面板尺寸（menuconfig 可选 4.2" / 5.83"，FPC1 与 FPC2 独立）----
// FPC1：黑白快刷信息屏 SSD1619（主屏，对话 + 学习）；
// FPC2：彩色图像屏（menuconfig 选 3-color SSD1683 / 4-color SSD2683ZA；相册 / InkSight）。
#if defined(CONFIG_FPC1_PANEL_583)
#define FPC1_WIDTH  648
#define FPC1_HEIGHT 480
#else
#define FPC1_WIDTH  400
#define FPC1_HEIGHT 300
#endif

#if defined(CONFIG_FPC2_PANEL_583)
#define FPC2_WIDTH  648
#define FPC2_HEIGHT 480
#else
#define FPC2_WIDTH  400
#define FPC2_HEIGHT 300
#endif

#if defined(CONFIG_FPC2_PANEL_COLORS_3)
#define FPC2_COLOR_COUNT 3
#else
#define FPC2_COLOR_COUNT 4
#endif

// 主屏 = FPC1（黑白快刷），保持现有 DISPLAY_* 宏语义不变，现有 UI / SSD1619
// 代码无需改动（默认 4.2" 时 DISPLAY == 400x300，与改动前完全一致）。
#define DISPLAY_WIDTH  FPC1_WIDTH
#define DISPLAY_HEIGHT FPC1_HEIGHT

// FPC2 彩色屏衍生尺寸宏（相册 / InkSight / 色彩打包使用，避免与 FPC1 耦合）。
#define FPC2_STRIDE        ((FPC2_WIDTH + 7) / 8)
#define FPC2_PLANE_BYTES   (FPC2_STRIDE * FPC2_HEIGHT)   // 1bpp 平面
#define FPC2_FRAME_BYTES   (FPC2_WIDTH * FPC2_HEIGHT / 4) // 2bpp 彩色帧

// ---------------------------------------------------------------------------
// InkSight /api/render 后端地址解析（自动选路，免手动配置）。
// 设备联网后按以下顺序确定后端，无需在每台设备手动配网（见
// InkSightClient::ResolveServerUrl）：
//   1) NVS 手工覆盖（KEY- 配网 / MCP set_inksight_server 写入的 server_url）：
//      仅当该地址当前可达时才优先使用；
//   2) 编译期候选列表 INKSIGHT_SERVER_CANDIDATES：按各办公点内网 IP + 常用
//      端口探测（GET /health 返回 2xx 即视为真后端），第一个可达者胜出；
//   3) 兜底：生产环境单一域名 INKSIGHT_PRODUCTION_SERVER_URL。
// 换办公点 / 内网 IP 漂移时，只要候选列表覆盖对应网段且设备与后端 PC 处于
// 同一局域网，设备即自动连上，不必每次改 NVS。新增办公点只需在候选列表追加。
// 注意：本机后端当前监听 8092（run_backend.bat 启动 xiaowu_platform.main:app）；
// 8104 为历史端口（旧 dual 控制台），仅作兜底。候选列表把当前实际端口放前面，
// 避免设备每次联网都先去连一个已停用的 8104 而刷一排连接失败日志。
// ---------------------------------------------------------------------------
#ifndef INKSIGHT_DEFAULT_SERVER_URL
#  define INKSIGHT_DEFAULT_SERVER_URL  "http://192.168.1.4:8092"
#endif

// 各办公点内网后端候选（IP:端口，逗号分隔）。设备联网后逐个探测 /health，
// 首个返回 2xx 的即作为后端。当前端口 8092 前置，8104 仅作兜底以兼容旧后端。
// 换办公点时把该办公点"电脑 LAN IP:端口"加进来即可。
#ifndef INKSIGHT_SERVER_CANDIDATES
#  define INKSIGHT_SERVER_CANDIDATES                                       \
    "http://192.168.1.4:8092,http://192.168.1.4:8104,"                    \
    "http://192.168.101.69:8092,http://192.168.101.69:8104"
#endif

// 生产环境单一域名（正式出货 / 非测试环境最终落点，单域名）。
#ifndef INKSIGHT_PRODUCTION_SERVER_URL
#  define INKSIGHT_PRODUCTION_SERVER_URL  "http://nas.insane.vip:8090"
#endif

// Restricted credential accepted only by the product backend's LightAcade
// ticket endpoint. Set this at build time; never put the Partner API key here.
#ifndef LIGHTACADE_PROXY_TOKEN
#  define LIGHTACADE_PROXY_TOKEN "lap_23f7e4f23e1c9c2cd47d7aa0345c80847b77540d"
#endif

#endif
