#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "adc_battery_monitor.h"
#include "album_photo.h"
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
#include "codecs/es8311_audio_codec.h"
#else
#include "codecs/no_audio_codec.h"
#endif
#include "config.h"
#include "inksight_client.h"
#include "epaper_todo_web.h"
#include "lightacade_client.h"
#include "mcp_server.h"
#include "sd_card_storage.h"
#include "settings.h"
#include "ssd1619_display.h"
#if EPD_ENABLE_COLOR_SCREEN
#include "ssd2683za_color_panel.h"
#include "uc1679_display.h"
#endif
#include "wifi_board.h"
#include "lvgl_image.h"
#include "system_info.h"
#include <cJSON.h>

#ifndef INKSIGHT_DEFAULT_SERVER_URL
#  define INKSIGHT_DEFAULT_SERVER_URL ""
#endif

#include <esp_heap_caps.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {
constexpr const char* kTag = "Ssd2683zaS3Board";

#if EPD_ENABLE_COLOR_SCREEN
// 仅 4-color(SSD2683ZA BWRY) 需要把 BW/RED 平面打包成 2bpp；3-color(SSD1683 BWR)
// 直接喂两平面（见 ShowColorFrame），故本函数只在 4-color 编译。
// A 座也可能换成四色屏（EPD_A_PANEL_COLORS == 4），两块座共用这段打包逻辑。
#if FPC2_COLOR_COUNT == 4 || EPD_A_PANEL_COLORS == 4
// Match the existing SSD2683ZA reference driver: RAM codes are black=0,
// white=1, yellow=2, red=3. Load2bpp returns B/W and red-marker planes,
// preserving yellow vs red by whether the underlying B/W pixel is white.
void PackInkSightFourColor(const uint8_t* bw_plane, const uint8_t* red_plane,
                           uint8_t* packed_frame) {
    // packed_frame 由调用方按 FPC2 尺寸分配（Ssd2683zaColorPanel::kFrameBytes）。
    constexpr size_t kPixels = FPC2_WIDTH * FPC2_HEIGHT;
    std::memset(packed_frame, 0xff, kPixels / 4);  // default to panel white=3
    for (size_t pixel = 0; pixel < kPixels; ++pixel) {
        const size_t plane_byte = pixel >> 3;
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (pixel & 7));
        const bool white = (bw_plane[plane_byte] & mask) != 0;
        const bool marked_color = (red_plane[plane_byte] & mask) != 0;
        const uint8_t panel_color = marked_color
                                        ? (white ? 2 : 3)  // yellow / red
                                        : (white ? 1 : 0); // white / black
        const unsigned shift = 6 - static_cast<unsigned>(pixel & 3) * 2;
        const uint8_t pixel_mask = static_cast<uint8_t>(0x03u << shift);
        const size_t output_byte = pixel >> 2;
        packed_frame[output_byte] = static_cast<uint8_t>(
            (packed_frame[output_byte] & ~pixel_mask) |
            (panel_color << shift));
    }
}
#endif  // FPC2_COLOR_COUNT == 4 || EPD_A_PANEL_COLORS == 4
#endif
}

class Ssd2683zaS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Ssd1619Display* display_ = nullptr;
#if EPD_A_PANEL_COLORS >= 3
    // A 座换装彩色屏后的上屏驱动（见 config.h EPD_A_PANEL_COLORS）：
    //   3 色 → Uc1679Display（BW/RED 两平面）
    //   4 色 → Ssd2683zaColorPanel（2bpp packed）
    // display_ 仍是 Display 接口与 LVGL 帧缓冲的持有者，双屏内容由这里的实例直接送显。
#if EPD_A_PANEL_COLORS == 3
    Uc1679Display* color_panel_a_ = nullptr;
#else
    Ssd2683zaColorPanel* color_panel_a_ = nullptr;
#endif
    uint8_t* a_bw_ = nullptr;       // A 座白平面（1=白）
    uint8_t* a_red_ = nullptr;      // A 座红标记平面（1=红）
    uint8_t* a_packed_ = nullptr;   // 四色打包帧（仅 4 色用）
    uint8_t* a_last_bw_ = nullptr;  // 上一帧副本，用于内容去重（全刷 20~30 s）
    uint8_t* a_last_red_ = nullptr;
    bool a_frame_valid_ = false;
#endif
    SdCardStorage storage_;
    // VBAT 分压采样（GPIO7 = ADC1_CH6，见 config.h）。双屏 A 屏右上角
    // 电量显示的数据来源；无充电检测脚，仅软件估算电量。
    AdcBatteryMonitor battery_monitor_{BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL,
                                       BATTERY_UPPER_RESISTOR,
                                       BATTERY_LOWER_RESISTOR, GPIO_NUM_NC};
    std::atomic<bool> recording_{false};

#if EPD_ENABLE_COLOR_SCREEN
    // FPC2 彩色图像屏（相册/InkSight/双屏 B 屏），驱动随 menuconfig
    // FPC2_PANEL_COLORS 切换：3-color(B/W/R) → 微雪 SSD1683(Uc1679Display)，
    // 4-color(B/W/Y/R) → SSD2683ZA(Ssd2683zaColorPanel)。两驱动共用 FPC1 的
    // SPI/DC/RST，仅 CS/BUSY 独立（GPIO9/12）。
    // FPC1/CS2：SSD1619 对话信息屏 + 学习 dashboard（快刷）。
#if FPC2_COLOR_COUNT == 3
    using ColorPanelT = Uc1679Display;
#else
    using ColorPanelT = Ssd2683zaColorPanel;
