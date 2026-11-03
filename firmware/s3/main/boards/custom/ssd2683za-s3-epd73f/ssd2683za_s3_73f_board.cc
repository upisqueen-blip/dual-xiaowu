#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <esp_heap_caps.h>

#include "application.h"
// 两套音频方案共用同一套板级代码，靠 Kconfig 选型切换（详见 config.h）。
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
#include "codecs/es8311_audio_codec.h"
#else
#include "codecs/no_audio_codec.h"
#endif
#include "config.h"
#include "epd73f_display.h"
#include "inksight_client.h"
// LightAcadeClient 仍复用 epd42 板级目录里的实现（该目录不在 include 搜索路径）
#include "../ssd2683za-s3-epd42/lightacade_client.h"
#include "mcp_server.h"
#include "../ssd2683za-s3-epd42/sd_card_storage.h"
#include "settings.h"
#include "system_info.h"
#include "wifi_board.h"
#include <wifi_manager.h>

#ifndef INKSIGHT_DEFAULT_SERVER_URL
#  define INKSIGHT_DEFAULT_SERVER_URL ""
#endif

namespace {
constexpr const char* kTag = "Ssd2683zaS373fBoard";
constexpr const char* kInkSightFixBuildId = "2026-11-03-online-domain";
}

// 7.3 寸七彩屏板型（ssd2683za-s3-epd73f，main_2 三屏幕板 + 8P->50P 转接）。
//
// 与双屏 epd42 板的差异：
//   - 主屏 = 7.3" 七彩屏（FPC2/FPC3 信号组，CS=GPIO9/BUSY=GPIO12），
//     无 SSD1619 快刷屏、无 UC1679 三色屏；FPC2 座必须空置。
//   - 音频固定为 MAX98357A + ICS-43434（98357_main_3 引脚映射）。
//   - 内容来源：InkSight 看板（2bpp 彩色流，红/黄真彩显示）、
//     MCP 清屏/色条工具。语音对话的视觉反馈依赖聊天 UI 时需打开
//     EPD73F_ENABLE_CHAT_UI（默认关，七彩屏 35s 全刷不适合逐句刷）。
class Ssd2683zaS373fBoard : public WifiBoard {
private:
    Epd73fDisplay* display_ = nullptr;
    SdCardStorage storage_;
    std::atomic<bool> recording_{false};
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
    i2c_master_bus_handle_t i2c_bus_ = nullptr;  // 仅 ES8311 方案使用
#endif

    // InkSight 看板：三个 1bpp 平面（黑/黄/红），各 48000 字节 PSRAM。
    // 这条是 2bpp（4 色）回落路径，后端为老版本 / 设备未绑定时使用。
    uint8_t* ink_bw_ = nullptr;
    uint8_t* ink_yellow_ = nullptr;
    uint8_t* ink_red_ = nullptr;
    // 4bpp「规范索引」整帧（192000 字节 PSRAM），六色面板完整能力路径：
    // 0=黑 1=白 2=黄 3=红 4=蓝 5=绿。优先走这条，失败再回落上面的三平面。
    uint8_t* ink_frame4_ = nullptr;
    std::atomic<bool> inksight_busy_{false};
    std::atomic<bool> inksight_cancel_{false};  // 长按 OK 取消下载
    std::string inksight_server_;
    std::unique_ptr<LightAcadeClient> lightacade_;
    std::atomic<bool> audio_service_hijacked_{false};
    // 上一次拉图是否失败（活跃模式下据此缩短重试间隔）。
    std::atomic<bool> inksight_last_failed_{false};
    std::atomic<bool> sleep_mode_pending_{false};
    std::atomic<bool> rebind_refresh_scheduled_{false};
    std::mutex inksight_refresh_task_mutex_;
    bool inksight_refresh_task_pending_ = false;
    bool inksight_refresh_task_rerun_ = false;

    // ── 电源模式：活跃 / 间歇 ──────────────────────────────────────────
    // 语义与 C3 固件（firmware/src/main.cpp 的 localActive / alwaysActive）对齐：
    //   活跃   —— **出厂默认**。Wi-Fi 常在线、刷新更勤（默认 5 分钟）、取图
    //             失败后 1 分钟快速重试；后台改配置/生成内容后屏幕很快就能看到。
    //   间歇   —— 唤醒后在线取图并完成屏幕刷新，随后上报离线并进入深睡；
    //             RTC 定时器或 KEY+ 可唤醒。
    // 长按 KEY+（GPIO0，兼 BOOT）3 秒切换，落在 NVS inksight/power_mode。
    // 键不存在（新烧录 / 清空 NVS）时默认 1=活跃，这就是"出厂默认活跃"。
    enum class PowerMode : int { kIntermittent = 0, kActive = 1 };
    PowerMode power_mode_ = PowerMode::kActive;

    // 长按阈值按按键分开：KEY+ 要 3 秒（切电源模式），KEY-/KEY_OK 维持原 2 秒。
    // 顺序同 KeyScanTask 里的 pins[] = {KEY_UP(+), KEY_OK, KEY_DOWN(-)}。
    static constexpr int64_t kLongPressMsByKey[3] = {3000, 2000, 2000};
    // 活跃 / 间歇各自的默认自动刷新间隔（分钟）。
    static constexpr int kActiveRefreshMinutes = 5;
    static constexpr int kIdleRefreshMinutes = 30;
    // 活跃模式取图失败后的快速重试间隔（分钟）。
    static constexpr int kFastRetryMinutes = 1;

    static const char* PowerModeName(PowerMode mode) {
        return mode == PowerMode::kActive ? "活跃" : "间歇";
    }

    PowerMode LoadPowerMode() {
        Settings s("inksight", /*read_write=*/false);
        return s.GetInt("power_mode", 1) != 0 ? PowerMode::kActive
                                              : PowerMode::kIntermittent;
    }

    void SavePowerMode(PowerMode mode) {
        Settings s("inksight", /*read_write=*/true);
        s.SetInt("power_mode", mode == PowerMode::kActive ? 1 : 0);
    }

    // 本模式下真正生效的自动刷新间隔（分钟）。
    int EffectiveRefreshMinutes() const {
        Settings s("inksight", /*read_write=*/false);
        // color_refresh：服务端 X-Refresh-Minutes 下发，或用户用 MCP 设的值。
        const int configured = s.GetInt("color_refresh", 0);
        if (power_mode_ == PowerMode::kActive) {
            if (configured <= 0) return kActiveRefreshMinutes;
            // 活跃模式可以更勤快，但不比服务端要求的更勤：服务端是按内容更新
            // 节奏下发间隔的，刷更勤只是白白耗电、加速屏老化。
            return std::min(configured, kActiveRefreshMinutes);
        }
        return configured > 0 ? configured : kIdleRefreshMinutes;
    }

