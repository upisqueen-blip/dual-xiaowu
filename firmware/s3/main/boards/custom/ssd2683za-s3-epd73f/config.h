#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

// ---------------------------------------------------------------------------
// 板型：ssd2683za-s3-epd73f（三屏幕版 main_2，2026-09-23 网表）
//
// 与 ssd2683za-s3-epd42（双屏板）的区别：
//   - 新增 FPC3（8pin 座，HC-FPC-05-09-8RLTAG），经 8pin 转 50pin 转接板
//     接 7.3 寸七彩电子纸（Waveshare 7.3" e-Paper (F)，800x480，7 色，
//     全刷约 35s）。本板型以该七彩屏为主屏。
//   - 网表要点（3屏驱动_main_2_2026-09-23.net）：
//       FPC3-1 = BUSY1(GPIO12)  FPC3-2 = RST(GPIO11)   FPC3-3 = DC(GPIO10)
//       FPC3-4 = CS1(GPIO9)     FPC3-5 = CLK(GPIO8)    FPC3-6 = DIN(GPIO6)
//       FPC3-7 = VDD3V3         FPC3-8 = GND
//     即 FPC3 与 FPC2（24pin 快刷屏座）并联在同一组信号上：
//     七彩屏占用 CS1/BUSY1，插上七彩屏时 FPC2 座必须空置，否则两个面板的
//     CS/BUSY 直接短接、总线冲突。
//   - 音频两套方案可选（Kconfig: SSD2683ZA_AUDIO_HARDWARE）：
//       ES8311 + NS4150B        （8311_main_1，I2C 编解码器）
//       MAX98357A + ICS-43434   （98357_main_3，纯 I2S，默认）
//     引脚随选型切换，见下方 AUDIO_* 定义。
// ---------------------------------------------------------------------------

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
// ---- ES8311 编解码器 + NS4150B 功放（8311_main_1，与 epd42 板一致）----
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_13
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_14
#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_21
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_18
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_16
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_15
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_17
// NS4150B CTRL 接 GPIO38（R27=100k 下拉），常高使能，不交给 esp_codec_dev
// 切换，避免唤醒词常开 ADC 时被关掉。
#define AUDIO_CODEC_PA_CTRL_PIN GPIO_NUM_38
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_NC
#define AUDIO_CODEC_PA_VOLTAGE  3.3f
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#else
// ---- MAX98357A 功放 + ICS-43434 数字麦克风（main_2 与 main_3 引脚一致）----
// 喇叭与麦克风使用独立 I2S 控制器；麦克风 LR 引脚接地，取左时隙。
#define AUDIO_SPK_I2S_GPIO_BCLK GPIO_NUM_15
#define AUDIO_SPK_I2S_GPIO_WS   GPIO_NUM_16
#define AUDIO_SPK_I2S_GPIO_DOUT GPIO_NUM_14
#define AUDIO_SPK_SD_MODE_PIN   GPIO_NUM_13
#define AUDIO_MIC_I2S_GPIO_SCK  GPIO_NUM_21
#define AUDIO_MIC_I2S_GPIO_WS   GPIO_NUM_17
#define AUDIO_MIC_I2S_GPIO_DIN  GPIO_NUM_18
#endif

#define KEY_UP_GPIO       GPIO_NUM_0
#define KEY_DOWN_GPIO     GPIO_NUM_46
#define KEY_OK_GPIO       GPIO_NUM_45
#define BOOT_BUTTON_GPIO  KEY_UP_GPIO

// VBAT 经分压电阻网络采样到 GPIO7 = ADC1_CH6（与双屏板一致）。
#define BATTERY_ADC_UNIT       ADC_UNIT_1
#define BATTERY_ADC_CHANNEL    ADC_CHANNEL_6
#define BATTERY_UPPER_RESISTOR 100.0f
#define BATTERY_LOWER_RESISTOR 100.0f

// 24pin/8pin 电子纸连接器共享信号（DIN/CLK/DC/RST 三个座并联）。
#define EPD_SPI_NUM   SPI3_HOST
#define EPD_MOSI_PIN  GPIO_NUM_6
#define EPD_SCK_PIN   GPIO_NUM_8
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_RST_PIN   GPIO_NUM_11

// ---- FPC2/FPC3 信号组：7.3 寸七彩屏（本板型主屏）----
// FPC3-4 与 FPC2-12 同接 CS1，FPC3-1 与 FPC2-9 同接 BUSY1。
#define EPD_CS_PIN    GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_12

// ---- FPC1：三色屏 SSD1683/UC8179 兼容命令集座（本板型固件不驱动，
//      引脚定义保留给后续 FPC1+七彩双屏组合扩展）----
#define EPD_COLOR_CS_PIN   GPIO_NUM_39
#define EPD_COLOR_BUSY_PIN GPIO_NUM_40

// 7.3 寸七彩屏（Waveshare 7.3" e-Paper (F)） BUSY 语义：
// 官方 demo BusyHigh() 在 BUSY=0 期间等待，即忙=0、空闲=1。
// 转接板为无源转接，不应反相；若实测相反，改此宏即可。
#ifndef EPD73F_BUSY_IDLE_LEVEL
#define EPD73F_BUSY_IDLE_LEVEL 1
#endif

// 显示与 TF 卡使用独立 SPI 主机。
#define EPD_SPI_BUS_MAX_SZ 8192

// 主屏：7.3 寸七彩 800x480。
#define DISPLAY_WIDTH  800
#define DISPLAY_HEIGHT 480

// LVGL 聊天界面的最小物理刷新间隔（毫秒）。
// 七彩屏没有局部快刷，每次刷新都是 35s 全刷；界面状态/时钟的连续失效
// 会被节流到该间隔，避免频繁全刷磨损面板。InkSight 看板/测试图/清屏等
// 主动操作不受此限制（强制刷新）。
#ifndef EPD73F_UI_REFRESH_MIN_INTERVAL_MS
#define EPD73F_UI_REFRESH_MIN_INTERVAL_MS (5 * 60 * 1000)
#endif

// ---------------------------------------------------------------------------
// InkSight /api/render backend。留空则运行时用 MCP 工具
// self.epaper.set_inksight_server(...) 配置；也可在此填默认值。
// ---------------------------------------------------------------------------
// 后端主地址 = 线上域名（门户反代到 FastAPI，/health 与 /api/render 均可用）。
// 本地联调时改成 http://<电脑 LAN IP>:8092，或用 MCP 工具
// self.epaper.set_inksight_server 运行时改（会写入 NVS）。
#ifndef INKSIGHT_DEFAULT_SERVER_URL
#  define INKSIGHT_DEFAULT_SERVER_URL  "https://dual.xiaowuai.vip/backend"
#endif

// 主地址连不上时自动回退一次的备用地址。留空则关闭回退。
#ifndef INKSIGHT_FALLBACK_SERVER_URL
#  define INKSIGHT_FALLBACK_SERVER_URL  ""
#endif

// LightAcade 代理令牌（与双屏板一致）。
#ifndef LIGHTACADE_PROXY_TOKEN
#  define LIGHTACADE_PROXY_TOKEN "lap_23f7e4f23e1c9c2cd47d7aa0345c80847b77540d"
#endif

// XKTF-015-G microSD 卡座（与双屏板一致）。
#define SD_SPI_NUM   SPI2_HOST
#define SD_CS_PIN    GPIO_NUM_1
#define SD_MISO_PIN  GPIO_NUM_2
#define SD_SCK_PIN   GPIO_NUM_4
#define SD_MOSI_PIN  GPIO_NUM_5

#endif