#endif
    ColorPanelT* color_screen_ = nullptr;
    uint8_t* color_bw_ = nullptr;   // FPC2 1bpp 平面（1=白），大小见对应驱动
    uint8_t* color_red_ = nullptr;  // FPC2 1bpp 平面（1=红）
    uint8_t* color_frame_ = nullptr;  // 4-color 驱动用的 2bpp 帧（SSD2683ZA BWRY）
    TaskHandle_t color_refresh_task_ = nullptr;
    std::atomic<bool> inksight_device_unbound_{false};
    // 双屏后台模式：dual-config（课程表/便签）存在时，定时刷新改为拉取
    // DUAL_A（A 屏便签）/ DUAL_B（B 屏课程表），代替 InkSight persona。
    std::atomic<bool> dual_mode_{false};
    // 后端 dual-config 的 savedAt/updated_at。网页每次「应用到设备」都会更新，
    // 固件据此在常规彩屏刷新周期之外尽快刷新新配置。
    std::string dual_config_revision_;
    std::atomic<bool> dual_refresh_pending_{false};
    // 当前场景 B 屏模式（ProbeDualMode 解析 b.mode），用于判断是否走 TF 卡本地相册：
    // mode=album-tf 时图片在设备 TF 卡上，后端拿不到，必须由固件读卡刷屏。
    std::string dual_b_mode_;
    // TF 卡相册轮换游标：每次刷新换下一张，重启归零（不做持久化，省 NVS 擦写）。
    size_t dual_tf_album_index_ = 0;
    // 开机跳过首页菜单后，若首刷网络图失败（断网/服务器不可达），用这个 once
    // 标志确保只回退显示一次首页菜单，避免白屏且不在每个刷新周期重复全闪。
    std::atomic<bool> boot_home_fallback_done_{false};

    enum class PowerMode : int { kIntermittent = 0, kActive = 1 };
    PowerMode power_mode_ = PowerMode::kActive;
    std::atomic<bool> sleep_mode_pending_{false};

    static const char* PowerModeName(PowerMode mode) {
        return mode == PowerMode::kActive ? "活跃" : "间歇";
    }

    PowerMode LoadPowerMode() {
        Settings settings("inksight", false);
        return settings.GetInt("power_mode", 1) != 0
                   ? PowerMode::kActive
                   : PowerMode::kIntermittent;
    }

    void ReportRuntimeMode(bool awake) {
        if (inksight_server_.empty() || GetNetwork() == nullptr) return;
        std::string error;
        if (!InkSightClient::ReportRuntimeMode(
                GetNetwork(), inksight_server_, SystemInfo::GetMacAddress(),
                power_mode_ == PowerMode::kActive, awake, &error)) {
            ESP_LOGW(kTag, "运行模式上报失败: %s", error.c_str());
        } else {
            ESP_LOGI(kTag, "运行模式已上报: %s awake=%s",
                     PowerModeName(power_mode_), awake ? "true" : "false");
        }
    }

    // 把 TF 卡当前内容快照（各分类文件清单）POST 给后端「本地 TF 卡」面板。
    // 快照 JSON：{"mounted":true,"status":"...","categories":{"images":[...],"audio":[...],"books":[...],"recordings":[...]}}
    void ReportTfStorage() {
        if (inksight_server_.empty() || GetNetwork() == nullptr) return;
        // 开机时的挂载可能早于用户插卡，第一次 SPI 握手也可能偶发失败（卡座
        // 是自弹式、没有独立 CD 脚，空座和接触不良在协议层无法区分）。上报前
        // 补一次挂载重试。
        if (!storage_.mounted()) storage_.EnsureMounted();
        const bool card_ok = storage_.mounted();
        if (!card_ok) {
            const std::string& me = storage_.MountError();
            ESP_LOGI(kTag, "TF 卡未挂载：%s",
                     me.empty() ? "未检测到 TF 卡（卡座为空或接触不良）" : me.c_str());
        }
        // 关键：失败也要上报。以前挂载失败直接 return，后台「本地 TF 卡」面板
        // 永远显示「暂无 TF 卡上报数据」，用户和自己都分不清是没插卡、触点
        // 接触不良、格式不对还是固件没走到这里。现在把 mounted=false + 原因
        // 写进快照，面板会直接显示未挂载和具体原因。
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "mounted", card_ok);
        if (card_ok) {
            // 容量单独上报数字字段，后台「本地 TF 卡」面板直接显示
            // 「总容量 X MB · 可用 Y MB」，不用再解析 status 文本。
            uint32_t total_mb = 0, free_mb = 0;
            if (storage_.Capacity(total_mb, free_mb)) {
                cJSON_AddNumberToObject(root, "total_mb", total_mb);
                cJSON_AddNumberToObject(root, "free_mb", free_mb);
            }
            cJSON_AddStringToObject(root, "status", storage_.Status().c_str());
            cJSON* cats = cJSON_CreateObject();
            for (const char* cat : {"images", "audio", "books", "recordings"}) {
                cJSON* arr = cJSON_CreateArray();
                std::string listing = storage_.List(cat);
                if (!listing.empty() && listing != "(empty)") {
                    size_t start = 0;
                    while (true) {
                        size_t nl = listing.find('\n', start);
                        const size_t len = (nl == std::string::npos) ? std::string::npos : nl - start;
                        std::string name = listing.substr(start, len);
                        if (!name.empty()) cJSON_AddItemToArray(arr, cJSON_CreateString(name.c_str()));
                        if (nl == std::string::npos) break;
                        start = nl + 1;
                    }
                }
                cJSON_AddItemToObject(cats, cat, arr);
            }
            cJSON_AddItemToObject(root, "categories", cats);
        } else {
            const std::string& me = storage_.MountError();
            const std::string reason =
                me.empty() ? std::string("未检测到 TF 卡（卡座为空或接触不良）") : me;
            cJSON_AddStringToObject(root, "status", reason.c_str());
            cJSON_AddStringToObject(root, "reason", reason.c_str());
        }
        char* payload = cJSON_PrintUnformatted(root);
        std::string snapshot = payload ? payload : "{}";
        if (payload) free(payload);
        cJSON_Delete(root);
        std::string error;
        if (!InkSightClient::ReportStorage(
                GetNetwork(), inksight_server_, SystemInfo::GetMacAddress(), snapshot, &error)) {
            ESP_LOGW(kTag, "TF 卡存储上报失败: %s", error.c_str());
        } else {
            ESP_LOGI(kTag, "TF 卡存储已上报后台");
        }
    }

    int EffectiveRefreshMinutes() const {
        Settings settings("inksight", false);
        const int clr = settings.GetInt("clr_ref_min", 30);
        // 双屏（FPC2 在线）：B 屏课程表按彩屏间隔（默认 30 分钟）整对刷新。
        if (dual_mode_.load()) return clr > 0 ? clr : 30;
        // 单屏/降级（FPC2 探测失败）：A 屏跟随后端 X-Refresh-Minutes
        // （/api/render DUAL_A 下发 5 分钟），不再陪跑彩屏的 30 分钟周期。
        const int a = settings.GetInt("a_ref_min", 0);
        if (a > 0) return a;
        return clr > 0 ? clr : 30;
    }

    void ApplyPowerMode() {
        SetPowerSaveLevel(power_mode_ == PowerMode::kActive
                              ? PowerSaveLevel::PERFORMANCE
                              : PowerSaveLevel::LOW_POWER);
        ESP_LOGI(kTag, "电源模式: %s，彩屏刷新间隔 %d 分钟",
                 PowerModeName(power_mode_), EffectiveRefreshMinutes());
    }

    void EnterIntervalSleepIfPending() {
        if (!sleep_mode_pending_.load() ||
            power_mode_ != PowerMode::kIntermittent) return;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            ESP_LOGI(kTag, "间歇模式待机：语音会话结束后再休眠");
            return;
        }
        const TickType_t start = xTaskGetTickCount();
        while (display_->IsPanelBusy() &&
               (xTaskGetTickCount() - start) * portTICK_PERIOD_MS < 60000) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (display_->IsPanelBusy()) {
            ESP_LOGW(kTag, "FPC1 刷新未结束，暂不进入深睡");
            return;
        }
        while (gpio_get_level(KEY_UP_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ReportRuntimeMode(false);
        // Pair-code claims expire sooner than the normal image interval. Keep
        // an unbound device awake on a short refresh cycle so its code remains
        // claimable; after binding, use the configured interval.
        const int sleep_minutes = inksight_device_unbound_.load()
                                      ? 4
                                      : EffectiveRefreshMinutes();
        const uint64_t sleep_us = static_cast<uint64_t>(
            std::max(1, sleep_minutes)) * 60ULL * 1000000ULL;
        const esp_err_t timer_err = esp_sleep_enable_timer_wakeup(sleep_us);
        const esp_err_t key_err = esp_sleep_enable_ext0_wakeup(KEY_UP_GPIO, 0);
        if (timer_err != ESP_OK || key_err != ESP_OK) {
            ESP_LOGE(kTag, "深睡唤醒源配置失败: timer=%s KEY+=%s",
                     esp_err_to_name(timer_err), esp_err_to_name(key_err));
            return;
        }
        ESP_LOGI(kTag, "间歇模式进入深睡，%d 分钟后定时唤醒，KEY+ 可提前唤醒",
                 sleep_minutes);
        esp_deep_sleep_start();
    }

    void SetPowerMode(PowerMode next) {
        if (next == power_mode_) return;
        power_mode_ = next;
        Settings settings("inksight", true);
        settings.SetInt("power_mode", next == PowerMode::kActive ? 1 : 0);
        sleep_mode_pending_.store(next == PowerMode::kIntermittent);
        ApplyPowerMode();
        ReportRuntimeMode(true);
        display_->dashboard().SetInkSightFooter(
            next == PowerMode::kActive ? "已切换活跃模式" : "已切换间歇模式，刷新后休眠");
        if (next == PowerMode::kActive) {
            Application::GetInstance().Schedule(
                [this]() { RefreshInkSight(/*initial_full=*/true); });
        } else if (!inksight_busy_.load()) {
            Application::GetInstance().Schedule(
                [this]() { RefreshInkSight(/*initial_full=*/true); });
        }
        ESP_LOGI(kTag, "电源模式切换为 %s", PowerModeName(power_mode_));
    }

    void TogglePowerMode() {
        SetPowerMode(power_mode_ == PowerMode::kActive
                         ? PowerMode::kIntermittent
                         : PowerMode::kActive);
    }

    void InitializeColorScreenAfterNetwork() {
        if (color_screen_ != nullptr) return;
        // The color panel shares DC/RST with the SSD1619. Keep it completely
        // untouched during provisioning so the fast panel can show the prompt.
        // FPC2 上电初始化偶发失败：BUSY 引脚上电瞬态/电源未稳，会让极性被误判
        // 或面板复位后迟迟不释放 BUSY（同一次上电首次激活却曾成功）。复位稳定后
        // 多试几次即可恢复，避免彩色屏整段不可用。
#if FPC2_COLOR_COUNT == 3
        auto* candidate = new Uc1679Display([this]() {
            return display_ != nullptr && display_->IsPanelBusy();
        });
#else
        auto* candidate = new Ssd2683zaColorPanel([this]() {
            return display_ != nullptr && display_->IsPanelBusy();
        });
#endif
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            if (attempt > 1) vTaskDelay(pdMS_TO_TICKS(500));
            ok = candidate->Init();
            if (!ok) {
                ESP_LOGW(kTag, "FPC2 color panel init attempt %d failed, retrying",
                         attempt);
            }
        }
        if (!ok) {
            delete candidate;
            ESP_LOGW(kTag, "FPC2 color panel initialization failed; keeping FPC1 active");
            // FPC2 挂了也必须起刷新任务：双屏模式下 RefreshDualScreens 会懒探测
            // FPC2（见下方 B 屏分支），单屏/未绑定则降级显示在 FPC1。
            if (color_refresh_task_ == nullptr &&
                xTaskCreate(ColorRefreshTask, "color_refresh", 8192, this, 3,
                            &color_refresh_task_) != pdPASS) {
                ESP_LOGW(kTag, "Cannot create color refresh task");
            }
            return;
        }
        color_screen_ = candidate;
        if (color_refresh_task_ == nullptr &&
            xTaskCreate(ColorRefreshTask, "color_refresh", 8192, this, 3,
                        &color_refresh_task_) != pdPASS) {
            ESP_LOGW(kTag, "Cannot create color refresh task");
        }
    }

    // 把 BW/RED 两平面送显到 FPC2：3-color(SSD1683 BWR) 直接喂两平面；
    // 4-color(SSD2683ZA BWRY) 先打包成 2bpp 帧。两驱动接口不同，在此统一。
    bool ShowColorFrame(const uint8_t* bw, const uint8_t* red) {
#if FPC2_COLOR_COUNT == 3
        // SSD1683 RED RAM 约定：1=红、0=非红（与生成端 ConvertJpegToBwrPlanes
        // 的 "1=红标记" 一致；Waveshare 官方 EPD_4in2bc_Display 亦直接发送
        // ryimage 不取反）。故三色路径直接送显，无需取反（取反反而整屏泛红）。
        return color_screen_->ShowFrame(bw, red);
#else
        PackInkSightFourColor(bw, red, color_frame_);
        return color_screen_->ShowFrame(color_frame_);
#endif
    }
#endif

#if EPD_A_PANEL_COLORS >= 3
    // 把 BW/RED 两平面送显到 A 座（FPC1）。三色直接喂两平面，四色先打包 2bpp。
    // 两块座尺寸一致，packed/frame 缓冲都按 FPC2_* 分配。
    bool ShowColorFrameA(const uint8_t* bw, const uint8_t* red) {
        if (color_panel_a_ == nullptr || bw == nullptr || red == nullptr) {
            return false;
        }
#if EPD_A_PANEL_COLORS == 3
        return color_panel_a_->ShowFrame(bw, red);
#else
        if (a_packed_ == nullptr) return false;
        PackInkSightFourColor(bw, red, a_packed_);
        return color_panel_a_->ShowFrame(a_packed_);
#endif
    }