    void ApplyPowerMode() {
        const wifi_ps_type_t ps = power_mode_ == PowerMode::kActive
                                      ? WIFI_PS_NONE
                                      : WIFI_PS_MAX_MODEM;
        const esp_err_t err = esp_wifi_set_ps(ps);
        // 构造期调用时 Wi-Fi 还没起来，ESP_ERR_WIFI_NOT_INIT 属正常，不报警。
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(kTag, "Wi-Fi 省电设置失败: %s", esp_err_to_name(err));
        }
        ESP_LOGI(kTag, "电源模式: %s（Wi-Fi %s，自动刷新 %d 分钟）",
                 PowerModeName(power_mode_),
                 ps == WIFI_PS_NONE ? "常在线" : "modem sleep",
                 EffectiveRefreshMinutes());
    }

    void ReportRuntimeMode(PowerMode mode) {
        if (inksight_server_.empty()) return;
        auto* network = GetNetwork();
        if (!network) return;
        if (!WifiManager::GetInstance().IsConnected()) return;
        std::string error;
        if (InkSightClient::ReportRuntimeMode(network, inksight_server_,
                                              SystemInfo::GetMacAddress(),
                                              mode == PowerMode::kActive,
                                              true,
                                              &error)) {
            ESP_LOGI(kTag, "运行模式已上报: %s", PowerModeName(mode));
        } else {
            ESP_LOGW(kTag, "运行模式上报失败: %s", error.c_str());
        }
    }

    void ReportSleepMode() {
        if (inksight_server_.empty() || !GetNetwork() ||
            !WifiManager::GetInstance().IsConnected()) return;
        std::string error;
        if (!InkSightClient::ReportRuntimeMode(
                GetNetwork(), inksight_server_, SystemInfo::GetMacAddress(),
                false, false, &error)) {
            ESP_LOGW(kTag, "间歇模式上报失败: %s", error.c_str());
        } else {
            ESP_LOGI(kTag, "已上报 runtime_mode=interval, awake=false");
        }
    }

    bool ReportDeviceAwake(bool awake) {
        if (inksight_server_.empty() || !GetNetwork() ||
            !WifiManager::GetInstance().IsConnected()) return false;
        std::string error;
        const bool ok = InkSightClient::ReportRuntimeMode(
            GetNetwork(), inksight_server_, SystemInfo::GetMacAddress(),
            power_mode_ == PowerMode::kActive, awake, &error);
        if (!ok) ESP_LOGW(kTag, "设备状态上报(%s)失败: %s",
                          awake ? "awake" : "sleep", error.c_str());
        if (!awake && ok) ESP_LOGI(kTag, "设备已上报离线状态");
        return ok;
    }

    void EnterIntervalSleep() {
        if (power_mode_ != PowerMode::kIntermittent) return;
        // GPIO0 按键若仍被按住，低电平 EXT0 会让芯片立即反复唤醒；
        // 等用户松手，期间继续保持醒着并轮询按键电平。
        while (gpio_get_level(KEY_UP_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (display_->IsPanelBusy()) return;
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConnected()) {
            ReportHeartbeat();
            ReportSleepMode();
        }
        const uint64_t sleep_us = static_cast<uint64_t>(
            std::max(1, EffectiveRefreshMinutes())) * 60ULL * 1000000ULL;
        const esp_err_t timer_err = esp_sleep_enable_timer_wakeup(sleep_us);
        const esp_err_t button_err = esp_sleep_enable_ext0_wakeup(KEY_UP_GPIO, 0);
        if (timer_err != ESP_OK || button_err != ESP_OK) {
            ESP_LOGE(kTag, "无法配置深睡唤醒源: timer=%s button=%s",
                     esp_err_to_name(timer_err), esp_err_to_name(button_err));
            return;
        }
        ESP_LOGI(kTag, "间歇模式进入深度睡眠，%d 分钟后定时唤醒，KEY+ 可提前唤醒",
                 EffectiveRefreshMinutes());
        esp_deep_sleep_start();
    }

    // 长按 KEY+ 3 秒 / MCP self.epaper.set_power_mode 都走这里。
    void SetPowerMode(PowerMode next) {
        if (next == power_mode_) return;
        power_mode_ = next;
        SavePowerMode(next);
        ESP_LOGI(kTag, "电源模式切换为 %s（已写入 NVS，重启保持）",
                 PowerModeName(next));
        ApplyPowerMode();
        ReportRuntimeMode(next);
        if (next == PowerMode::kActive) {
            sleep_mode_pending_.store(false);
            // 切回活跃立刻刷一屏：既让用户马上看到"在线了"，也把新间隔生效。
            Application::GetInstance().Schedule([this]() { StartInkSightRefresh(); });
        } else {
            // 等间歇模式这次取图和物理屏幕刷新都结束，再上报离线并睡眠。
            sleep_mode_pending_.store(true);
            if (!inksight_busy_.load() && !display_->IsPanelBusy())
                Application::GetInstance().Schedule([this]() { StartInkSightRefresh(); });
        }
    }

    void TogglePowerMode() {
        SetPowerMode(power_mode_ == PowerMode::kActive ? PowerMode::kIntermittent
                                                       : PowerMode::kActive);
    }

    void InitializePowerAmplifier() {
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
        // NS4150B CTRL 高电平有效，常开；不交给 esp_codec_dev 随输入/输出
        // 状态切换（否则唤醒词常开时功放会被关掉）。
        ESP_ERROR_CHECK(gpio_reset_pin(AUDIO_CODEC_PA_CTRL_PIN));
        ESP_ERROR_CHECK(gpio_set_direction(AUDIO_CODEC_PA_CTRL_PIN,
                                           GPIO_MODE_INPUT_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(AUDIO_CODEC_PA_CTRL_PIN, 1));
        ESP_LOGI(kTag, "AUDIO PA: NS4150B enabled permanently: GPIO%d pad=%d",
                 AUDIO_CODEC_PA_CTRL_PIN,
                 gpio_get_level(AUDIO_CODEC_PA_CTRL_PIN));
#else
        // MAX98357A SD_MODE 板上带上拉，主动拉高保持功放常开。
        ESP_ERROR_CHECK(gpio_reset_pin(AUDIO_SPK_SD_MODE_PIN));
        ESP_ERROR_CHECK(gpio_set_direction(AUDIO_SPK_SD_MODE_PIN,
                                           GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(AUDIO_SPK_SD_MODE_PIN, 1));
        ESP_LOGI(kTag, "AUDIO PA: MAX98357A enabled: GPIO%d",
                 AUDIO_SPK_SD_MODE_PIN);
#endif
    }

    // ES8311 方案才需要 I2C 总线；MAX98357 方案是纯 I2S，跳过。
    void InitializeI2c() {
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
        ESP_LOGW(kTag, "AUDIO: Starting I2C initialization on SDA=%d SCL=%d...",
                 AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
        i2c_master_bus_config_t config = {};
        config.i2c_port = I2C_NUM_0;
        config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = 1;
        const esp_err_t bus_ret = i2c_new_master_bus(&config, &i2c_bus_);
        if (bus_ret != ESP_OK) {
            ESP_LOGE(kTag, "AUDIO: I2C bus creation FAILED: %s (0x%x)",
                     esp_err_to_name(bus_ret), bus_ret);
            return;
        }
        ESP_LOGI(kTag, "AUDIO: I2C bus created successfully.");
        // esp_codec_dev 用 8 位地址 0x30，IDF 探测接口要 7 位地址 0x18。
        const uint8_t address_7bit = AUDIO_CODEC_ES8311_ADDR >> 1;
        const esp_err_t probe = i2c_master_probe(i2c_bus_, address_7bit, 100);
        if (probe == ESP_OK) {
            ESP_LOGI(kTag, "AUDIO: ES8311 ACK at 0x%02X (SDA=%d SCL=%d)",
                     address_7bit, AUDIO_CODEC_I2C_SDA_PIN,
                     AUDIO_CODEC_I2C_SCL_PIN);
        } else {
            ESP_LOGE(kTag,
                     "AUDIO CODEC PROBE RESULT: FAILURE — ES8311 NO-ACK at "
                     "0x%02X (SDA=%d SCL=%d): %s. 麦克风不可用。",
                     address_7bit, AUDIO_CODEC_I2C_SDA_PIN,
                     AUDIO_CODEC_I2C_SCL_PIN, esp_err_to_name(probe));
        }
#else
        ESP_LOGI(kTag, "AUDIO: MAX98357A + ICS-43434 (I2S), no I2C codec bus");
#endif
    }

    // 把常见底层错误翻译成中文，方便排查。
    static std::string FriendlyInkSightError(const std::string& e) {
        if (e.find("not set") != std::string::npos) return "没配服务器";
        if (e.find("URL not set") != std::string::npos) return "没配服务器";
        if (e.find("err=104") != std::string::npos ||
            e.find("err=0x68") != std::string::npos)
            return "服务端拒连";
        if (e.find("err=110") != std::string::npos) return "连接超时";
        if (e.find("err=111") != std::string::npos) return "端口未监听";
        if (e.find("err=113") != std::string::npos) return "路由不通";
        if (e.find("err=118") != std::string::npos) return "网络未就绪";
        if (e.find("HTTP 429") != std::string::npos) return "请求太快";
        if (e.find("HTTP 502") != std::string::npos) return "后端网关错误";
        if (e.find("HTTP 503") != std::string::npos) return "后端未启动";
        if (e.find("HTTP 504") != std::string::npos) return "后端响应超时";
        if (e.find("HTTP 404") != std::string::npos) return "路径不对";
        if (e.find("HTTP ") == 0) return e;
        if (e.find("stream end early") != std::string::npos)
            return "下载中断(" + e + ")";
        return e;
    }

    // 电池电压（mV）：VBAT 经分压电阻网络进 ADC1_CH6（GPIO7），与官方
    // 固件上报给 /api/render 的 v= 参数同源（后端用来画电量图标）。
    // 采样失败返回 -1（此时不上报电压，后端用默认 3.3V）。
    static int ReadBatteryVoltageMv() {
        static adc_oneshot_unit_handle_t unit = nullptr;
        static adc_cali_handle_t cali = nullptr;
        static bool cali_tried = false;
        static bool failed = false;
        if (failed) return -1;
        if (unit == nullptr) {
            adc_oneshot_unit_init_cfg_t init = {};
            init.unit_id = BATTERY_ADC_UNIT;
            init.ulp_mode = ADC_ULP_MODE_DISABLE;
            if (adc_oneshot_new_unit(&init, &unit) != ESP_OK) {
                failed = true;
                unit = nullptr;
                return -1;
            }
            adc_oneshot_chan_cfg_t chan = {};
            chan.atten = ADC_ATTEN_DB_12;
            chan.bitwidth = ADC_BITWIDTH_DEFAULT;
            if (adc_oneshot_config_channel(unit, BATTERY_ADC_CHANNEL, &chan) !=
                ESP_OK) {
                failed = true;
                return -1;
            }
        }
        if (cali == nullptr && !cali_tried) {
            cali_tried = true;
            adc_cali_curve_fitting_config_t cfg = {};
            cfg.unit_id = BATTERY_ADC_UNIT;
            cfg.chan = BATTERY_ADC_CHANNEL;
            cfg.atten = ADC_ATTEN_DB_12;
            cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
            if (adc_cali_create_scheme_curve_fitting(&cfg, &cali) != ESP_OK) {
                cali = nullptr;  // 无 eFuse 校准数据时走粗算
            }
        }
        int raw = 0;
        if (adc_oneshot_read(unit, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            return -1;
        }
        int pin_mv = 0;
        if (cali != nullptr) {
            if (adc_cali_raw_to_voltage(cali, raw, &pin_mv) != ESP_OK) return -1;
        } else {
            pin_mv = raw * 3100 / 4095;  // 12dB 衰减下的粗略斜率
        }
        const double ratio = (static_cast<double>(BATTERY_UPPER_RESISTOR) +
                              BATTERY_LOWER_RESISTOR) /
                             BATTERY_LOWER_RESISTOR;
        return static_cast<int>(pin_mv * ratio);
    }

    struct InkSightRefreshTaskArgs {
        Ssd2683zaS373fBoard* board;
        int delay_ms;
    };

    // HTTP 拉图可能阻塞数十秒，必须放到独立任务，不能占用 Application 主事件循环。
    void StartInkSightRefresh(int delay_ms = 0) {
        {
            std::lock_guard<std::mutex> lock(inksight_refresh_task_mutex_);
            if (inksight_refresh_task_pending_) {
                // If the URL/config changed during an in-flight request, don't drop
                // the refresh: run once more afterward with the latest settings.
                inksight_refresh_task_rerun_ = true;
                ESP_LOGI(kTag, "XiaowuAI refresh already running; queue one refresh with latest settings");
                return;
            }
            inksight_refresh_task_pending_ = true;
        }
        auto* task_args = new (std::nothrow) InkSightRefreshTaskArgs{this, delay_ms};
        if (!task_args) {
            std::lock_guard<std::mutex> lock(inksight_refresh_task_mutex_);
            inksight_refresh_task_pending_ = false;
            ESP_LOGE(kTag, "Cannot allocate XiaowuAI refresh task argument");
            return;
        }
        const BaseType_t created = xTaskCreate(
            [](void* argument) {
                auto* args = static_cast<InkSightRefreshTaskArgs*>(argument);
                auto* self = args->board;
                const int wait_ms = args->delay_ms;
                delete args;
                if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
                for (;;) {
                    self->RefreshInkSight();
                    bool rerun = false;
                    {
                        std::lock_guard<std::mutex> lock(self->inksight_refresh_task_mutex_);
                        rerun = self->inksight_refresh_task_rerun_;
                        self->inksight_refresh_task_rerun_ = false;
                        if (!rerun) self->inksight_refresh_task_pending_ = false;
                    }
                    if (!rerun) break;
                }
                vTaskDelete(nullptr);
            },
            "inksight_refresh", 8192, task_args, 3, nullptr);
        if (created != pdPASS) {
            delete task_args;
            std::lock_guard<std::mutex> lock(inksight_refresh_task_mutex_);
            inksight_refresh_task_pending_ = false;
            inksight_refresh_task_rerun_ = false;
            ESP_LOGE(kTag, "Cannot create XiaowuAI refresh task");
        }
    }

    // InkSight 拉取 + 屏幕刷写。优先拉 4bpp 六色整帧（黑/白/黄/红/蓝/绿），
    // 后端不支持时回落 2bpp 三平面（黑/黄/红）。
    // 物理刷新约 12s（六色）/ 35s（七彩），后台任务执行，本函数只等下载完成即返回。
    void RefreshInkSight() {
        // 先抢占刷新锁，再分配大缓冲区；否则并发排队的刷新会重复覆盖
        // 裸指针并造成 PSRAM 泄漏，最终可能触发重启。
        if (inksight_busy_.exchange(true)) {
            ESP_LOGW(kTag, "XiaowuAI refresh already in progress, skip");
            return;
        }
        auto cleanup = [this]() { inksight_busy_.store(false); };

        // 重新配网遗留的待解绑请求（配网那一刻网络不可用、联网回调也没赶上）：
        // 在这里补一次，成功后本轮刷新就会拉到配对码图。
        {
            Settings s("inksight", /*read_write=*/true);
            if (s.GetInt("rebind_pending", 0) != 0) {
                std::string unbind_error;
                if (InkSightClient::UnbindSelf(GetNetwork(), inksight_server_,
                                               SystemInfo::GetMacAddress(),
                                               &unbind_error)) {
                    s.SetInt("rebind_pending", 0);
                    ESP_LOGI(kTag, "XiaowuAI: 已解绑，本次刷新会显示配对码");
                } else {
                    ESP_LOGW(kTag, "XiaowuAI: 解绑仍未成功(%s)",
                             unbind_error.c_str());
                }
            }
        }
        if (ink_bw_ == nullptr) {
            ink_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                InkSightClient::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            ink_yellow_ = static_cast<uint8_t*>(heap_caps_malloc(
                InkSightClient::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            ink_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                InkSightClient::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            ink_frame4_ = static_cast<uint8_t*>(heap_caps_malloc(
                InkSightClient::kFrame4bppBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (ink_bw_ == nullptr || ink_yellow_ == nullptr || ink_red_ == nullptr ||
            ink_frame4_ == nullptr) {
            ESP_LOGE(kTag, "XiaowuAI: PSRAM OOM for frame buffers");
            if (ink_bw_) heap_caps_free(ink_bw_);
            if (ink_yellow_) heap_caps_free(ink_yellow_);
            if (ink_red_) heap_caps_free(ink_red_);
            if (ink_frame4_) heap_caps_free(ink_frame4_);
            ink_bw_ = ink_yellow_ = ink_red_ = ink_frame4_ = nullptr;
            cleanup();
            return;
        }
        inksight_cancel_.store(false);

        ESP_LOGI(kTag, "XiaowuAI: updating 7-color screen...");
        Settings inksight_settings("inksight", /*read_write=*/false);
        std::string persona = inksight_settings.GetString("persona", "");
        if (persona == "auto" || persona == "AUTO") {
            persona.clear();
        }
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        std::string error;
        bool ok = false;
        // 本次是否成功拉到 4bpp 六色整帧（决定最后走哪条刷屏路径）。
        bool used_4bpp = false;
        // 按官方固件的上报习惯带上电压 / 当前刷新间隔：后端用它画电量图标、
        // 判断重连预热与刷新间隔统计（RSSI 后端只用于统计，这里不上报）。
        InkSightRenderHints hints;
        const int mv = ReadBatteryVoltageMv();
        if (mv > 0) hints.voltage = static_cast<float>(mv) / 1000.0f;
        hints.refresh_minutes = inksight_settings.GetInt("color_refresh", 0);
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            if (inksight_cancel_.load()) {
                ESP_LOGI(kTag, "XiaowuAI refresh cancelled by user");
                SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                cleanup();
                return;
            }
            if (attempt > 1) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            int server_refresh_minutes = 0;
            // 先试 4bpp 六色整帧（把蓝/绿两个通道也用上），内部自带重试。
            ok = InkSightClient::Load4bpp(
                GetNetwork(), inksight_server_, persona,
                SystemInfo::GetMacAddress(), ink_frame4_, error,
                &server_refresh_minutes, hints);
            used_4bpp = ok;
            if (!ok) {
                // 后端不支持 bpp=4（老版本）或设备还没绑定：回落 2bpp 三平面。
                ESP_LOGI(kTag, "4bpp 六色通道不可用(%s)，回落 2bpp 三平面",
                         FriendlyInkSightError(error).c_str());
                error.clear();
                ok = InkSightClient::Load2bpp(
                    GetNetwork(), inksight_server_, persona,
                    SystemInfo::GetMacAddress(), ink_bw_, ink_yellow_, ink_red_,
                    error, &server_refresh_minutes, hints);
            }
            if (server_refresh_minutes > 0) {
                Settings writable("inksight", true);
                writable.SetInt("color_refresh", server_refresh_minutes);
            }
        }
        // 主地址不可用时回退到备用地址；401 代表 token/权限问题，换端口也
        // 无法修复，避免用错误地址再重复生成一轮请求。
        if (!ok && !inksight_server_.empty() &&
            error.find("HTTP 401") == std::string::npos &&
            inksight_server_ == INKSIGHT_DEFAULT_SERVER_URL &&
            std::string(INKSIGHT_FALLBACK_SERVER_URL) != inksight_server_ &&
            !std::string(INKSIGHT_FALLBACK_SERVER_URL).empty()) {
            const std::string fallback = INKSIGHT_FALLBACK_SERVER_URL;
            ESP_LOGW(kTag,
                     "XiaowuAI %s 失败(%s)，回退到备用地址 %s 重试一次",
                     inksight_server_.c_str(),
                     FriendlyInkSightError(error).c_str(), fallback.c_str());
            error.clear();
            int server_refresh_minutes = 0;
            ok = InkSightClient::Load4bpp(
                GetNetwork(), fallback, persona,
                SystemInfo::GetMacAddress(), ink_frame4_, error,
                &server_refresh_minutes, hints);
            used_4bpp = ok;
            if (!ok) {
                error.clear();
                ok = InkSightClient::Load2bpp(
                    GetNetwork(), fallback, persona,
                    SystemInfo::GetMacAddress(), ink_bw_, ink_yellow_, ink_red_,
                    error, &server_refresh_minutes, hints);
            }
            if (server_refresh_minutes > 0) {
                Settings writable("inksight", true);
                writable.SetInt("color_refresh", server_refresh_minutes);
            }
        }
        SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        inksight_last_failed_.store(!ok);
        if (!ok) {
            ESP_LOGE(kTag, "XiaowuAI download failed: %s",
                     FriendlyInkSightError(error).c_str());
            cleanup();
            return;
        }
        // 物理刷新在 display 的后台任务执行（六色约 12s / 七彩约 35s），
        // 此处立即返回。
        if (used_4bpp) {
            if (!display_->ShowInkSightFrame(ink_frame4_)) {
                inksight_last_failed_.store(true);
                cleanup();
                return;
            }
            ESP_LOGI(kTag, "XiaowuAI frame queued for 6-color (4bpp) refresh");
        } else {
            if (!display_->ShowInkSightPlanes(ink_bw_, ink_yellow_, ink_red_)) {
                inksight_last_failed_.store(true);
                cleanup();
                return;
            }
            ESP_LOGI(kTag, "XiaowuAI frame queued for 4-color (2bpp) refresh");
        }
        cleanup();
        if (power_mode_ == PowerMode::kIntermittent &&
            sleep_mode_pending_.load() && !inksight_last_failed_.load()) {
            if (xTaskCreate(IntervalSleepTask, "epd73f_sleep", 3072, this, 3,
                            nullptr) != pdPASS) {
                ESP_LOGE(kTag, "Cannot create interval sleep task");
            }
        }
    }

    // 活跃模式按分钟累计触发刷新；间歇模式每次深睡唤醒只刷新一轮后再睡。
    static void InkSightRefreshTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS373fBoard*>(argument);
        const auto wake_cause = esp_sleep_get_wakeup_cause();
        const bool woke_from_deep_sleep = wake_cause == ESP_SLEEP_WAKEUP_TIMER ||
                                          wake_cause == ESP_SLEEP_WAKEUP_EXT0;
        if (self->power_mode_ == PowerMode::kIntermittent &&
            !woke_from_deep_sleep) {
            self->sleep_mode_pending_.store(!woke_from_deep_sleep);
        }
        if (woke_from_deep_sleep && self->power_mode_ == PowerMode::kIntermittent) {
            const auto cause = esp_sleep_get_wakeup_cause();
            self->sleep_mode_pending_.store(false);
            ESP_LOGI(kTag, "间歇模式唤醒，原因=%d", static_cast<int>(cause));
        }
        // 等系统就绪（WiFi/网络栈）后再首刷。
        vTaskDelay(pdMS_TO_TICKS(25000));
        if (woke_from_deep_sleep &&
            self->power_mode_ == PowerMode::kIntermittent) {
            self->ReportHeartbeat();
            self->ReportDeviceAwake(true);
        }
        Application::GetInstance().Schedule([self, woke_from_deep_sleep]() {
            if (!(woke_from_deep_sleep &&
                  self->power_mode_ == PowerMode::kIntermittent)) {
                self->StartInkSightRefresh();
            }
            if (self->power_mode_ == PowerMode::kIntermittent &&
                woke_from_deep_sleep) {
                if (!self->display_->WaitForRefreshComplete()) {
                    ESP_LOGW(kTag, "等待墨水屏刷新超时，保持唤醒不进入深睡");
                    return;
                }
                self->EnterIntervalSleep();
            }
        });
        int minutes_elapsed = 0;
        while (true) {
            if (self->power_mode_ == PowerMode::kIntermittent) {
                // 深睡唤醒以后这一轮任务已排入应用队列，销毁本轮调度任务。
                vTaskDelete(nullptr);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(60000));
            // 自动型号探测命中后把型号落盘到 NVS，后续开机直接用，不再探测。
            {
                Epd7in3fPanel::Variant probed;
                if (self->display_->panel().TakeProbeResult(&probed)) {
                    Settings s("epd73f", /*read_write=*/true);
                    s.SetString("variant",
                                probed == Epd7in3fPanel::Variant::kSpectra6
                                    ? "6color" : "7color");
                    ESP_LOGI(kTag, "自动型号探测结果已写入 NVS: %s",
                             Epd7in3fPanel::VariantName(probed));
                }
            }
            Settings settings("inksight", /*read_write=*/false);
            const int interval_minutes = self->EffectiveRefreshMinutes();
            if (interval_minutes <= 0) continue;  // 0 = 关闭自动刷新
            ++minutes_elapsed;
            // 活跃模式下取图失败要快速重试（后台刚配好/刚生成完内容时，
            // 等到下一个 5 分钟周期才恢复就太迟了）。间歇模式按原节奏走。
            const int due_minutes =
                (self->power_mode_ == PowerMode::kActive &&
                 self->inksight_last_failed_.load())
                    ? kFastRetryMinutes
                    : interval_minutes;
            if (minutes_elapsed < due_minutes) continue;
            minutes_elapsed = 0;
            Application::GetInstance().Schedule(
                [self]() { self->StartInkSightRefresh(); });
        }
    }

    // 心跳上报：后端「设备配置」页的在线状态看 device_heartbeats 表
    // （ONLINE_WINDOW_MINUTES=15 分钟窗口）。固件默认 30 分钟才刷一次屏，
    // 只靠渲染会在两次刷屏之间显示「离线」，所以每 5 分钟补一次心跳。
    void ReportHeartbeat() {
        if (inksight_server_.empty()) return;
        auto* network = GetNetwork();
        if (!network) return;
        auto& wifi = WifiManager::GetInstance();
        if (!wifi.IsConnected()) return;  // 断网时不发，避免无意义的超时等待
        const int mv = ReadBatteryVoltageMv();
        const float voltage = mv > 0 ? static_cast<float>(mv) / 1000.0f : 3.3f;
        const int rssi = wifi.GetRssi();
        std::string error;
        bool is_bound = true;
        if (InkSightClient::ReportHeartbeat(network, inksight_server_,
                                            SystemInfo::GetMacAddress(), voltage,
                                            rssi, &error, &is_bound)) {
            ESP_LOGI(kTag, "心跳已上报: %.2fV rssi=%d dBm", voltage, rssi);
            if (!is_bound) {
                ESP_LOGW(kTag, "服务器显示设备未绑定，MAC=%s；立即刷新屏幕显示 MAC 和配对码",
                         SystemInfo::GetMacAddress().c_str());
                if (!rebind_refresh_scheduled_.exchange(true)) {
                    Application::GetInstance().Schedule(
                        [this]() { StartInkSightRefresh(); });
                }
            } else {
                rebind_refresh_scheduled_.store(false);
            }
        } else {
            ESP_LOGW(kTag, "心跳上报失败: %s", error.c_str());
        }
    }

    static void HeartbeatTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS373fBoard*>(argument);
        // 启动后尽快检查绑定状态，让后台解绑后的设备无需等完整心跳周期
        // 才显示配对码。ReportHeartbeat 自身会判断网络是否已连接。
        vTaskDelay(pdMS_TO_TICKS(15000));
        while (true) {
            if (self->power_mode_ == PowerMode::kActive) {
                self->ReportHeartbeat();
            }
            vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        }
    }

    static void IntervalSleepTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS373fBoard*>(argument);
        if (!self->display_->WaitForRefreshComplete()) {
            ESP_LOGW(kTag, "等待墨水屏刷新超时，保持唤醒");
        } else {
            self->EnterIntervalSleep();
        }
        vTaskDelete(nullptr);
    }

    static void RecordTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS373fBoard*>(argument);
        char filename[48];
        const uint32_t recording_id =
            static_cast<uint32_t>((esp_timer_get_time() / 1000) & 0xffffffffU);
        std::snprintf(filename, sizeof(filename), "recording_%lu.wav",
                      static_cast<unsigned long>(recording_id));
        ESP_LOGI(kTag, "Recording started: /xiaozhi/recordings/%s (10 seconds)",
                 filename);
        std::string error;
        const bool ok = self->storage_.RecordWav(
            self->GetAudioCodec(), filename, 10, error);
        if (ok) {
            ESP_LOGI(kTag, "Recording saved: /xiaozhi/recordings/%s", filename);
        } else {
            ESP_LOGE(kTag, "Recording failed: %s", error.c_str());
        }
        self->recording_.store(false);
        vTaskDelete(nullptr);
    }

    void StartRecording() {
        bool expected = false;
        if (!recording_.compare_exchange_strong(expected, true)) {
            ESP_LOGW(kTag, "Recording already in progress");
            return;
        }
        if (!storage_.mounted()) {
            recording_.store(false);
            ESP_LOGW(kTag, "Cannot record: TF card is not mounted");
            return;
        }
        if (xTaskCreate(RecordTask, "tf_record", 4096, this, 4, nullptr) != pdPASS) {
            recording_.store(false);
            ESP_LOGE(kTag, "Cannot create recording task");
        }
    }

    // ---- LightAcade（专业外教陪练，仅音频；状态走日志/状态栏）----
    void StartLightAcade() {
        if (lightacade_ && lightacade_->IsRunning()) {
            ESP_LOGI(kTag, "LightAcade already running");
            return;
        }
        if (LIGHTACADE_PROXY_TOKEN[0] == '\0' || inksight_server_.empty()) {
            ESP_LOGE(kTag, "LightAcade not configured: server='%s' token_set=%d",
                     inksight_server_.c_str(),
                     LIGHTACADE_PROXY_TOKEN[0] != '\0' ? 1 : 0);
            return;
        }
        std::string endpoint = inksight_server_;
        if (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
        endpoint += "/api/lightacade/live-ticket";
        Settings settings("lightacade", false);
        const std::string grade_band =
            settings.GetString("grade_band", "junior");
        const std::string language = settings.GetString("language", "en");
        const std::string voice = settings.GetString("voice", "Puck");

        Application::GetInstance().GetAudioService().Stop();
        audio_service_hijacked_.store(true);
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        ESP_LOGI(kTag, "LightAcade start: language=%s grade=%s voice=%s",
                 language.c_str(), grade_band.c_str(), voice.c_str());
        lightacade_ =
            std::make_unique<LightAcadeClient>(GetNetwork(), GetAudioCodec());
        const bool started = lightacade_->Start(
            endpoint, SystemInfo::GetMacAddress(), LIGHTACADE_PROXY_TOKEN,
            language, grade_band, voice,
            [this](const std::string& status) {
                ESP_LOGI(kTag, "LightAcade: %s", status.c_str());
                if (status.find("结束") != std::string::npos) {
                    Application::GetInstance().Schedule(
                        [this]() { RestoreAudioService(); });
                }
            });
        if (!started) {
            lightacade_.reset();
            RestoreAudioService();
        }
    }

    void RestoreAudioService() {
        if (!audio_service_hijacked_.exchange(false)) return;
        for (int i = 0; i < 100 && lightacade_ && lightacade_->IsRunning(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        auto& audio = Application::GetInstance().GetAudioService();
        audio.Start();
        audio.EnableWakeWordDetection(true);
        SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }

    void StopLightAcade() {
        if (lightacade_ && lightacade_->IsRunning()) {
            lightacade_->Stop();
        }
        RestoreAudioService();
    }

    // ---- 按键（无 dashboard，简单映射）----
    void HandleKeyUp() {
        ESP_LOGI(kTag, "KEY+ click");
        auto* codec = GetAudioCodec();
        const int volume = std::min(100, codec->output_volume() + 10);
        codec->SetOutputVolume(volume);
        ESP_LOGI(kTag, "KEY+ volume=%d", volume);
    }

    void HandleKeyDown() {
        ESP_LOGI(kTag, "KEY- click");
        auto* codec = GetAudioCodec();
        const int volume = std::max(0, codec->output_volume() - 10);
        codec->SetOutputVolume(volume);
        ESP_LOGI(kTag, "KEY- volume=%d", volume);
    }

    // 重新配网前先解除后端绑定：MAC 不会因为换 WiFi 而改变，不解绑的话设备
    // 永远停在「已绑定」分支，屏幕上再也不会出现配对码（换账号/换主人时也是
    // 同一个死结）。解绑后设备下次 /api/render 就会拿到带 8 位配对码的引导图。
    //
    // 这里网络可能已经断了（或马上被 StopStation 掐掉），请求失败是常态 ——
    // 所以先落盘 rebind_pending=1，联网成功的回调会再补一次。
    void UnbindForReconfig() {
        Settings s("inksight", /*read_write=*/true);
        s.SetInt("rebind_pending", 1);
        std::string error;
        if (InkSightClient::UnbindSelf(GetNetwork(), inksight_server_,
                                       SystemInfo::GetMacAddress(), &error)) {
            s.SetInt("rebind_pending", 0);
            ESP_LOGI(kTag, "重新配网：已解除绑定，配网完成后会显示配对码");
        } else {
            ESP_LOGW(kTag, "重新配网：解绑请求失败(%s)，联网后自动重试",
                     error.c_str());
        }
    }

    // 按键映射（长按阈值见 kLongPressMsByKey，按住到点即触发，不必等松手）：
    //   短按 +            -> 音量 +10
    //   短按 -            -> 音量 -10
    //   长按 + (3s)       -> 切换电源模式 活跃 <-> 间歇（出厂默认活跃）
    //   长按 - (2s)       -> 进入 WiFi 配网（重配网，起 AP 热点）
    //   单击 OK           -> 语音助手开/关
    //   双击 OK           -> 开始录音
    //   长按 OK (2s)      -> 取消正在进行的 InkSight 下载

    static void KeyScanTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS373fBoard*>(argument);
        constexpr gpio_num_t pins[] = {KEY_UP_GPIO, KEY_OK_GPIO, KEY_DOWN_GPIO};
        int raw[3] = {1, 1, 1};
        int stable[3] = {1, 1, 1};
        int samples[3] = {0, 0, 0};
        int64_t pressed_at[3] = {0, 0, 0};
        bool long_fired[3] = {false, false, false};
        int64_t pending_ok_release = 0;
        for (int i = 0; i < 3; ++i) raw[i] = stable[i] = gpio_get_level(pins[i]);
        ESP_LOGI(kTag, "KEY raw idle: +=%d OK=%d -=%d", stable[0], stable[1], stable[2]);

        while (true) {
            const int64_t now = esp_timer_get_time() / 1000;
            for (int i = 0; i < 3; ++i) {
                const int value = gpio_get_level(pins[i]);
                if (value == raw[i]) ++samples[i];
                else { raw[i] = value; samples[i] = 1; }
                if (samples[i] < 3) continue;

                if (stable[i] != value) {
                    stable[i] = value;
                    if (value == 0) {
                        // 按下
                        pressed_at[i] = now;
                        long_fired[i] = false;
                    } else {
                        // 松开：长按已经处理过就不再走短按逻辑。
                        if (long_fired[i]) {
                            long_fired[i] = false;
                            continue;
                        }
                        if (i == 0) {
                            Application::GetInstance().Schedule(
                                [self]() { self->HandleKeyUp(); });
                        } else if (i == 2) {
                            Application::GetInstance().Schedule(
                                [self]() { self->HandleKeyDown(); });
                        } else {
                            // OK：单击=语音助手开/关，双击=录音。
                            if (pending_ok_release != 0 &&
                                now - pending_ok_release <= 400) {
                                pending_ok_release = 0;
                                Application::GetInstance().Schedule(
                                    [self]() { self->StartRecording(); });
                            } else {
                                pending_ok_release = now;
                            }
                        }
                    }
                }

                // 按住到点即触发长按（不用等松手，操作有确定性反馈）。
                if (stable[i] == 0 && !long_fired[i] && pressed_at[i] != 0 &&
                    now - pressed_at[i] >= kLongPressMsByKey[i]) {
                    long_fired[i] = true;
                    if (i == 2) {
                        ESP_LOGI(kTag, "KEY- long press (%dms): unbind + WiFi config mode",
                                 (int)kLongPressMsByKey[i]);
                        Application::GetInstance().Schedule([self]() {
                            // 先解绑（后端），再起配网热点：配完网设备就会
                            // 重新显示配对码，而不是继续停在已绑定状态。
                            self->UnbindForReconfig();
                            self->EnterWifiConfigMode();
                        });
                    } else if (i == 1) {
                        ESP_LOGI(kTag, "KEY_OK long press: cancel XiaowuAI");
                        self->inksight_cancel_.store(true);
                    } else {
                        // KEY+（GPIO0，兼 BOOT）长按 3 秒：切换 活跃 / 间歇。
                        ESP_LOGI(kTag, "KEY+ long press (%dms): toggle power mode",
                                 (int)kLongPressMsByKey[i]);
                        Application::GetInstance().Schedule(
                            [self]() { self->TogglePowerMode(); });
                    }
                }
            }
            if (pending_ok_release != 0 && now - pending_ok_release > 400) {
                pending_ok_release = 0;
                // OK 短按 = 语音助手开/关。
                Application::GetInstance().Schedule([self]() {
                    Application::GetInstance().ToggleChatState();
                });
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void InitializeButton() {
        gpio_config_t config = {};
        config.pin_bit_mask = (1ULL << KEY_UP_GPIO) | (1ULL << KEY_OK_GPIO) |
                              (1ULL << KEY_DOWN_GPIO);
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&config));
        xTaskCreate(KeyScanTask, "epd73f_keys", 3072, this, 4, nullptr);
    }

    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.lightacade.start",
                    "启动固定 LightAcade 服务的专业英语口语陪练。",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        StartLightAcade();
                        return true;
                    });
        mcp.AddTool("self.lightacade.stop", "结束专业英语口语陪练。",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        StopLightAcade();
                        return true;
                    });
        mcp.AddTool(
            "self.lightacade.set_grade_band",
            "设置英语陪练学段：primary_lower、primary_upper、junior、senior。",
            PropertyList({Property("grade_band", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string value =
                    properties["grade_band"].value<std::string>();
                if (value != "primary_lower" && value != "primary_upper" &&
                    value != "junior" && value != "senior") {
                    return std::string("invalid grade_band");
                }
                Settings settings("lightacade", true);
                settings.SetString("grade_band", value);
                return true;
            });
        mcp.AddTool("self.epaper.set_inksight_server",
                    "设置 InkSight 后端地址（七彩屏看板画面来源）。参数 url 形如 "
                    "https://dual.xiaowuai.vip/backend （不要带 /api/render 后缀）。"
                    "设置后会立即用新地址刷新一次看板。",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        std::string url = properties["url"].value<std::string>();
                        while (!url.empty() && url.back() == '/') url.pop_back();
                        InkSightClient::SaveServerUrl(url);
                        inksight_server_ = url;
                        ESP_LOGI(kTag, "XiaowuAI server saved: %s", url.c_str());
                        Application::GetInstance().Schedule(
                            [this]() { StartInkSightRefresh(); });
                        return true;
                    });
        mcp.AddTool("self.epaper.get_inksight_server",
                    "查询设备当前使用的 InkSight 后端地址（NVS 里保存的值）。"
                    "网页显示「离线」时先用它核对设备到底连的是哪个后端。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        return std::string("inksight_server=") +
                               (inksight_server_.empty() ? "(未设置)"
                                                         : inksight_server_) +
                               "  mac=" + SystemInfo::GetMacAddress();
                    });
        mcp.AddTool("self.epaper.report_heartbeat",
                    "立即上报一次心跳（POST /api/device/<mac>/heartbeat），"
                    "用来确认后端是否收到了设备在线状态。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        std::string error;
                        auto* network = GetNetwork();
                        if (!network) return std::string("失败：网络未就绪");
                        const int mv = ReadBatteryVoltageMv();
                        const float voltage =
                            mv > 0 ? static_cast<float>(mv) / 1000.0f : 3.3f;
                        const int rssi = WifiManager::GetInstance().GetRssi();
                        const bool ok = InkSightClient::ReportHeartbeat(
                            network, inksight_server_,
                            SystemInfo::GetMacAddress(), voltage, rssi, &error);
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "心跳%s server=%s v=%.2f rssi=%d %s", ok
                                          ? "成功"
                                          : "失败",
                                      inksight_server_.c_str(), voltage, rssi,
                                      error.c_str());
                        return std::string(buf);
                    });
        mcp.AddTool("self.epaper.set_inksight_persona",
                    "设置今日看板模式。mode=auto 时由后端按设备(MAC)的模式配置/"
                    "轮换策略决定内容；也可传后端支持的模式 ID（如 WEATHER、ZEN、"
                    "POETRY、DAILY 或自定义模式 ID，可查后端 /api/modes/catalog）。"
                    "设置后立即刷新看板。",
                    PropertyList({Property("mode", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        std::string mode = properties["mode"].value<std::string>();
                        Settings s("inksight", /*read_write=*/true);
                        s.SetString("persona", mode);
                        ESP_LOGI(kTag, "XiaowuAI persona saved: %s", mode.c_str());
                        Application::GetInstance().Schedule(
                            [this]() { StartInkSightRefresh(); });
                        return true;
                    });
        mcp.AddTool("self.epaper.set_panel_variant",
                    "切换 7.3 寸屏型号并在切换后立刻刷一次屏："
                    "variant=6color（7.3\" e-Paper (E) Spectra 6 六色，全刷约 12s）"
                    "或 7color（7.3\" e-Paper (F) ACeP 七彩，全刷约 35s）。"
                    "两种屏分辨率与帧格式相同，只有初始化序列与色索引不同，"
                    "型号选错屏幕会完全没反应。",
                    PropertyList({Property("variant", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string text =
                            properties["variant"].value<std::string>();
                        Epd7in3fPanel::Variant v;
                        if (!Epd7in3fPanel::ParseVariant(text, &v)) {
                            return std::string("invalid variant: ") + text;
                        }
                        Settings s("epd73f", /*read_write=*/true);
                        s.SetString("variant", v == Epd7in3fPanel::Variant::kSpectra6
                                                   ? "6color" : "7color");
                        display_->panel().SetVariant(v);
                        display_->panel().DisableAutoProbe();
                        ESP_LOGI(kTag, "Panel variant set: %s", text.c_str());
                        Application::GetInstance().Schedule(
                            [this]() { StartInkSightRefresh(); });
                        return true;
                    });
        mcp.AddTool("self.epaper.refresh_color_screen",
                    "立即刷新 7.3 寸屏（InkSight 看板画面；六色约 12s、七彩约 35s）。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        Application::GetInstance().Schedule(
                            [this]() { StartInkSightRefresh(); });
                        return true;
                    });
        mcp.AddTool("self.epaper.set_color_refresh_minutes",
                    "设置七彩屏自动刷新间隔（分钟，1~1440；0=关闭自动刷新）。",
                    PropertyList({Property("minutes", kPropertyTypeInteger, 0, 1440)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const int minutes = properties["minutes"].value<int>();
                        Settings s("inksight", /*read_write=*/true);
                        s.SetInt("color_refresh", minutes);
                        ESP_LOGI(kTag, "Color screen refresh interval: %d min",
                                 minutes);
                        return true;
                    });
        mcp.AddTool("self.epaper.set_power_mode",
                    "切换电源模式：active=活跃（出厂默认，Wi-Fi 常在线、刷新更勤、"
                    "取图失败 1 分钟快速重试）；interval=间歇（只在服务端刷新间隔"
                    "到了才刷屏，中间 Wi-Fi modem sleep 省电）。等价操作：长按"
                    "「+」键 3 秒。写入 NVS，重启保持。",
                    PropertyList({Property("mode", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string mode =
                            properties["mode"].value<std::string>();
                        PowerMode next;
                        if (mode == "active" || mode == "活跃") {
                            next = PowerMode::kActive;
                        } else if (mode == "interval" || mode == "间歇") {
                            next = PowerMode::kIntermittent;
                        } else {
                            return std::string("invalid mode: ") + mode;
                        }
                        if (next == power_mode_) {
                            return std::string("power mode = ") + mode +
                                   "（未变化）";
                        }
                        SetPowerMode(next);
                        return std::string("power mode = ") + mode +
                               "（已存 NVS，重启保持）";
                    });
        mcp.AddTool("self.epaper.clear_screen",
                    "整屏清除七彩屏为指定颜色。color 可选 black/white/green/blue/red/yellow/orange。",
                    PropertyList({Property("color", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string color =
                            properties["color"].value<std::string>();
                        uint8_t index;
                        if (color == "black") index = Epd7in3fPanel::kColorBlack;
                        else if (color == "white") index = Epd7in3fPanel::kColorWhite;
                        else if (color == "green") index = Epd7in3fPanel::kColorGreen;
                        else if (color == "blue") index = Epd7in3fPanel::kColorBlue;
                        else if (color == "red") index = Epd7in3fPanel::kColorRed;
                        else if (color == "yellow") index = Epd7in3fPanel::kColorYellow;
                        else if (color == "orange") index = Epd7in3fPanel::kColorOrange;
                        else return std::string("invalid color");
                        return display_->ClearScreen(index);
                    });
        mcp.AddTool("self.epaper.show_color_bars",
                    "在七彩屏显示 7 色色条测试图（黑/蓝/绿/橙/红/黄/白/白，全刷约 35 秒）。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        return display_->ShowColorBars();
                    });
        mcp.AddTool("self.epaper.probe_pins",
                    "排线接线自检：固件依次把 RST/DC/CS 拉低、拉高各 3 秒（日志实时"
                    "打印当前电平，共 6 档约 18 秒）。用万用表在转接板侧量同名丝印脚"
                    "对 GND 的电压并对照日志：跟随变化=该脚通；完全不动=断线或排线没"
                    "插到底；变化出现在别的脚=排线反插（脚序镜像）。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        return display_->panel().ProbePins(3000);
                    });
        mcp.AddTool("self.epaper.set_spi_mode",
                    "切换 7.3 寸屏的 SPI 发送方式（排障用，立即生效，并写入 NVS "
                    "开机保持）。mode=bitbang 用软件模拟 SPI，1:1 复刻参考工程 "
                    "epd.h 的 DEV_SPI_WriteByte（每字节一个 CS 脉冲、约 200kHz）；"
                    "mode=hw 用硬件 SPI（2MHz，整块传输）。命令进不去屏时先试 "
                    "bitbang。",
                    PropertyList({Property("mode", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string mode =
                            properties["mode"].value<std::string>();
                        bool bitbang;
                        if (mode == "bitbang" || mode == "soft" || mode == "sw") {
                            bitbang = true;
                        } else if (mode == "hw" || mode == "hardware" ||
                                   mode == "spi") {
                            bitbang = false;
                        } else {
                            return std::string("invalid mode: ") + mode;
                        }
                        Settings s("epd73f", /*read_write=*/true);
                        s.SetString("spi", bitbang ? "bitbang" : "hw");
                        display_->panel().SetSpiMode(bitbang);
                        ESP_LOGI(kTag, "SPI mode set: %s",
                                 bitbang ? "bitbang(software)" : "hardware");
                        return std::string("spi mode = ") +
                               (bitbang ? "bitbang" : "hw") +
                               "（已存 NVS，重启保持）";
                    });
        mcp.AddTool("self.epaper.blind_test",
                    "盲刷自检：全程忽略 BUSY、按固定延时连刷 全黑 -> 全白 -> 色条 "
                    "三帧（每帧间隔 5 秒，总共约 1~2 分钟）。屏幕应有两次明显的"
                    "整体翻转。若屏幕毫无变化 = 命令根本没进屏（供电/排线/脚序问题，"
                    "先用 self.epaper.probe_pins 核对）；若有翻转但颜色不对 = "
                    "用 self.epaper.set_panel_variant 换另一种型号再跑一次。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        Application::GetInstance().Schedule(
                            [this]() { display_->panel().BlindTest(); });
                        return std::string("盲刷自检已启动，请盯住屏幕");
                    });
        mcp.AddTool("self.epaper.screen_status",
                    "查看七彩屏与 InkSight 配置状态。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        const bool six = display_->panel().variant() ==
                                         Epd7in3fPanel::Variant::kSpectra6;
                        std::string status = std::string("screen(epd73f ") +
                                             (six ? "6-color E" : "7-color F") + "):" +
                                             (display_->panel().Detected() ? "ok"
                                                                           : "busy_floating");
                        status += display_->IsPanelBusy()
                                      ? "; refreshing:true"
                                      : "; refreshing:false";
                        status += display_->panel().BusyReliable()
                                      ? "; busy:ok"
                                      : "; busy:unreliable(use fixed delay)";
                        status += "; server=" + inksight_server_;
                        Settings s("inksight", false);
                        status += "; persona=" + s.GetString("persona", "");
                        status += "; power_mode=";
                        status += PowerModeName(power_mode_);
                        status += "; refresh=" +
                                  std::to_string(EffectiveRefreshMinutes()) + "min";
                        return status;
                    });
        mcp.AddTool("self.audio.set_wake_threshold",
                    "设置唤醒词灵敏度阈值（1~100，默认 15 = 0.15）。值越小越灵敏、"
                    "越容易误触发；喊不响就往下调（如 10），老误唤醒就往上调（如 25）。"
                    "写入 NVS，**重启设备后生效**。",
                    PropertyList({Property("threshold", kPropertyTypeInteger, 1, 100)}),
                    [](const PropertyList& properties) -> ReturnValue {
                        const int t = properties["threshold"].value<int>();
                        Settings s("ww", /*read_write=*/true);
                        s.SetInt("threshold", t);
                        ESP_LOGI(kTag, "Wake word threshold saved: %d (reboot to apply)", t);
                        return std::string("阈值已保存 = ") + std::to_string(t) +
                               "，重启后生效";
                    });
#if CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
        // 仅 MAX98357+ICS-43434 方案有这路可增益（ES8311 方案由 codec 内部
        // 增益控制）。RTTI 关闭，用 static_cast（该分支下类型确定）。
        mcp.AddTool("self.audio.set_mic_gain",
                    "设置麦克风增益（8~20，**立即生效**）。数值 = I2S 32 位采样转 "
                    "int16 的右移位数：越小增益越大。ICS-43434 标准值是 16（24->16）；"
                    "原来的 12 相当于多放大 16 倍、底噪被抬到与语音同量级，会导致"
                    "喊不醒。若改到 16 后声音太小可试 14。",
                    PropertyList({Property("bits", kPropertyTypeInteger, 8, 20)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const int bits = properties["bits"].value<int>();
                        auto* codec = static_cast<NoAudioCodecSimplex*>(GetAudioCodec());
                        codec->SetMicShiftBits(bits);
                        Settings s("audio", /*read_write=*/true);
                        s.SetInt("mic_shift", bits);
                        ESP_LOGI(kTag, "Mic shift bits set: %d", bits);
                        return std::string("麦克风增益已设为 ") + std::to_string(bits);
                    });
        mcp.AddTool("self.audio.set_mic_slot",
                    "强制指定数字麦的 I2S 时隙（left/right，**重启后生效**，写入 NVS "
                    "\"audio\"/\"mic_slot\"）。唤醒词日志长期 peak=0 avg=0 且自动探测"
                    "两个时隙都无数据时，可手动切到 right 试一次；设成 auto 可恢复开机"
                    "自动探测。",
                    PropertyList({Property("slot", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string slot = properties["slot"].value<std::string>();
                        Settings s("audio", /*read_write=*/true);
                        if (slot == "right") {
                            s.SetInt("mic_slot", 1);
                            return std::string("麦克风时隙已强制为 RIGHT，重启后生效");
                        }
                        if (slot == "left") {
                            s.SetInt("mic_slot", 0);
                            return std::string("麦克风时隙已强制为 LEFT，重启后生效");
                        }
                        if (slot == "auto") {
                            s.EraseKey("mic_slot");
                            return std::string("已恢复开机自动探测，重启后生效");
                        }
                        return std::string("slot 只能是 left / right / auto");
                    });
        mcp.AddTool("self.audio.mic_probe",
                    "读一段麦克风原始 I2S 数据并返回峰值/首样本，用于判断麦克风是否真的有数据。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        auto* codec = static_cast<NoAudioCodecSimplex*>(GetAudioCodec());
                        codec->ProbeMicSlot(400, /*force=*/true);
                        const int32_t peak = codec->mic_raw_peak();
                        return std::string("slot=") +
                               ((codec->mic_slot_mask() == I2S_STD_SLOT_LEFT) ? "LEFT" : "RIGHT") +
                               " raw_peak=" + std::to_string(peak) +
                               (peak == 0 ? "（无数据：查麦克风供电/DIN/GPIO/LR-SEL）" : "");
                    });
#endif
        mcp.AddTool("self.storage.status", "查看 TF 卡挂载状态和剩余容量。", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue { return storage_.Status(); });
        mcp.AddTool("self.storage.list", "列出 TF 卡文件。category 可选 images、audio、recordings。",
                    PropertyList({Property("category", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return storage_.List(p["category"].value<std::string>());
                    });
        mcp.AddTool("self.audio.play_sd_wav", "播放 TF 卡 /xiaozhi/audio 中的单声道 16-bit 24kHz PCM WAV。",
                    PropertyList({Property("name", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string error;
                        return storage_.PlayWav(GetAudioCodec(), p["name"].value<std::string>(), error)
                                   ? ReturnValue(true) : ReturnValue(error);
                    });
        mcp.AddTool("self.audio.record_sd_wav", "录音到 TF 卡 /xiaozhi/recordings，seconds 为 1~60 秒。",
                    PropertyList({Property("name", kPropertyTypeString),
                                  Property("seconds", kPropertyTypeInteger, 1, 60)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string error;
                        return storage_.RecordWav(GetAudioCodec(), p["name"].value<std::string>(),
                                                  p["seconds"].value<int>(), error)
                                   ? ReturnValue(true) : ReturnValue(error);
                    });
    }

public:
    Ssd2683zaS373fBoard() {
        ESP_LOGI(kTag, "XiaowuAI fix build: %s", kInkSightFixBuildId);
        InitializePowerAmplifier();
        InitializeI2c();  // 仅 ES8311 方案真正建总线；MAX98357 方案只打日志
        // 开机默认 SPI 方式（NVS "epd73f"/"spi" = bitbang|hw）。必须在
        // Epd73fDisplay 构造（内部会 Init 面板）之前读入。
        {
            Settings s("epd73f", /*read_write=*/false);
            const std::string mode = s.GetString("spi", "hw");
            Epd7in3fPanel::SetDefaultSpiMode(mode == "bitbang" ||
                                             mode == "soft");
        }
        display_ = new Epd73fDisplay();
        inksight_server_ = InkSightClient::LoadServerUrl();
        // 历史版本的编译期默认地址（旧 NAS 域名 nas.insane.vip 的 8090/8091
        // 端口、办公点内网 IP）已全部下线，固件默认改为线上域名。下面按前缀
        // 匹配兼容 NVS 里可能带斜杠/路径的旧值，统一迁移到当前默认地址；
        // 用户显式配置的第三方地址不受影响。
        {
            static const char* kLegacyPrefixes[] = {
                "http://nas.insane.vip:8080",
                "nas.insane.vip:8080",
                "http://nas.insane.vip:8090",
                "https://nas.insane.vip:8090",
                "nas.insane.vip:8090",
                "http://nas.insane.vip:8091",
                "https://nas.insane.vip:8091",
                "nas.insane.vip:8091",
                "http://192.168.1.4:8092",
                "192.168.1.4:8092",
                "http://192.168.101.69:8092",
                "192.168.101.69:8092",
                "http://192.168.101.69:8104",
                "192.168.101.69:8104",
            };
            bool legacy = false;
            for (const char* prefix : kLegacyPrefixes) {
                if (inksight_server_.rfind(prefix, 0) == 0) {
                    legacy = true;
                    break;
                }
            }
            if (legacy) {
                inksight_server_ = INKSIGHT_DEFAULT_SERVER_URL;
                InkSightClient::SaveServerUrl(inksight_server_);
                ESP_LOGI(kTag, "迁移 NVS XiaowuAI 地址至 %s",
                         inksight_server_.c_str());
            }
        }
        if (inksight_server_.empty()) {
            inksight_server_ = INKSIGHT_DEFAULT_SERVER_URL;
            if (inksight_server_.empty()) {
                ESP_LOGW(kTag, "XiaowuAI server not set — use MCP "
                               "self.epaper.set_inksight_server to configure, "
                               "e.g. https://dual.xiaowuai.vip/backend");
            } else {
                ESP_LOGI(kTag, "XiaowuAI server from compile-time default: %s",
                         inksight_server_.c_str());
            }
        } else {
            ESP_LOGI(kTag, "XiaowuAI server (NVS): %s", inksight_server_.c_str());
        }
        // 面板型号（7.3" (E) 六色 / (F) 七彩）：分辨率帧格式一致，但初始化
        // 序列与色索引不同，用错型号屏幕完全点不亮。NVS 键 "variant"。
        {
            Settings s("epd73f", /*read_write=*/false);
            const std::string saved = s.GetString("variant", "");
            Epd7in3fPanel::Variant v;
            if (Epd7in3fPanel::ParseVariant(saved, &v)) {
                display_->panel().SetVariant(v);
            }
            const bool six = display_->panel().variant() ==
                             Epd7in3fPanel::Variant::kSpectra6;
            // 每次开机的第一次刷新都做一次型号探测：若 BUSY 全程无跳变
            // （面板没被这套序列驱动起来），自动换另一种序列重试一次。
            // 有响应或 BUSY 浮空恒高都视为「可能正常」，不会误重试。
            display_->panel().EnableAutoProbe();
            ESP_LOGI(kTag, "面板型号: 7.3\" %s（NVS variant=%s；切换用 MCP "
                           "self.epaper.set_panel_variant）[已开启自动型号探测]",
                     six ? "e-Paper (E) Spectra 6 六色" : "e-Paper (F) ACeP 七彩",
                     saved.empty() ? "默认" : saved.c_str());
        }
        InitializeButton();
        storage_.Mount();  // 卡不在也不能阻塞语音/显示启动。
        InitializeTools();
        // 电源模式：键不存在（新烧录 / 清空 NVS）时默认 1=活跃。
        power_mode_ = LoadPowerMode();
        ESP_LOGI(kTag, "电源模式: %s（NVS inksight/power_mode；长按「+」3 秒切换）",
                 PowerModeName(power_mode_));
        // 重新配网 -> 自动重新配对：长按「-」时网络可能已经不可用（或马上要被
        // StopStation 断掉），解绑请求多半失败，所以先落一个 pending 标志，
        // 等联网成功的这一刻再补一次解绑，并立刻刷一屏把配对码显示出来。
        SetNetworkEventCallback([this](NetworkEvent event, const std::string&) {
            if (event != NetworkEvent::Connected) return;
            // 线上后端走 HTTPS：先确保系统时间有效，否则首个 TLS 握手会因证书
            // Not-Before 校验失败（冷启动时钟停在 1970）。必须早于下面的心跳。
            InkSightClient::EnsureClockForTls();
            // Wi-Fi 起来了才轮得到省电策略与模式上报（构造期 esp_wifi 还没初始化）。
            ApplyPowerMode();
            ReportRuntimeMode(power_mode_);
            // 在线状态依赖 heartbeat；联网后立即上报，避免首个 45 秒周期内
            // 网页仍显示离线。
            ReportHeartbeat();
            ReportDeviceAwake(true);
            // 深睡唤醒后的刷新统一由 InkSightRefreshTask 安排，防止重复拉取。
            Settings s("inksight", /*read_write=*/true);
            if (s.GetInt("rebind_pending", 0) == 0) return;
            std::string error;
            if (InkSightClient::UnbindSelf(GetNetwork(), inksight_server_,
                                           SystemInfo::GetMacAddress(), &error)) {
                s.SetInt("rebind_pending", 0);
                ESP_LOGI(kTag, "重新配网：已解除绑定，本轮刷新会显示配对码");
            } else {
                ESP_LOGW(kTag, "重新配网：解绑失败(%s)，下次刷新继续重试",
                         error.c_str());
            }
            Application::GetInstance().Schedule(
                [this]() { StartInkSightRefresh(3000); });
        });
        if (xTaskCreate(InkSightRefreshTask, "epd73f_inksight", 4096, this, 3,
                        nullptr) != pdPASS) {
            ESP_LOGW(kTag, "Cannot create XiaowuAI refresh task");
        }
        // 活跃模式每 5 分钟心跳；间歇模式在每次唤醒时单独上报后即睡眠。
        if (xTaskCreate(HeartbeatTask, "epd73f_hb", 4096, this, 3, nullptr) !=
            pdPASS) {
            ESP_LOGW(kTag, "Cannot create heartbeat task");
        }
    }

    AudioCodec* GetAudioCodec() override {
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
        static Es8311AudioCodec codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, false, AUDIO_CODEC_PA_VOLTAGE);
#else
        // MAX98357A（左时隙）+ ICS-43434（LR 接地 → 左时隙），独立 I2S 控制器。
        // NVS "audio"/"mic_slot" 可强制指定时隙（0=左 1=右）；未指定则交给
        // ProbeMicSlot() 自动探测（读到全 0 时自动换另一时隙）。
        i2s_std_slot_mask_t mic_slot = I2S_STD_SLOT_LEFT;
        int bits = 16;
        bool slot_explicit = false;
        {
            Settings s("audio", /*read_write=*/false);
            bits = s.GetInt("mic_shift", 16);
            if (bits < 8 || bits > 20) bits = 16;
            const int slot = s.GetInt("mic_slot", -1);
            if (slot == 1) {
                mic_slot = I2S_STD_SLOT_RIGHT;
                slot_explicit = true;
            } else if (slot == 0) {
                mic_slot = I2S_STD_SLOT_LEFT;
                slot_explicit = true;
            }
        }
        static NoAudioCodecSimplex codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_SPK_I2S_GPIO_BCLK, AUDIO_SPK_I2S_GPIO_WS,
            AUDIO_SPK_I2S_GPIO_DOUT, I2S_STD_SLOT_LEFT,
            AUDIO_MIC_I2S_GPIO_SCK, AUDIO_MIC_I2S_GPIO_WS,
            AUDIO_MIC_I2S_GPIO_DIN, mic_slot);
        // ICS-43434 输出 24 位左对齐数据，24->16 的标准转换是 >>16。
        // 默认的 >>12 相当于多放大 16 倍：底噪被抬到与语音同量级，AFE 的 VAD
        // 一直判定"有语音"（日志 detecting 长期满格），MultiNet 反而永远出不了
        // 结果、唤醒词喊不响。这里改回标准增益。
        // NVS 键 "audio"/"mic_shift" 可覆盖（MCP self.audio.set_mic_gain）。
        codec.SetMicShiftBits(bits);
        static bool codec_logged = false;
        if (!codec_logged) {
            codec_logged = true;
            ESP_LOGI(kTag, "ICS-43434 mic shift bits = %d (24->16 标准值 16), slot = %s%s",
                     bits, (mic_slot == I2S_STD_SLOT_LEFT) ? "LEFT" : "RIGHT",
                     slot_explicit ? " (NVS 强制)" : " (自动探测)");
            if (!slot_explicit) {
                codec.ProbeMicSlot();
            }
        }
#endif
        return &codec;
    }

    Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(Ssd2683zaS373fBoard);
