#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ES8311 + NS4150B pins from the "s3单屏幕+外接50pin" schematic.
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
//     FPC2: CS1=GPIO9,  BUSY1=GPIO12  -> 三色屏 UC1679（InkSight 模式屏）
//   每路座子各配独立 GDR/RESE 升压泵（面板驱动芯片自管理，无需 MCU 控制）。
// GPIO6/8/9/10/11/12 and the PA control on GPIO38 are available GPIOs on
// this ESP32-S3R8 board configuration.
#define EPD_SPI_NUM   SPI3_HOST
#define EPD_MOSI_PIN  GPIO_NUM_6
#define EPD_SCK_PIN   GPIO_NUM_8
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_RST_PIN   GPIO_NUM_11

// ---- FPC1：黑白快刷屏 SSD1619（GetDisplay() 主屏，语音对话 + 学习内容）----
#define EPD_CS_PIN    GPIO_NUM_39
#define EPD_BUSY_PIN  GPIO_NUM_40

// ---- FPC2：三色屏 UC1679（InkSight 模式画面，黑白红）----
#define EPD_COLOR_CS_PIN   GPIO_NUM_9
#define EPD_COLOR_BUSY_PIN GPIO_NUM_12

// 双屏配置开关：1 = 快刷屏 + 三色屏同时驱动（双排座双屏）；
// 0 = 仅快刷屏（旧行为，三色屏代码不初始化）。
#ifndef EPD_ENABLE_COLOR_SCREEN
#define EPD_ENABLE_COLOR_SCREEN 1
#endif

// 三色屏（黑白红）没有黄色。InkSight 后端 2bpp 流中的黄(2)像素映射：
// 1 = 显示为红色（默认，内容醒目）；0 = 丢弃为白色。
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

// Active panel: SSD1619 4.2-inch black/white, native 400 x 300 RAM.
#define DISPLAY_WIDTH  400
#define DISPLAY_HEIGHT 300

// ---------------------------------------------------------------------------
// InkSight /api/render backend. Leave empty to configure at runtime via
// the self.epaper.set_inksight_server(...) MCP tool. Otherwise fill with a
// reachable base URL, e.g. "https://dual.xiaowuai.vip/backend" (no trailing
// slash, no /api/... suffix). The runtime MCP setting overrides this macro.
// ---------------------------------------------------------------------------
#ifndef INKSIGHT_DEFAULT_SERVER_URL
#  define INKSIGHT_DEFAULT_SERVER_URL  "https://dual.xiaowuai.vip/backend"
#endif

// Restricted credential accepted only by the product backend's LightAcade
// ticket endpoint. Set this at build time; never put the Partner API key here.
#ifndef LIGHTACADE_PROXY_TOKEN
#  define LIGHTACADE_PROXY_TOKEN "lap_23f7e4f23e1c9c2cd47d7aa0345c80847b77540d"
#endif

#endif