#endif

    // InkSight 今日看板：帧缓冲放在 PSRAM，15KB；busy_ 防重复刷新。
    uint8_t* inksight_frame_ = nullptr;
    std::atomic<bool> inksight_busy_{false};
    std::atomic<bool> inksight_cancel_{false};  // 长按返回时取消正在进行的下载
    std::string inksight_server_;
    std::unique_ptr<LightAcadeClient> lightacade_;
    std::unique_ptr<EpaperTodoWeb> todo_web_;
    int64_t lightacade_last_screen_update_ms_ = 0;
    // LightAcade 占用编解码器期间 AudioService 被整体停掉，此标志标记
    // "需要恢复"，避免重复 Start 打乱正常运行的音频服务。
    std::atomic<bool> audio_service_hijacked_{false};

    void StartLightAcade() {
        // 双屏学习模式：A 屏（便签）/ B 屏（课程表）由后端渲染、定时刷新，
        // 与口语陪练抢占同一块 FPC1 黑白快刷屏且语义冲突，故双屏模式下禁用
        // 口语陪练。小智（XiaoZhi）语音助手不受影响，照常可用。
        if (dual_mode_.load()) {
            ESP_LOGW(kTag, "LightAcade blocked: dual learning mode active");
            display_->dashboard().SetSpeakingStatus(
                "双屏学习模式已启用\n\n口语陪练不可用");
            return;
        }
        if (lightacade_ && lightacade_->IsRunning()) {
            display_->dashboard().SetSpeakingStatus(
                "专业外教陪练进行中…\n\n长按 OK 结束并返回");
            return;
        }
        // 配置预检：未配置时绝不停/启音频服务，否则一次必然失败的启动会把
        // 整个音频管线重启一遍，新旧任务竞争会破坏 I2S 录音通路。
        if (LIGHTACADE_PROXY_TOKEN[0] == '\0' || inksight_server_.empty()) {
            ESP_LOGE(kTag, "LightAcade not configured: server='%s' token_set=%d",
                     inksight_server_.c_str(),
                     LIGHTACADE_PROXY_TOKEN[0] != '\0' ? 1 : 0);
            display_->dashboard().SetSpeakingStatus(
                "陪练服务未配置\n\n需在固件中设置访问令牌");
            return;
        }
        std::string endpoint = inksight_server_;
        if (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
        endpoint += "/api/lightacade/live-ticket";
        Settings settings("lightacade", false);
        const std::string grade_band =
            settings.GetString("grade_band", "junior");
        // 语种/音色由墨水屏"用户中心"页设置（默认英语/小阳）。
        const std::string language =
            settings.GetString("language", "en");
        const std::string voice =
            settings.GetString("voice", "Puck");

        // Xiaozhi's Opus task and LightAcade's raw PCM task must not read
        // the ES8311 concurrently.
        Application::GetInstance().GetAudioService().Stop();
        audio_service_hijacked_.store(true);
        // LightAcade 绕过了框架的会话机制，没人替它切 WiFi 电源模式；
        // MAX_MODEM 深度省电下突发 WS 音频延迟飙升、丢包，表现为
        // "开始还行，越说越卡、卡着卡着就断"。会话期间强制 PERFORMANCE。
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        ESP_LOGI(kTag, "LightAcade start: language=%s grade=%s voice=%s",
                 language.c_str(), grade_band.c_str(), voice.c_str());
        lightacade_ =
            std::make_unique<LightAcadeClient>(GetNetwork(), GetAudioCodec());
        const bool started = lightacade_->Start(
            endpoint, SystemInfo::GetMacAddress(), LIGHTACADE_PROXY_TOKEN,
            language, grade_band, voice,
            [this](const std::string& status) {
                const int64_t now_ms = esp_timer_get_time() / 1000;
                const bool important =
                    status.find("结束") != std::string::npos ||
                    status.find("错误") != std::string::npos ||
                    status.find("失败") != std::string::npos ||
                    status.find("额度") != std::string::npos;
                if (!important &&
                    now_ms - lightacade_last_screen_update_ms_ < 1800) {
                    return;
                }
                lightacade_last_screen_update_ms_ = now_ms;
                Application::GetInstance().Schedule([this, status]() {
                    display_->dashboard().SetSpeakingStatus(status);
                    // 会话自行结束（end 帧 / 连接断开 / 任务退出）时立即恢复
                    // 音频服务与唤醒词，不等用户长按返回首页。
                    if (status.find("结束") != std::string::npos) {
                        RestoreAudioService();
                    }
                });
            });
        if (!started) {
            lightacade_.reset();
            RestoreAudioService();
        }
    }

    // LightAcade 直接占用编解码器，AudioService 被整体停掉。会话结束
    // （含服务端自行结束）或用户返回后调用：等陪练任务真正退出释放编解码
    // 器，再重启音频服务并重新挂上唤醒词——AudioService::Start() 会清掉
    // 唤醒词事件位，不重挂则返回首页后"呼叫小智"失灵。幂等：未占用时空操作。
    void RestoreAudioService() {
        if (!audio_service_hijacked_.exchange(false)) return;
        for (int i = 0; i < 100 && lightacade_ && lightacade_->IsRunning(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        auto& audio = Application::GetInstance().GetAudioService();
        audio.Start();
        audio.EnableWakeWordDetection(true);
        // 会话结束，恢复 WiFi 深度省电
        SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }

    void StopLightAcade() {
        if (lightacade_ && lightacade_->IsRunning()) {
            lightacade_->Stop();
        }
        RestoreAudioService();
    }

    // 今日看板模式轮换：WEATHER → DAILY → STOIC → ZEN → POETRY → WEATHER
    void CycleInkSightPersona() {
        static const char* kPersonas[] = {
            "WEATHER", "DAILY", "STOIC", "ZEN", "POETRY"
        };
        static constexpr int kCount = 5;
        Settings settings("inksight", true);
        std::string current = settings.GetString("persona", "");
        int idx = 0;
        for (int i = 0; i < kCount; ++i) {
            if (current == kPersonas[i]) { idx = (i + 1) % kCount; break; }
        }
        // current 为空或 "auto" 时从 WEATHER 开始
        if (current.empty() || current == "auto" || current == "AUTO") idx = 0;
        settings.SetString("persona", kPersonas[idx]);
        ESP_LOGI(kTag, "XiaowuAI persona cycled to %s", kPersonas[idx]);
        display_->dashboard().SetInkSightFooter(
            std::string("模式: ") + kPersonas[idx] + " 正在更新…");
        Application::GetInstance().Schedule(
            [this]() { RefreshInkSight(/*initial_full=*/true); });
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

#if EPD_ENABLE_COLOR_SCREEN
    // 探测双屏后台模式：控制台发出的 dual-config 含 sceneKey + config[sceneKey]
    // 且 a/b 两块屏配置齐备即视为已启用（兼容旧 config.learning 结构）。每次定时
    // 刷新前调用一次，控制台「应用到设备」后最多一个刷新周期生效（开机即探测）。
    bool ProbeDualMode() {
        if (inksight_server_.empty() || GetNetwork() == nullptr) return false;
        std::string json;
        std::string error;
        if (!InkSightClient::LoadDualConfig(GetNetwork(), inksight_server_,
                                            SystemInfo::GetMacAddress(), json,
                                            &error)) {
            ESP_LOGW(kTag, "dual-config 拉取失败: %s", error.c_str());
            return false;  // 拉取失败保持现状，下个周期再试
        }
        bool has_dual = false;
        std::string revision;
        cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
        if (root != nullptr) {
            // 每次重新解析前清空，避免切换场景后旧场景的 B 屏模式残留
            // （例如从「TF 卡相册」切回天气，仍被当成本地读卡）。
            dual_b_mode_.clear();
            // 后端 GET /api/device/<mac>/dual-config 返回 {"config": <dual_config>,
            // "updated_at": ...}，先解包一层拿到真正的 dual_config。
            cJSON* dc = cJSON_GetObjectItemCaseSensitive(root, "config");
            if (!cJSON_IsObject(dc)) dc = root;  // 兼容直接返回 dual_config 的情况
            const cJSON* updated_at =
                cJSON_GetObjectItemCaseSensitive(root, "updated_at");
            if (cJSON_IsString(updated_at)) revision = updated_at->valuestring;
            if (revision.empty()) {
                const cJSON* saved_at =
                    cJSON_GetObjectItemCaseSensitive(dc, "savedAt");
                if (cJSON_IsString(saved_at)) revision = saved_at->valuestring;
            }
            const cJSON* config =
                cJSON_GetObjectItemCaseSensitive(dc, "config");
            // 新 schema：dual_config 顶层 sceneKey 指向 config 下某个场景，且该
            // 场景含 a/b 两块屏配置即视为已启用双屏（控制台当前发出的结构）。
            const cJSON* scene_key =
                cJSON_GetObjectItemCaseSensitive(dc, "sceneKey");
            if (cJSON_IsString(scene_key) && cJSON_IsObject(config)) {
                const cJSON* scene = cJSON_GetObjectItemCaseSensitive(
                    config, scene_key->valuestring);
                if (cJSON_IsObject(scene)) {
                    const cJSON* a =
                        cJSON_GetObjectItemCaseSensitive(scene, "a");
                    const cJSON* b =
                        cJSON_GetObjectItemCaseSensitive(scene, "b");
                    if (cJSON_IsObject(a) && cJSON_IsObject(b)) has_dual = true;
                    // 记下 B 屏模式：album-tf 表示「TF 卡相册」，图片在卡上，
                    // 后端出不了，刷屏时改走本地读卡（其余模式仍拉网络图）。
                    if (cJSON_IsObject(b)) {
                        const cJSON* b_mode =
                            cJSON_GetObjectItemCaseSensitive(b, "mode");
                        if (cJSON_IsString(b_mode) &&
                            b_mode->valuestring != nullptr) {
                            dual_b_mode_ = b_mode->valuestring;
                        }
                    }
                }
            }
            // 兼容旧 schema：config.learning 为对象即视为已启用。
            if (!has_dual && cJSON_IsObject(config)) {
                const cJSON* learning =
                    cJSON_GetObjectItemCaseSensitive(config, "learning");
                if (cJSON_IsObject(learning)) has_dual = true;
            }
            cJSON_Delete(root);
        }
        const bool prev = dual_mode_.exchange(has_dual);
        const bool revision_changed = has_dual && !revision.empty() &&
                                      revision != dual_config_revision_;
        if (has_dual && !revision.empty()) dual_config_revision_ = revision;
        if (!has_dual) dual_config_revision_.clear();
        if (has_dual != prev) {
            ESP_LOGI(kTag, "双屏模式%s",
                     has_dual ? "已启用（检测到 dual-config）" : "已关闭");
        }
        return has_dual != prev || revision_changed;
    }

    // 双屏刷新：A 屏（FPC1 黑白）拉 DUAL_A 便签 BMP，B 屏（FPC2 彩色）拉
    // DUAL_B 今日课程表 2bpp。渲染全部在后端完成，固件只负责拉图 + 刷屏。
    void RefreshDualScreens() {
        if (!dual_mode_.load()) return;
        if (inksight_busy_.exchange(true)) {
            ESP_LOGW(kTag, "dual refresh skipped: busy");
            return;
        }
        auto cleanup = [this]() { inksight_busy_.store(false); };
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        // 双屏任一屏成功取到图后，据此更新未绑定标志，绑定态会自动清掉残留
        // 配对码帧；连续失败时保持上一次判定，避免误清。
        bool device_unbound = inksight_device_unbound_.load();
        bool any_ok = false;

        // A 屏：便签。面板类型决定帧格式（见 config.h EPD_A_PANEL_COLORS）：
        //   2 = 1bpp BMP → SSD1619 快刷
        //   3/4 = 2bpp 裸帧 → 拆成 BW/RED 两平面 → 三色直送 / 四色打包
#if EPD_A_PANEL_COLORS == 2
        if (inksight_frame_ == nullptr) {
            inksight_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
                InkSightClient::kFrameBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (inksight_frame_ != nullptr) {
            std::string error;
            // 电量随 DUAL_A 请求上报，后端画在 A 屏右上角（icon + 百分比）。
            const int battery_pct =
                static_cast<int>(battery_monitor_.GetBatteryLevel());
            if (InkSightClient::Load(GetNetwork(), inksight_server_, "DUAL_A",
                                     SystemInfo::GetMacAddress(),
                                     inksight_frame_, error,
                                     nullptr /* server_refresh_minutes */,
                                     battery_pct, &device_unbound)) {
                any_ok = true;
                display_->RawFrameRefresh(inksight_frame_,
                                          false /* partial 快刷 */);
                ESP_LOGI(kTag, "双屏 A（便签）已刷新");
            } else {
                ESP_LOGW(kTag, "双屏 A 拉取失败: %s", error.c_str());
            }
        }
#else
        {
            static constexpr size_t kPlaneBytes =
                InkSightClient::kColorStride * InkSightClient::kColorHeight;
            if (a_bw_ == nullptr) {
                a_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                    kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                a_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                    kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                a_last_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                    kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                a_last_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                    kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
#if EPD_A_PANEL_COLORS == 4
            if (a_packed_ == nullptr) {
                a_packed_ = static_cast<uint8_t*>(heap_caps_malloc(
                    Ssd2683zaColorPanel::kFrameBytes,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
            const bool a_mem_ok = a_bw_ && a_red_ && a_last_bw_ && a_last_red_ && a_packed_;
#else
            const bool a_mem_ok = a_bw_ && a_red_ && a_last_bw_ && a_last_red_;
#endif
            if (a_mem_ok) {
                std::string error;
                // 电量仍随 DUAL_A 上报（后端画在 A 屏右上角）。
                // （Load2bpp 目前不带电量参数，改由 Load 的历史路径保证；A 座
                //   彩色屏不显示电量图标不影响功能，后续需要再加。）
                if (InkSightClient::Load2bpp(
                        GetNetwork(), inksight_server_, "DUAL_A",
                        SystemInfo::GetMacAddress(), a_bw_, a_red_, error,
                        nullptr /* server_refresh_minutes */, &device_unbound,
                        EPD_A_PANEL_COLORS, "A")) {
                    any_ok = true;
                    // 三色/四色都是 20~30 s 全刷，内容没变就别刷，否则每轮
                    // 定时刷新都白等半分钟、还会和 LVGL 重绘抢总线。
                    if (a_frame_valid_ &&
                        std::memcmp(a_last_bw_, a_bw_, kPlaneBytes) == 0 &&
                        std::memcmp(a_last_red_, a_red_, kPlaneBytes) == 0) {
                        ESP_LOGI(kTag, "双屏 A 内容未变，跳过全刷");
                    } else {
                        std::memcpy(a_last_bw_, a_bw_, kPlaneBytes);
                        std::memcpy(a_last_red_, a_red_, kPlaneBytes);
                        a_frame_valid_ = true;
                        if (ShowColorFrameA(a_bw_, a_red_)) {
                            ESP_LOGI(kTag, "双屏 A（便签）已刷新（%d 色）",
                                     EPD_A_PANEL_COLORS);
                        } else {
                            ESP_LOGW(kTag, "双屏 A 刷屏失败");
                            a_frame_valid_ = false;  // 下次同一画面还能重试
                        }
                    }
                } else {
                    ESP_LOGW(kTag, "双屏 A 拉取失败: %s", error.c_str());
                }
            } else {
                ESP_LOGW(kTag, "双屏 A 内存不足");
            }
        }
#endif

        // B 屏：今日课程表（2bpp → SSD2683ZA BWRY）
        // 上电时 FPC2 偶发探测失败，这里懒探测自愈：只要双屏模式还在，就再试一次
        // 初始化，成功后续刷新本屏；失败则继续保持单屏（A 屏）运行。
        if (color_screen_ == nullptr) {
            InitializeColorScreenAfterNetwork();
        }
        if (color_screen_ != nullptr && color_screen_->Available()) {
            if (color_bw_ == nullptr) {
                color_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                    Ssd2683zaColorPanel::kWidth / 8 * Ssd2683zaColorPanel::kHeight,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
            if (color_red_ == nullptr) {
                color_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                    Ssd2683zaColorPanel::kWidth / 8 * Ssd2683zaColorPanel::kHeight,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
#if FPC2_COLOR_COUNT == 4
            if (color_frame_ == nullptr) {
                color_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
                    Ssd2683zaColorPanel::kFrameBytes,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
#endif  // FPC2_COLOR_COUNT == 4
#if FPC2_COLOR_COUNT == 4
            if (color_bw_ != nullptr && color_red_ != nullptr &&
                color_frame_ != nullptr) {
#else
            if (color_bw_ != nullptr && color_red_ != nullptr) {
#endif  // FPC2_COLOR_COUNT == 4
                std::string error;
                int server_refresh_minutes = 0;
                // TF 卡相册：图片在设备卡上，后端渲染不出来，优先本地读卡刷屏。
                // 失败（没插卡 / 目录空 / 解码失败）就落到下面的网络图，不让屏空着。
                if (dual_b_mode_ == "album-tf" && RefreshDualScreenFromTf()) {
                    any_ok = true;
                } else if (InkSightClient::Load2bpp(
                        GetNetwork(), inksight_server_, "DUAL_B",
                        SystemInfo::GetMacAddress(), color_bw_, color_red_,
                        error, &server_refresh_minutes, &device_unbound)) {
                    any_ok = true;
                    if (server_refresh_minutes > 0) {
                        Settings writable("inksight", true);
                        writable.SetInt("clr_ref_min", server_refresh_minutes);
                    }
                    if (ShowColorFrame(color_bw_, color_red_)) {
                        ESP_LOGI(kTag, "双屏 B（课程表）已刷新");
                    } else {
                        ESP_LOGW(kTag, "双屏 B 刷屏失败");
                    }
                } else {
                    ESP_LOGW(kTag, "双屏 B 拉取失败: %s", error.c_str());
                }
            } else {
                ESP_LOGW(kTag, "双屏 B 内存不足");
            }
        }
        // 双屏任一成功取图后落盘未绑定标志，使绑定设备下次不再卡在配对码帧。
        if (any_ok) inksight_device_unbound_.store(device_unbound);
        // 开机跳过首页菜单后，若首次双屏拉取彻底失败（断网/服务器不可达），
        // 回退显示一次首页菜单，避免白屏；后续失败不再重复全闪。
        if (!any_ok && !boot_home_fallback_done_.exchange(true)) {
            ESP_LOGI(kTag, "双屏首刷失败，回退首页菜单（断网/服务器不可达）");
            display_->ShowBootHome();
        }

        SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        cleanup();
        EnterIntervalSleepIfPending();
    }

    // FPC2 彩色 InkSight：先解包为 BW/RED 平面，再映射/直喂驱动；
    // 控制器的 BWRY 2bpp 码；FPC1 kInkSight 页只显示状态/进度。
    void RefreshInkSightColor(bool initial_full_refresh) {
        if (color_bw_ == nullptr) {
            color_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                Ssd2683zaColorPanel::kWidth / 8 * Ssd2683zaColorPanel::kHeight,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (color_red_ == nullptr) {
            color_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                Ssd2683zaColorPanel::kWidth / 8 * Ssd2683zaColorPanel::kHeight,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
#if FPC2_COLOR_COUNT == 4
        if (color_frame_ == nullptr) {
            color_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
                Ssd2683zaColorPanel::kFrameBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
#endif  // FPC2_COLOR_COUNT == 4
#if FPC2_COLOR_COUNT == 4
        if (color_bw_ == nullptr || color_red_ == nullptr ||
            color_frame_ == nullptr) {
#else
        if (color_bw_ == nullptr || color_red_ == nullptr) {
#endif  // FPC2_COLOR_COUNT == 4
            display_->dashboard().SetInkSightFooter("内存不足");
            return;
        }
        if (inksight_busy_.exchange(true)) {
            ESP_LOGW(kTag, "XiaowuAI color refresh already in progress, skip");
            return;
        }
        inksight_cancel_.store(false);
        auto cleanup = [this]() { inksight_busy_.store(false); };

        display_->dashboard().SetInkSightFooter("彩屏更新中…(约16秒)");
        Settings inksight_settings("inksight", /*read_write=*/false);
        std::string persona = inksight_settings.GetString("persona", "");
        if (persona == "auto" || persona == "AUTO") {
            persona.clear();
        }
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        std::string error;
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            if (inksight_cancel_.load()) {
                ESP_LOGI(kTag, "XiaowuAI color refresh cancelled by user");
                SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                cleanup();
                return;
            }
            if (attempt > 1) {
                vTaskDelay(pdMS_TO_TICKS(500));
                display_->dashboard().SetInkSightFooter("重试中…");
            }
            int server_refresh_minutes = 0;
            bool device_unbound = false;
            ok = InkSightClient::Load2bpp(
                GetNetwork(), inksight_server_, persona,
                SystemInfo::GetMacAddress(), color_bw_, color_red_, error,
                &server_refresh_minutes, &device_unbound);
            if (ok) inksight_device_unbound_.store(device_unbound);
            if (server_refresh_minutes > 0) {
                Settings writable("inksight", true);
                // ESP-IDF NVS keys are limited to 15 characters.
                writable.SetInt("clr_ref_min", server_refresh_minutes);
            }
        }
        SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (!ok) {
            const std::string friendly = FriendlyInkSightError(error);
            std::string tip = friendly.find("服务器") != std::string::npos ||
                                      friendly.find("连不") != std::string::npos
                                  ? friendly
                                  : "连不上服务器(" + friendly + ")";
            display_->dashboard().SetInkSightFooter(tip + " OK再试 · 长按返回");
            cleanup();
            return;
        }
        display_->dashboard().SetInkSightFooter(
            inksight_device_unbound_.load()
                ? "InkSight 未绑定：配对码已显示在 FPC2"
                : "正在刷彩屏，请稍候…");
        const bool refreshed = ShowColorFrame(color_bw_, color_red_);
        if (inksight_device_unbound_.load()) {
            display_->dashboard().SetInkSightFooter(
                refreshed ? "InkSight 未绑定：配对码在 FPC2"
                          : "配对码屏刷新失败 · OK重试");
        } else {
            display_->dashboard().SetInkSightFooter(
                refreshed ? std::string("模式: ") +
                                (persona.empty() ? "auto" : persona) +
                                " · OK刷新 · 长按返回"
                          : "彩屏刷新失败 OK再试");
        }
        ESP_LOGI(kTag, "XiaowuAI color screen refreshed (%s)",
                 initial_full_refresh ? "initial" : "manual");
        cleanup();
        EnterIntervalSleepIfPending();
    }

    void RefreshAlbumPhoto(const std::string& source, const std::string& name) {
        if (color_screen_ == nullptr || !color_screen_->Available()) {
            display_->dashboard().SetInkSightFooter("FPC2 彩色屏未连接");
            return;
        }
        if (color_bw_ == nullptr) {
            color_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                Ssd2683zaColorPanel::kWidth / 8 * Ssd2683zaColorPanel::kHeight,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (color_red_ == nullptr) {
            color_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                Ssd2683zaColorPanel::kWidth / 8 * Ssd2683zaColorPanel::kHeight,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
#if FPC2_COLOR_COUNT == 4
        if (color_frame_ == nullptr) {
            color_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
                Ssd2683zaColorPanel::kFrameBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
#endif  // FPC2_COLOR_COUNT == 4
#if FPC2_COLOR_COUNT == 4
        if (color_bw_ == nullptr || color_red_ == nullptr ||
            color_frame_ == nullptr) {
#else
        if (color_bw_ == nullptr || color_red_ == nullptr) {
#endif  // FPC2_COLOR_COUNT == 4
            display_->dashboard().SetInkSightFooter("相册图像内存不足");
            return;
        }
        if (inksight_busy_.exchange(true)) {
            display_->dashboard().SetInkSightFooter("彩屏正忙，请稍后再试");
            return;
        }

        std::string error;
        bool ok = false;
        if (source == "tf") {
            std::vector<uint8_t> jpeg;
            if (storage_.EnsureMounted() &&
                storage_.Load("images", name, jpeg, 6 * 1024 * 1024, error)) {
                ok = ConvertJpegToBwrPlanes(jpeg.data(), jpeg.size(), color_bw_,
                                            color_red_, error);
            } else if (error.empty()) {
                error = storage_.MountError().empty() ? "TF 卡不可用"
                                                       : storage_.MountError();
            }
        } else if (source == "backend") {
            SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            ok = InkSightClient::Load2bpp(
                GetNetwork(), inksight_server_, "MY_ADAPTIVE",
                SystemInfo::GetMacAddress(), color_bw_, color_red_, error);
            SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        } else {
            error = "source 只能是 tf 或 backend";
        }

        if (ok) {
            const bool refreshed = ShowColorFrame(color_bw_, color_red_);
            if (!refreshed) error = "FPC2 刷新失败";
        }
        display_->dashboard().SetInkSightFooter(
            ok && error.empty()
                ? (source == "tf" ? "相册显示完成: " + name
                                   : "已显示后台 Photo Frame")
                : "相册失败: " + FriendlyInkSightError(error));
        inksight_busy_.store(false);
    }

    // 双屏 B 屏的「TF 卡相册」：从 /sdcard/xiaowu/images 轮流取一张 JPG 刷 FPC2。
    // 图片在卡上，后端渲染不出来，所以这条路径完全在设备侧闭环；
    // 返回 false 时由调用方回落到网络图，不会把 B 屏留在空白。
    bool RefreshDualScreenFromTf() {
        if (color_bw_ == nullptr || color_red_ == nullptr) {
            ESP_LOGW(kTag, "TF 相册: 彩屏缓冲未就绪");
            return false;
        }
        if (!storage_.EnsureMounted()) {
            const std::string why = storage_.MountError();
            ESP_LOGW(kTag, "TF 相册: 卡未挂载 (%s)", why.c_str());
            return false;
        }
        const std::string listing = storage_.List("images");
        if (listing.empty() || listing == "(empty)") {
            ESP_LOGW(kTag, "TF 相册: /xiaowu/images 为空");
            return false;
        }
        const auto ends_with_ci = [](const std::string& s, const std::string& suf) {
            if (s.size() < suf.size()) return false;
            const size_t off = s.size() - suf.size();
            for (size_t i = 0; i < suf.size(); ++i) {
                char a = s[off + i];
                char b = suf[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) return false;
            }
            return true;
        };
        std::vector<std::string> names;
        size_t pos = 0;
        while (pos <= listing.size()) {
            const size_t nl = listing.find('\n', pos);
            const std::string item = listing.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? listing.size() + 1 : nl + 1;
            if (item.empty() || item[0] == '.' || item == "...") continue;
            if (ends_with_ci(item, ".jpg") || ends_with_ci(item, ".jpeg")) {
                names.push_back(item);
            }
        }
        if (names.empty()) {
            ESP_LOGW(kTag, "TF 相册: 目录里没有 JPG");
            return false;
        }
        // 轮换：每次刷新换下一张，让相册「活」起来。
        const std::string& name = names[dual_tf_album_index_ % names.size()];
        ++dual_tf_album_index_;
        std::vector<uint8_t> jpeg;
        std::string error;
        if (!storage_.Load("images", name, jpeg, 6 * 1024 * 1024, error)) {
            ESP_LOGW(kTag, "TF 相册: 读取 %s 失败: %s", name.c_str(), error.c_str());
            return false;
        }
        if (!ConvertJpegToBwrPlanes(jpeg.data(), jpeg.size(), color_bw_,
                                    color_red_, error)) {
            ESP_LOGW(kTag, "TF 相册: %s 解码失败: %s", name.c_str(), error.c_str());
            return false;
        }
        if (!ShowColorFrame(color_bw_, color_red_)) {
            ESP_LOGW(kTag, "TF 相册: 刷屏失败 %s", name.c_str());
            return false;
        }
        ESP_LOGI(kTag, "双屏 B（TF 相册）已刷新: %s", name.c_str());
        return true;
    }

    // FPC2 四色屏定时自动刷新：每分钟醒来，累计到 NVS 配置的间隔（默认 30
    // 分钟）就拉一次新画面。对话页时顺延，避免语音中抢带宽。
    //
    // ⚠️ 刷新必须在本任务内直接执行，禁止 Application::Schedule：一次刷新含
    // 后端渲染等待（10~40s）+ 彩屏全刷 BUSY 等待（20~45s），若占用主循环，
    // 语音唤醒等事件会被顺延几十秒——表现为「呼叫小悟没反应」。面板总线的
    // 并发安全由 EpaperBusGuard（epaper_bus_lock.h）保证。
    static void ColorRefreshTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS3Board*>(argument);
        // NetworkEvent::Connected means the InkSight HTTP client can run. Fetch
        // immediately so an unbound device shows its MAC/pair code on FPC2
        // while the XiaoZhi activation UI remains on the FPC1 information panel.
        vTaskDelay(pdMS_TO_TICKS(3000));
        self->ProbeDualMode();
        if (self->dual_mode_.load()) {
            self->RefreshDualScreens();
        } else {
            self->RefreshInkSight(/*initial_full=*/true);
        }
        int minutes_elapsed = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            const int interval_minutes = self->EffectiveRefreshMinutes();
            const bool device_unbound = self->inksight_device_unbound_.load();
            // Claim codes expire after 10 minutes. Refresh every 4 minutes while
            // unbound; normal image refresh still follows the user's interval.
            if (interval_minutes <= 0 && !device_unbound &&
                !self->sleep_mode_pending_.load()) continue;
            ++minutes_elapsed;
            const int effective_interval =
                (device_unbound || self->sleep_mode_pending_.load())
                    ? 1
                    : interval_minutes;
            const bool regular_refresh_due =
                minutes_elapsed >= effective_interval;
            if (regular_refresh_due) minutes_elapsed = 0;
            // 每分钟检查配置版本。网页「应用到设备」产生新 savedAt 后，
            // 不再等待默认 30 分钟的彩屏刷新周期，下一次检查即刷新双屏。
            const bool dual_changed = self->ProbeDualMode();
            if (dual_changed && self->dual_mode_.load()) {
                self->dual_refresh_pending_.store(true);
            }
            // 后端 X-Pending-Refresh：内容后台点了「应用到设备」→ 立即刷。
            const bool pending_now = InkSightClient::TakePendingRefresh();
            auto& dashboard = self->display_->dashboard();
            if (dashboard.IsConversation() && !device_unbound &&
                !pending_now) {
                // 配置版本已经记为 pending；语音结束后的下一分钟再刷，
                // 避免 A 屏在会话过程中被后台内容覆盖。
                continue;
            }
            if (self->dual_mode_.load()) {
                if (regular_refresh_due || pending_now ||
                    self->dual_refresh_pending_.exchange(false)) {
                    self->RefreshDualScreens();
                }
            } else if (regular_refresh_due || pending_now) {
                self->RefreshInkSight(false);
            }
        }
    }
#endif

    // 执行一次 InkSight 拉取+解码+刷屏幕（应用 Schedule 回调中调用）。
    // initial_full_refresh=true: 首次进入用全刷；false: 按 OK 快刷（局部）。
    void RefreshInkSight(bool initial_full_refresh) {
#if EPD_ENABLE_COLOR_SCREEN
        // 双屏模式：InkSight 图像归 FPC2 彩色屏，本函数转发；
        // FPC1 的 kInkSight 页只显示状态与按键提示。
        if (color_screen_ != nullptr && color_screen_->Available()) {
            RefreshInkSightColor(initial_full_refresh);
            return;
        }
        // FPC2 未插（探测失败）时走下面的 1bpp 路径，画面降级显示在
        // 快刷屏上，行为与旧单屏固件一致。
#endif
        if (inksight_frame_ == nullptr) {
            inksight_frame_ = static_cast<uint8_t*>(heap_caps_malloc(
                InkSightClient::kFrameBytes,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (inksight_frame_ == nullptr) {
            display_->dashboard().SetInkSightFooter("内存不足");
            return;
        }
        if (inksight_busy_.exchange(true)) {
            ESP_LOGW(kTag, "XiaowuAI refresh already in progress, skip");
            return;
        }
        display_->dashboard().SetInkSightFooter("正在更新画面…");
        inksight_cancel_.store(false);
        auto cleanup = [this]() { inksight_busy_.store(false); };

        // 看板模式（persona）：NVS "inksight"/"persona"（MCP 工具
        // self.epaper.set_inksight_persona 可设置）。
        // 空 = 默认 WEATHER 天气页；"auto" = 不带 persona 参数，由后端
        // 按该设备(MAC)的模式配置与轮换策略决定内容。
        Settings inksight_settings("inksight", /*read_write=*/false);
        std::string persona = inksight_settings.GetString("persona", "");
        if (persona == "auto" || persona == "AUTO") {
            persona.clear();
        }
        std::string error;
        // 空闲时 WiFi 处于 LOW_POWER(MAX_MODEM) 最大省电，突发 TCP 下载易被
        // AP 缓冲丢包掐断流（语音对话时框架会临时切 PERFORMANCE 所以没这问题）。
        // 下载期间临时切 PERFORMANCE，结束后恢复。
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        bool ok = false;
        bool device_unbound = false;
        std::string pair_code;
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            if (inksight_cancel_.load()) {
                ESP_LOGI(kTag, "XiaowuAI refresh cancelled by user");
                SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                cleanup();
                return;
            }
            if (attempt > 1) {
                // 服务端渲染结果有缓存，重试通常命中缓存立即返回
                vTaskDelay(pdMS_TO_TICKS(500));
                display_->dashboard().SetInkSightFooter("重试中…");
            }
            ok = InkSightClient::Load(
                GetNetwork(), inksight_server_, persona,
                SystemInfo::GetMacAddress(), inksight_frame_, error,
                nullptr, -1, &device_unbound, &pair_code);
        }
        SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (!ok) {
            const std::string friendly = FriendlyInkSightError(error);
            std::string tip = friendly.find("服务器") != std::string::npos ||
                                      friendly.find("连不") != std::string::npos
                                  ? friendly
                                  : "连不上服务器(" + friendly + ")";
            display_->dashboard().SetInkSightFooter(tip + " OK再试 · 长按返回");
            // 开机跳过首页菜单后，首刷若连不上服务器，回退显示一次首页菜单，
            // 避免白屏（后续失败不再重复全闪；成功上屏后由 RawFrameRefresh 覆盖）。
            if (!boot_home_fallback_done_.exchange(true)) {
                ESP_LOGI(kTag, "XiaowuAI 首刷失败，回退首页菜单");
                display_->ShowBootHome();
            }
            cleanup();
            return;
        }
        const bool was_unbound = inksight_device_unbound_.exchange(device_unbound);
        // 已绑定、但后台还没配任何双屏内容（也没手动指定 persona）：A 屏保
        // 持开机欢迎页，引导用户去后台配置；B 屏由 RefreshInkSightColor 继续
        // 渲染默认天气。否则后端对"未配置设备"会返回便签占位页，把欢迎页盖掉，
        // 用户配完网看到的是一张空白便签，以为设备坏了。
        // 只在当前正停在欢迎页时拦截：用户若是手动翻到看板页，仍让他看到
        // 后端返回的占位画面，不至于点了没反应。
        if (!device_unbound && !dual_mode_.load() && persona.empty() &&
            display_->dashboard().IsWelcome()) {
            ESP_LOGI(kTag, "后台未配置双屏内容：A 屏保持欢迎页（B 屏继续刷天气）");
            cleanup();
            return;
        }
        display_->RawFrameRefresh(inksight_frame_,
                                  !initial_full_refresh /* partial */);
        if (device_unbound) {
            display_->dashboard().SetInkSightFooter(
                pair_code.empty() ? "未绑定：配对码见 FPC1 · OK刷新"
                                  : "未绑定 · 配对码 " + pair_code + " · 后台输入绑定");
            // FPC2 不可用时配对码只画在 FPC1，用户不翻到看板页就永远看不到，
            // 所以未绑定状态下自动切一次（was_unbound 保证只切一次，不会递归）。
            if (!was_unbound && !display_->dashboard().IsInkSight()) {
                ESP_LOGI(kTag, "未绑定：自动切到 XiaowuAI 页显示配对码 %s",
                         pair_code.c_str());
                display_->dashboard().Show(EpaperDashboard::Page::kInkSight);
            }
        } else {
            display_->dashboard().SetInkSightFooter("OK刷新 · 长按返回");
        }
        ESP_LOGI(kTag, "XiaowuAI refreshed (%s)",
                 initial_full_refresh ? "full" : "partial");
        cleanup();
    }

    static void RecordTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS3Board*>(argument);
        char filename[48];
        const uint32_t recording_id =
            static_cast<uint32_t>((esp_timer_get_time() / 1000) & 0xffffffffU);
        std::snprintf(filename, sizeof(filename), "recording_%lu.wav",
                      static_cast<unsigned long>(recording_id));
        ESP_LOGI(kTag, "Recording started: /xiaowu/recordings/%s (10 seconds)",
                 filename);
        std::string error;
        const bool ok = self->storage_.RecordWav(
            self->GetAudioCodec(), filename, 10, error);
        if (ok) {
            ESP_LOGI(kTag, "Recording saved: /xiaowu/recordings/%s", filename);
            self->ReportTfStorage();
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

    std::vector<std::string> ListTextBooks() {
        const std::string listing = storage_.List("books");
        std::vector<std::string> books;
        size_t start = 0;
        while (start < listing.size()) {
            const size_t end = listing.find('\n', start);
            std::string name = listing.substr(start, end == std::string::npos
                                                         ? std::string::npos
                                                         : end - start);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool text_book =
                (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".txt") ||
                (lower.size() >= 3 && lower.substr(lower.size() - 3) == ".md");
            if (text_book) books.push_back(name);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        std::sort(books.begin(), books.end());
        ESP_LOGI(kTag, "TF book scan: %u TXT file(s)",
                 static_cast<unsigned>(books.size()));
        return books;
    }

    void ShowBookLibrary() {
        if (!storage_.EnsureMounted()) {
            ESP_LOGW(kTag, "TF book scan: card not mounted");
            std::string reason = storage_.MountError();
            if (reason.empty()) {
                reason = "TF 卡未挂载\n\n请插入 FAT32 格式的 TF 卡\n然后重新进入“阅读”";
            }
            display_->dashboard().ShowBookError(reason);
            return;
        }
        const auto books = ListTextBooks();
        if (books.empty()) {
            // Surface the raw directory state so the root cause (wrong folder,
            // empty directory, wrong extension) is visible instead of a
            // generic failure.
            const std::string listing = storage_.List("books");
            ESP_LOGW(kTag, "TF book scan empty: /sdcard/xiaowu/books -> %s",
                     listing.c_str());
            std::string message = "没有找到 TXT 电子书\n\n请放入 /xiaowu/books\n\n";
            message += "目录状态: ";
            message += listing;
            display_->dashboard().ShowBookError(message);
            return;
        }
        display_->dashboard().ShowBookList(books);
    }

    void OpenSelectedBook() {
        const std::string name = display_->dashboard().SelectedBook();
        if (name.empty()) {
            display_->dashboard().ShowBookError("没有选择电子书");
            return;
        }
        std::vector<uint8_t> bytes;
        std::string error;
        if (!storage_.Load("books", name, bytes, 2 * 1024 * 1024, error)) {
            display_->dashboard().ShowBookError("读取失败：" + error);
            return;
        }
        const size_t offset = bytes.size() >= 3 && bytes[0] == 0xef &&
                                      bytes[1] == 0xbb && bytes[2] == 0xbf
                                  ? 3
                                  : 0;
        const size_t dot = name.find_last_of('.');
        display_->dashboard().OpenBook(
            dot == std::string::npos ? name : name.substr(0, dot),
            std::string(reinterpret_cast<const char*>(bytes.data() + offset),
                        bytes.size() - offset));
    }

    // Loads a [[img:...]] illustration PNG from /xiaowu/books. PNG only;
    // the LVGL JPEG decoders are not enabled in this build.
    std::unique_ptr<LvglImage> LoadBookImage(const std::string& name) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!storage_.Load("books", name, bytes, 4 * 1024 * 1024, error)) {
            ESP_LOGW(kTag, "Book image %s load failed: %s", name.c_str(),
                     error.c_str());
            return nullptr;
        }
        void* data =
            heap_caps_malloc(bytes.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data == nullptr) return nullptr;
        memcpy(data, bytes.data(), bytes.size());
        try {
            return std::make_unique<LvglAllocatedImage>(data, bytes.size());
        } catch (...) {
            heap_caps_free(data);
            return nullptr;
        }
    }

    void InitializePowerAmplifier() {
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
        // NS4150B CTRL is active-high. Keep it permanently enabled and do not
        // let esp_codec_dev toggle it as the microphone/output states change.
        ESP_ERROR_CHECK(gpio_reset_pin(AUDIO_CODEC_PA_CTRL_PIN));
        ESP_ERROR_CHECK(gpio_set_direction(AUDIO_CODEC_PA_CTRL_PIN,
                                           GPIO_MODE_INPUT_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(AUDIO_CODEC_PA_CTRL_PIN, 1));
        ESP_LOGI(kTag, "AUDIO PA: NS4150B enabled permanently: GPIO%d pad=%d",
                 AUDIO_CODEC_PA_CTRL_PIN,
                 gpio_get_level(AUDIO_CODEC_PA_CTRL_PIN));
#else
        // MAX98357A SD_MODE is pulled up on the board. Drive it high so the
        // amplifier stays enabled while the shared audio service is running.
        ESP_ERROR_CHECK(gpio_reset_pin(AUDIO_SPK_SD_MODE_PIN));
        ESP_ERROR_CHECK(gpio_set_direction(AUDIO_SPK_SD_MODE_PIN,
                                           GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(AUDIO_SPK_SD_MODE_PIN, 1));
        ESP_LOGI(kTag, "AUDIO PA: MAX98357A enabled: GPIO%d",
                 AUDIO_SPK_SD_MODE_PIN);
#endif
    }

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
            ESP_LOGE(kTag, "AUDIO: I2C bus creation FAILED: %s (0x%x). "
                     "Check if SDA/SCL are shorted to GND or occupied by other peripherals.",
                     esp_err_to_name(bus_ret), bus_ret);
            return;
        }
        ESP_LOGI(kTag, "AUDIO: I2C bus created successfully.");

        // esp_codec_dev stores the 8-bit ES8311 address (0x30), while the IDF
        // probe API expects the corresponding 7-bit address (0x18).
        const uint8_t address_7bit = AUDIO_CODEC_ES8311_ADDR >> 1;
        ESP_LOGI(kTag, "AUDIO: Probing ES8311 at 0x%02X (7-bit)...", address_7bit);
        const esp_err_t probe = i2c_master_probe(i2c_bus_, address_7bit, 100);
        if (probe == ESP_OK) {
            ESP_LOGI(kTag,
                     "AUDIO CODEC PROBE RESULT: SUCCESS — ES8311 ACK at 0x%02X (SDA=%d SCL=%d). "
                     "Voice input hardware is reachable. Note: '呼叫小智' still requires "
                     "srmodels asset binary flashed into the assets partition.",
                     address_7bit, AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
        } else {
            ESP_LOGE(kTag,
                     "AUDIO CODEC PROBE RESULT: FAILURE — ES8311 NO-ACK at 0x%02X (SDA=%d SCL=%d): %s. "
                     "Without ES8311 the microphone path is dead — '呼叫小悟' will NEVER trigger. "
                     "Check: (1) codec soldering, (2) 3.3V supply to ES8311, (3) SDA/SCL continuity, "
                     "(4) ES8311 ADDR1/ADDR0 pins match 0x18.",
                     address_7bit, AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN,
                     esp_err_to_name(probe));
        }
#endif
    }

    // 重新配对前先解除后端绑定：MAC 不会因为换 WiFi 而改变，不解绑的话设备
    // 永远停在「已绑定」分支，屏幕上再也不会出现配对码（换账号/换主人时也是
    // 同一个死结）。解绑后设备下次 /api/render 就会拿到带配对码的引导图。
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
            ESP_LOGI(kTag, "重新配对：已解除绑定，配网完成后会显示配对码");
        } else {
            ESP_LOGW(kTag, "重新配对：解绑请求失败(%s)，联网后自动重试",
                     error.c_str());
        }
    }

    void HandleKeyUp() {
        ESP_LOGI(kTag, "KEY+ click: GPIO%d", KEY_UP_GPIO);
        if (display_->dashboard().IsHome()) {
            // '+' means forward/next on the home menu.
            display_->dashboard().Next();
            return;
        }
        if (display_->dashboard().IsInkSight()) {
            // 今日看板：上键 = 重新拉取画面（全刷清除残影）。
            Application::GetInstance().Schedule(
                [this]() { RefreshInkSight(/*initial_full=*/true); });
            return;
        }
        if (display_->dashboard().IsBookList()) {
            display_->dashboard().BookListPrevious();
            return;
        }
        if (display_->dashboard().IsBookOpen()) {
            display_->dashboard().BookPrevious();
            return;
        }
        if (display_->dashboard().IsWords()) {
            display_->dashboard().WordPrevious();
            return;
        }
        if (display_->dashboard().IsProfile()) {
            // 用户中心：上键移动到上一个设置项。
            display_->dashboard().ProfilePrev();
            return;
        }
        auto* codec = GetAudioCodec();
        const int volume = std::min(100, codec->output_volume() + 10);
        codec->SetOutputVolume(volume);
        ESP_LOGI(kTag, "KEY+ volume=%d", volume);
    }

    void HandleKeyDown() {
        ESP_LOGI(kTag, "KEY- click: GPIO%d", KEY_DOWN_GPIO);
        if (display_->dashboard().IsHome()) {
            // '-' means backward/previous on the home menu.
            display_->dashboard().Previous();
            return;
        }
        if (display_->dashboard().IsInkSight()) {
            // 今日看板：下键 = 切换看板模式。
            CycleInkSightPersona();
            return;
        }
        if (display_->dashboard().IsBookList()) {
            display_->dashboard().BookListNext();
            return;
        }
        if (display_->dashboard().IsBookOpen()) {
            display_->dashboard().BookNext();
            return;
        }
        if (display_->dashboard().IsWords()) {
            display_->dashboard().WordNext();
            return;
        }
        if (display_->dashboard().IsProfile()) {
            // 用户中心：下键移动到下一个设置项。
            display_->dashboard().ProfileNext();
            return;
        }
        auto* codec = GetAudioCodec();
        const int volume = std::max(0, codec->output_volume() - 10);
        codec->SetOutputVolume(volume);
        ESP_LOGI(kTag, "KEY- volume=%d", volume);
    }

    void HandleKeyOk() {
        ESP_LOGI(kTag, "KEY_OK click: GPIO%d", KEY_OK_GPIO);
        // 欢迎页（出厂/未配置默认页）按 OK 进入功能菜单，而不是唤醒对话。
        if (display_->dashboard().IsWelcome()) {
            display_->dashboard().Enter();
            return;
        }
        if (display_->dashboard().IsHome()) {
            const int selected = display_->dashboard().SelectedItem();
            ESP_LOGI(kTag, "KEY_OK enter home menu item %d/7", selected + 1);
            if (selected == 0) {
                display_->dashboard().Show(EpaperDashboard::Page::kSpeaking);
                StartLightAcade();
            } else if (selected == 1) {
                // 阅读 first opens a selectable TF-card library.
                ShowBookLibrary();
            } else {
                display_->dashboard().Enter();
            }
            return;
        }
        if (display_->dashboard().IsBookList()) {
            OpenSelectedBook();
            return;
        }
        if (display_->dashboard().IsWords()) {
            // OK drives the recall flashcard flow on the words page.
            display_->dashboard().WordEnter();
            return;
        }
        if (display_->dashboard().IsInkSight()) {
            // 今日看板：OK = 重新拉一次画面（差分快刷）。
            Application::GetInstance().Schedule(
                [this]() { RefreshInkSight(/*initial_full=*/false); });
            return;
        }
        if (display_->dashboard().IsProfile()) {
            // 用户中心：OK 循环切换当前设置项的值并保存。
            display_->dashboard().ProfileConfirm();
            return;
        }

        // Outside the home menu, short OK remains the physical Xiaozhi wake
        // key; long OK is handled separately and returns home.
        display_->dashboard().BeginConversation();
        Application::GetInstance().ToggleChatState();
    }

    static void KeyScanTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS3Board*>(argument);
        constexpr gpio_num_t pins[] = {KEY_UP_GPIO, KEY_OK_GPIO, KEY_DOWN_GPIO};
        int raw[3] = {1, 1, 1};
        int stable[3] = {1, 1, 1};
        int samples[3] = {0, 0, 0};
        int64_t pressed_at[3] = {0, 0, 0};
        int64_t pending_ok_release = 0;
        // The OK switch bounces for up to a few hundred ms AFTER a long
        // press is released; those edges used to register as a fresh click
        // and re-triggered the menu item (or the voice assistant) right
        // after "long press = back home". Swallow every OK edge in this
        // window.
        int64_t ok_suppress_until = 0;
        for (int i = 0; i < 3; ++i) raw[i] = stable[i] = gpio_get_level(pins[i]);
        ESP_LOGI(kTag, "KEY raw idle: +=%d OK=%d -=%d", stable[0], stable[1], stable[2]);

        while (true) {
            const int64_t now = esp_timer_get_time() / 1000;
            for (int i = 0; i < 3; ++i) {
                const int value = gpio_get_level(pins[i]);
                if (value == raw[i]) ++samples[i];
                else { raw[i] = value; samples[i] = 1; }
                if (samples[i] < 3 || stable[i] == value) continue;
                stable[i] = value;
                // 任何按键动作都要把屏幕从外部画面（InkSight / 双屏 A 便签）
                // 交还给仪表盘，否则按键后看不到任何界面变化。
                self->display_->ReleaseExternalFrameHold();
                ESP_LOGI(kTag, "KEY GPIO%d stable=%d", pins[i], value);
                if (value == 0) {
                    pressed_at[i] = now;
                } else {
                    const int64_t held = now - pressed_at[i];
                    if (i == 0) {
                        if (held >= 3000) {
                            // KEY+ 长按 3 秒：重新配对（解绑 + 起配网热点）。
                            // 解绑后设备下次渲染会显示配对码，可在双屏后台
                            // / 网页重新绑定到账号。
                            ESP_LOGI(kTag, "KEY+ long press: re-pair (unbind + wifi config)");
                            Application::GetInstance().Schedule([self]() {
                                self->UnbindForReconfig();
                                self->EnterWifiConfigMode();
                            });
                        } else {
                            Application::GetInstance().Schedule(
                                [self]() { self->HandleKeyUp(); });
                        }
                    }
                    else if (i == 2) {
                        if (held >= 2000) {
                            Application::GetInstance().Schedule(
                                [self]() { self->EnterWifiConfigMode(); });
                        } else {
                            Application::GetInstance().Schedule(
                                [self]() { self->HandleKeyDown(); });
                        }
                    } else if (held >= 2000) {
                        pending_ok_release = 0;
                        ok_suppress_until = now + 800;
                        ESP_LOGI(kTag, "KEY_OK long press: return home");
                        // InkSight 下载可能在 Schedule 队列里阻塞返回操作，
                        // 设置取消标志让下载循环尽快退出。
                        self->inksight_cancel_.store(true);
                        Application::GetInstance().Schedule([self]() {
                            self->StopLightAcade();
                            self->display_->dashboard().Back();
                        });
                    } else if (now < ok_suppress_until) {
                        // Bounce (or an accidental touch) right after the
                        // long press: ignore instead of firing a ghost
                        // click.
                    } else if (pending_ok_release != 0 &&
                               now - pending_ok_release <= 400) {
                        pending_ok_release = 0;
                        Application::GetInstance().Schedule(
                            [self]() { self->StartRecording(); });
                    } else {
                        pending_ok_release = now;
                    }
                }
            }
            if (pending_ok_release != 0) {
                if (now < ok_suppress_until) {
                    pending_ok_release = 0;
                } else if (now - pending_ok_release > 400) {
                    pending_ok_release = 0;
                    Application::GetInstance().Schedule(
                        [self]() { self->HandleKeyOk(); });
                }
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
        xTaskCreate(KeyScanTask, "epaper_keys", 3072, this, 4, nullptr);
    }

    void InitializeDashboardTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.lightacade.start",
                    "启动固定 LightAcade 服务的专业英语口语陪练。",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        display_->dashboard().Show(
                            EpaperDashboard::Page::kSpeaking);
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
        mcp.AddTool("self.epaper.show_page",
                    "显示墨水屏信息页。page 可选 home、english、reading、words、schedule、todo、quota、calendar。",
                    PropertyList({Property("page", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        display_->dashboard().Show(properties["page"].value<std::string>());
                        return display_->dashboard().CurrentPageName();
                    });
        mcp.AddTool("self.epaper.set_schedule",
                    "保存并显示课程表。content 可为旧版纯文本，或包含 semester_start、courses(day/name/start/end/room) 的 JSON。",
                    PropertyList({Property("content", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        display_->dashboard().SetSchedule(properties["content"].value<std::string>());
                        display_->dashboard().Show(EpaperDashboard::Page::kSchedule);
                        return true;
                    });
        mcp.AddTool("self.epaper.set_note",
                    "设置电子纸便签内容。",
                    PropertyList({Property("content", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        display_->dashboard().SetAlbumCaption(properties["content"].value<std::string>());
                        display_->dashboard().Show(EpaperDashboard::Page::kNotes);
                        return true;
                    });
        mcp.AddTool("self.epaper.set_todos",
                    "保存并显示待办列表。json 为数组，每项包含 text、priority(1高/2中/3低)、completed。",
                    PropertyList({Property("json", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        display_->dashboard().SetTodos(properties["json"].value<std::string>());
                        display_->dashboard().Show(EpaperDashboard::Page::kTodo);
                        return true;
                    });
        mcp.AddTool("self.epaper.set_quota",
                    "更新通用 API 额度看板数据。",
                    PropertyList({
                        Property("balance", kPropertyTypeInteger),
                        Property("used", kPropertyTypeInteger),
                        Property("requests", kPropertyTypeInteger),
                        Property("tokens", kPropertyTypeInteger),
                    }),
                    [this](const PropertyList& properties) -> ReturnValue {
                        display_->dashboard().SetQuota(
                            properties["balance"].value<int>(), properties["used"].value<int>(),
                            properties["requests"].value<int>(), properties["tokens"].value<int>());
                        display_->dashboard().Show(EpaperDashboard::Page::kQuota);
                        return true;
                    });
        mcp.AddTool("self.epaper.set_inksight_server",
                    "设置 InkSight 后端地址（今日看板画面来源）。参数 url 形如 "
                    "https://dual.xiaowuai.vip/backend （不要带 /api/render 后缀）。"
                    "设置后会立即用新地址刷新一次看板。",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        std::string url = properties["url"].value<std::string>();
                        while (!url.empty() && url.back() == '/') url.pop_back();
                        InkSightClient::SaveServerUrl(url);
                        inksight_server_ = url;
                        ESP_LOGI(kTag, "XiaowuAI server saved: %s", url.c_str());
                        display_->dashboard().Show(EpaperDashboard::Page::kInkSight);
                        return true;
                    });
        mcp.AddTool("self.epaper.set_inksight_persona",
                    "设置今日看板模式。mode=auto 时由后端按设备(MAC)的模式配置/"
                    "轮换策略决定内容；也可传后端支持的模式 ID（如 WEATHER、ZEN、"
                    "POETRY、ROAST、DAILY 或自定义模式 ID，可查后端 /api/modes/catalog）。"
                    "设置后立即刷新看板。",
                    PropertyList({Property("mode", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        std::string mode = properties["mode"].value<std::string>();
                        Settings s("inksight", /*read_write=*/true);
                        s.SetString("persona", mode);
                        ESP_LOGI(kTag, "XiaowuAI persona saved: %s", mode.c_str());
                    display_->dashboard().Show(EpaperDashboard::Page::kInkSight);
                    return true;
                });
#if EPD_ENABLE_COLOR_SCREEN
        mcp.AddTool("self.epaper.set_power_mode",
                    "设置设备电源模式：active=活跃（常在线、短刷新），interval=间歇（按间隔刷新、空闲深睡）。原硬件 KEY+ 长按已改为重新配对，电源模式改由本工具设置。",
                    PropertyList({Property("mode", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string mode =
                            properties["mode"].value<std::string>();
                        if (mode != "active" && mode != "interval") {
                            return std::string("invalid mode (use active|interval)");
                        }
                        SetPowerMode(mode == "active" ? PowerMode::kActive
                                                     : PowerMode::kIntermittent);
                        return true;
                    });
#endif
#if EPD_ENABLE_COLOR_SCREEN
        mcp.AddTool("self.epaper.refresh_color_screen",
                    "立即刷新 FPC2 FPC2 彩色屏的 InkSight 模式画面。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        display_->dashboard().Show(EpaperDashboard::Page::kInkSight);
                        Application::GetInstance().Schedule(
                            [this]() { RefreshInkSight(false); });
                        return true;
                    });
        mcp.AddTool("self.epaper.refresh_dual",
                    "立即拉取双屏后台（dual-config）并刷新 A/B 两屏。A、B 各显示什么由后台场景配置决定："
                    "学习场景通常是 A 便签 + B 课程表；理财场景是 A 自选行情 + B 某只证券的日 K 图。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        Application::GetInstance().Schedule([this]() {
                            ProbeDualMode();
                            RefreshDualScreens();
                        });
                        return std::string("已开始刷新双屏内容（按后台场景配置）");
                    });
        mcp.AddTool("self.epaper.set_kline",
                    "把 B 屏切换成某只股票/指数的日 K 图（理财场景）。query 可填 6 位代码或中文名称，"
                    "例如 688018、600519、乐鑫科技、贵州茅台、上证指数。A 屏会同步切到自选行情列表。"
                    "切换后会立即刷新双屏，几秒内生效。",
                    PropertyList({Property("query", kPropertyTypeString)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const std::string query = properties["query"].value<std::string>();
                        if (query.empty()) return std::string("缺少股票名称或代码");
                        std::string name, error;
                        const bool ok = InkSightClient::SetFinanceKline(
                            GetNetwork(), inksight_server_,
                            SystemInfo::GetMacAddress(), query, &name, &error);
                        if (!ok) {
                            return std::string("切换失败：") + (error.empty() ? "未知错误" : error);
                        }
                        ESP_LOGI(kTag, "B 屏 K 线切换为 %s（%s）", name.c_str(), query.c_str());
                        Application::GetInstance().Schedule([this]() {
                            ProbeDualMode();
                            RefreshDualScreens();
                        });
                        return std::string("已把 B 屏切换为 ") + name + " 的日 K 图，正在刷新";
                    });
        mcp.AddTool("self.epaper.set_color_refresh_minutes",
                    "设置 FPC2 FPC2 彩色屏自动刷新间隔（分钟，1~1440；0=关闭自动刷新）。",
                    PropertyList({Property("minutes", kPropertyTypeInteger, 0, 1440)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const int minutes =
                            properties["minutes"].value<int>();
                        Settings s("inksight", /*read_write=*/true);
                        s.SetInt("clr_ref_min", minutes);
                        ESP_LOGI(kTag, "Color screen refresh interval: %d min",
                                 minutes);
                        return true;
                    });
        mcp.AddTool("self.epaper.screen_status",
                    "查看双屏配置与状态（FPC1 SSD1619、FPC2 彩色屏、InkSight 服务器与模式）。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        std::string status = "bw_screen(fast):ok";
#if EPD_ENABLE_COLOR_SCREEN
                        const char* fpc2_key =
#if FPC2_COLOR_COUNT == 3
                            "FPC2_UC1679_BWR";
#else
                            "FPC2_SSD2683ZA_BWRY";
#endif
                        status += color_screen_ != nullptr &&
                                          color_screen_->Available()
                                      ? std::string("; ") + fpc2_key + ":ok"
                                      : std::string("; ") + fpc2_key + ":not_detected";
#else
                        status += "; FPC2_COLOR_DISABLED:disabled";
#endif
                        status += "; server=" + inksight_server_;
                        Settings s("inksight", false);
                        status += "; persona=" + s.GetString("persona", "");
                        return status;
                    });
#endif
        mcp.AddTool("self.storage.status", "查看 TF 卡挂载状态和剩余容量。", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue { return storage_.Status(); });
        mcp.AddTool("self.storage.list", "列出 TF 卡文件。category 可选 images、audio、recordings。",
                    PropertyList({Property("category", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return storage_.List(p["category"].value<std::string>());
                    });
        mcp.AddTool("self.storage.rescan",
                    "重新探测并挂载 TF 卡。卡座无插拔检测，带电插卡后用它可以免重启。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        // 卡座没有 CD 脚，开机只挂载一次；用户带着设备插卡后必须
                        // 显式重探，否则界面会一直停在「未挂载」。
                        if (storage_.mounted()) return storage_.Status();
                        if (storage_.EnsureMounted()) return storage_.Status();
                        return storage_.MountError();
                    });
        mcp.AddTool("self.epaper.show_sd_image", "在 FPC2 FPC2 彩色屏显示 TF 卡 /xiaowu/images 中的 JPG/JPEG 图片。",
                    PropertyList({Property("name", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        const std::string name = p["name"].value<std::string>();
#if EPD_ENABLE_COLOR_SCREEN
                        Application::GetInstance().Schedule(
                            [this, name]() { RefreshAlbumPhoto("tf", name); });
                        return std::string("已开始在 FPC2 显示 ") + name;
#else
                        return std::string("当前固件未启用 FPC2 彩屏");
#endif
                    });
        mcp.AddTool("self.epaper.show_backend_album",
                    "从 InkSight 后台 MY_ADAPTIVE Photo Frame 读取图片并显示到 FPC2 FPC2 彩色屏。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
#if EPD_ENABLE_COLOR_SCREEN
                        Application::GetInstance().Schedule(
                            [this]() { RefreshAlbumPhoto("backend", ""); });
                        return std::string("已开始从后台 Photo Frame 刷新 FPC2");
#else
                        return std::string("当前固件未启用 FPC2 彩屏");
#endif
                    });
        mcp.AddTool("self.audio.play_sd_wav", "播放 TF 卡 /xiaowu/audio 中的单声道 16-bit 24kHz PCM WAV。",
                    PropertyList({Property("name", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string error;
                        return storage_.PlayWav(GetAudioCodec(), p["name"].value<std::string>(), error)
                                   ? ReturnValue(true) : ReturnValue(error);
                    });
        mcp.AddTool("self.audio.record_sd_wav", "录音到 TF 卡 /xiaowu/recordings，seconds 为 1~60 秒。",
                    PropertyList({Property("name", kPropertyTypeString),
                                  Property("seconds", kPropertyTypeInteger, 1, 60)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string error;
                        return storage_.RecordWav(GetAudioCodec(), p["name"].value<std::string>(),
                                                  p["seconds"].value<int>(), error)
                                   ? ReturnValue(true) : ReturnValue(error);
                    });
        mcp.AddTool("self.audio_mic.set_gain", "设置麦克风数字输入增益（dB，0~43）。立即生效并持久化，无需重启。",
                    PropertyList({Property("gain", kPropertyTypeInteger, 0, 43)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        float gain = static_cast<float>(p["gain"].value<int>());
                        GetAudioCodec()->SetInputGain(gain);
                        return ReturnValue(true);
                    });
    }

public:
    Ssd2683zaS3Board() {
        InitializePowerAmplifier();
        InitializeI2c();
        display_ = new Ssd1619Display();
#if EPD_A_PANEL_COLORS == 3
        // A 座三色屏：建一个指向 A 座的实例，接管全部上屏动作。
        // 不走 Init()——它会先刷一张全白探测帧（约 28 s），开机白等半分钟；
        // ConfigurePanel() 本来每次刷新都会复位并重测 BUSY 极性，够用。
        color_panel_a_ = new Uc1679Display(nullptr, EPD_CS_PIN, EPD_BUSY_PIN);
        color_panel_a_->ForceAvailable();
        display_->SetBwrBackend(color_panel_a_);
        ESP_LOGW(kTag, "A 座按三色屏驱动（SSD1619 命令已停用）：CS=GPIO%d BUSY=GPIO%d",
                 EPD_CS_PIN, EPD_BUSY_PIN);
#elif EPD_A_PANEL_COLORS == 4
        // A 座四色屏：SSD2683ZA 与三色屏同座电气兼容，但帧是 2bpp packed，
        // 由 board 直接调用，Ssd1619Display 不再向该座发任何命令。
        color_panel_a_ = new Ssd2683zaColorPanel(nullptr, EPD_CS_PIN, EPD_BUSY_PIN);
        display_->SetExternalColorPanel(true);
        if (color_panel_a_->Init()) {
            ESP_LOGW(kTag, "A 座按四色屏驱动（SSD1619 命令已停用）：CS=GPIO%d BUSY=GPIO%d",
                     EPD_CS_PIN, EPD_BUSY_PIN);
        } else {
            ESP_LOGE(kTag, "A 座四色屏初始化失败，双屏 A 内容将无法显示");
        }
#else
        ESP_LOGI(kTag, "A 座按黑白屏（SSD1619）驱动：CS=GPIO%d BUSY=GPIO%d",
                 EPD_CS_PIN, EPD_BUSY_PIN);
#endif
        display_->dashboard().SetBookImageLoader(
            [this](const std::string& name) { return LoadBookImage(name); });
        display_->dashboard().SetWordListLoader([this]() {
            if (!storage_.EnsureMounted()) return std::string();
            std::vector<uint8_t> bytes;
            std::string error;
            if (!storage_.Load("words", "words.txt", bytes, 512 * 1024,
                               error)) {
                ESP_LOGW(kTag, "Word list load failed: %s", error.c_str());
                return std::string();
            }
            size_t offset = 0;
            if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
                bytes[2] == 0xbf) {
                offset = 3;  // skip the UTF-8 BOM
            }
            return std::string(
                reinterpret_cast<const char*>(bytes.data() + offset),
                bytes.size() - offset);
        });
        inksight_server_ = InkSightClient::LoadServerUrl();
        // 一次性迁移：仅把历史上硬编码的旧内网 / NAS 地址迁到线上域名。
        // 用 NVS 标记 server_mig_v1 保证只跑一次——之后用户用 MCP
        // self.epaper.set_inksight_server 设的任意本地地址（含 192.168.x.x）
        // 都不会被再次覆盖，重启后保留，方便本地联调。
        {
            Settings s("inksight", /*read_write=*/true);
            if (!s.GetBool("server_mig_v1", false)) {
                if (inksight_server_ == "http://res123.zapto.org:8090" ||
                    inksight_server_ == "res123.zapto.org:8090" ||
                    inksight_server_ == "http://nas.insane.vip:8080" ||
                    inksight_server_ == "nas.insane.vip:8080" ||
                    inksight_server_ == "http://nas.insane.vip:8090" ||
                    inksight_server_ == "nas.insane.vip:8090" ||
                    inksight_server_ == "http://nas.insane.vip:8091" ||
                    inksight_server_ == "nas.insane.vip:8091" ||
                    inksight_server_ == "http://nas.insane.vip:8092" ||
                    inksight_server_ == "nas.insane.vip:8092" ||
                    // 旧编译期默认值：办公点内网 IP + 8080/8092/8104 历史端口。
                    inksight_server_ == "http://192.168.1.4:8092" ||
                    inksight_server_ == "192.168.1.4:8092" ||
                    inksight_server_ == "http://192.168.1.4:8104" ||
                    inksight_server_ == "192.168.1.4:8104" ||
                    inksight_server_ == "http://192.168.101.69:8092" ||
                    inksight_server_ == "192.168.101.69:8092" ||
                    inksight_server_ == "http://192.168.101.69:8104" ||
                    inksight_server_ == "192.168.101.69:8104") {
                    inksight_server_ = INKSIGHT_DEFAULT_SERVER_URL;
                    InkSightClient::SaveServerUrl(inksight_server_);
                    ESP_LOGI(kTag, "Migrated XiaowuAI server to %s",
                             inksight_server_.c_str());
                }
                s.SetBool("server_mig_v1", true);
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
        // A 屏首屏策略：开机始终先渲染欢迎页作为兜底，保证屏幕永不空白。
        // 注意：这里不能用「inksight_server_ 是否为空」来判断是否已配置——
        // 上面紧邻的兜底分支会把空的 server 填成编译期默认域名
        // （INKSIGHT_DEFAULT_SERVER_URL，非空），所以 inksight_server_ 永远非空，
        // 那样写 ShowBootHome() 会变成死代码：新板子/未配网设备开机后 A 屏
        // 一片空白，只在进配网模式时闪一个提示框（实测 black_pixels=22029，
        // 而欢迎页应为 14363）。
        // 正确做法：先显示欢迎页；已绑定且后台配了双屏内容的设备，
        // RefreshDualScreens 成功后会用 RawFrameRefresh 把 A 屏覆盖成网络图
        // （大盘全景等）；未配置/未配网的设备就停在欢迎页引导去后台配置。
        display_->ShowBootHome();
        ESP_LOGI(kTag, "A 屏首屏显示欢迎页（已配置双屏内容时将被网络图覆盖）");
        display_->dashboard().SetInkSightShow(
            [this](bool initial_full) {
                Application::GetInstance().Schedule(
                    [this, initial_full]() { RefreshInkSight(initial_full); });
            });
        power_mode_ = LoadPowerMode();
        sleep_mode_pending_.store(power_mode_ == PowerMode::kIntermittent);
        ESP_LOGI(kTag, "电源模式: %s（KEY+ 长按已改为重新配对；电源模式请用 MCP self.epaper.set_power_mode）",
                 PowerModeName(power_mode_));
        InitializeButton();
        if (storage_.Mount()) {
            ESP_LOGI(kTag, "TF 卡挂载成功: %s", storage_.Status().c_str());
        } else {
            const std::string& me = storage_.MountError();
            ESP_LOGI(kTag, "TF 卡挂载失败: %s",
                     me.empty() ? "未插入 / SPI 初始化失败" : me.c_str());
        }
        InitializeDashboardTools();
        todo_web_ = std::make_unique<EpaperTodoWeb>([this](const char* kind) {
            Application::GetInstance().Schedule([this, kind = std::string(kind)]() {
                if (kind == "schedule") {
                    // 网页课程表保存：从 NVS 读取固件格式 JSON 并刷新课程表页
                    Settings settings("epd_dash", false);
                    display_->dashboard().SetSchedule(settings.GetString("schedule", ""));
                    display_->dashboard().Show(EpaperDashboard::Page::kSchedule);
                } else if (display_->dashboard().CurrentPageName() == "待办事项") {
                    display_->dashboard().Show(EpaperDashboard::Page::kTodo);
                }
            });
        });
    }

    // Application installs its UI/network callback after constructing the
    // board. Registering a board callback in the constructor would be silently
    // overwritten by Application::SetNetworkEventCallback(), so wrap the
    // application callback and append our post-connect services here.
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](
                NetworkEvent event, const std::string& data) {
                if (callback) callback(event, data);
                if (event == NetworkEvent::Disconnected) {
                    // StartConfigAp() 会先停 station 并广播 Disconnected，
                    // 之后才启动配网门户的 httpd。这里提前释放 8080 编辑器，
                    // 保证配网时只有一个 httpd 实例（联网后会自动重启）。
                    if (todo_web_ && todo_web_->IsRunning()) todo_web_->Stop();
                }
                if (event == NetworkEvent::Connected) {
                    Application::GetInstance().Schedule([this]() {
                        // 联网后自动选路：按办公点内网候选探测 InkSight 后端，
                        // 不再依赖手动配置（见 InkSightClient::ResolveServerUrl）。
                        inksight_server_ =
                            InkSightClient::ResolveServerUrl(GetNetwork());
                        ESP_LOGI(kTag, "XiaowuAI server resolved: %s",
                                 inksight_server_.c_str());
                        ApplyPowerMode();
                        // Overwrite any stale runtime mode left by an older
                        // board build, then let an interval wake refresh once
                        // before it reports asleep and enters deep sleep.
                        ReportRuntimeMode(true);
                        // TF 卡联网就绪后上报一次存储快照，供后台「本地 TF 卡」面板读取。
                        ReportTfStorage();
                        if (todo_web_ && !todo_web_->IsRunning()) {
                            todo_web_->Start();
                        }
#if EPD_ENABLE_COLOR_SCREEN
                        InitializeColorScreenAfterNetwork();
#endif
                        // 重新配对遗留的待解绑：长按 KEY+ 重新配对时网络可能
                        // 已经断了，解绑请求失败，这里联网成功后补一次，下一个
                        // 刷新周期就会拉到配对码图。
                        Settings rebind("inksight", /*read_write=*/true);
                        if (rebind.GetInt("rebind_pending", 0) != 0) {
                            std::string unbind_error;
                            if (InkSightClient::UnbindSelf(
                                    GetNetwork(), inksight_server_,
                                    SystemInfo::GetMacAddress(),
                                    &unbind_error)) {
                                rebind.SetInt("rebind_pending", 0);
                                ESP_LOGI(kTag, "重新配对：联网后已解绑，将显示配对码");
                            } else {
                                ESP_LOGW(kTag, "重新配对：解绑仍未成功(%s)",
                                         unbind_error.c_str());
                            }
                        }
                    });
                }
            });
    }

    AudioCodec* GetAudioCodec() override {
#if !CONFIG_SSD2683ZA_AUDIO_MAX98357_ICS43434
        static Es8311AudioCodec codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, false, AUDIO_CODEC_PA_VOLTAGE);
#else
        static NoAudioCodecSimplex codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_SPK_I2S_GPIO_BCLK, AUDIO_SPK_I2S_GPIO_WS,
            AUDIO_SPK_I2S_GPIO_DOUT, I2S_STD_SLOT_LEFT,
            AUDIO_MIC_I2S_GPIO_SCK, AUDIO_MIC_I2S_GPIO_WS,
            AUDIO_MIC_I2S_GPIO_DIN, I2S_STD_SLOT_LEFT);
#endif
        return &codec;
    }

    Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(Ssd2683zaS3Board);
