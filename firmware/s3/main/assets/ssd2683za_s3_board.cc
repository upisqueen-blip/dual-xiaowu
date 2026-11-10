#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "inksight_client.h"
#include "epaper_todo_web.h"
#include "lightacade_client.h"
#include "mcp_server.h"
#include "sd_card_storage.h"
#include "settings.h"
#include "ssd1619_display.h"
#if EPD_ENABLE_COLOR_SCREEN
#include "uc1679_display.h"
#endif
#include "wifi_board.h"
#include "lvgl_image.h"
#include "system_info.h"

#ifndef INKSIGHT_DEFAULT_SERVER_URL
#  define INKSIGHT_DEFAULT_SERVER_URL ""
#endif

#include <esp_heap_caps.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>

namespace {
constexpr const char* kTag = "Ssd2683zaS3Board";
}

class Ssd2683zaS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Ssd1619Display* display_ = nullptr;
    SdCardStorage storage_;
    std::atomic<bool> recording_{false};

#if EPD_ENABLE_COLOR_SCREEN
    // 双排座第二屏（FPC2）：UC1679 4.2" 黑白红三色，InkSight 模式画面。
    // SSD1619（FPC1）保持对话信息屏 + 学习 dashboard（快刷）。
    Uc1679Display* color_screen_ = nullptr;
    uint8_t* color_bw_ = nullptr;   // 15000 字节，1=白
    uint8_t* color_red_ = nullptr;  // 15000 字节，1=红
    TaskHandle_t color_refresh_task_ = nullptr;
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
        ESP_LOGI(kTag, "InkSight persona cycled to %s", kPersonas[idx]);
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
    // 三色屏（UC1679）InkSight 拉取+刷写。拉 2bpp raw 分解为 BW/RED
    // 两个平面后全刷；快刷屏 kInkSight 页仅显示状态（模式/进度/错误）。
    void RefreshInkSightColor(bool initial_full_refresh) {
        if (color_bw_ == nullptr) {
            color_bw_ = static_cast<uint8_t*>(heap_caps_malloc(
                Uc1679Display::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            color_red_ = static_cast<uint8_t*>(heap_caps_malloc(
                Uc1679Display::kPlaneBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        if (color_bw_ == nullptr || color_red_ == nullptr) {
            display_->dashboard().SetInkSightFooter("内存不足");
            return;
        }
        if (inksight_busy_.exchange(true)) {
            ESP_LOGW(kTag, "InkSight color refresh already in progress, skip");
            return;
        }
        inksight_cancel_.store(false);
        auto cleanup = [this]() { inksight_busy_.store(false); };

        display_->dashboard().SetInkSightFooter("彩屏更新中…(约16秒)");
        Settings inksight_settings("inksight", /*read_write=*/false);
        std::string persona = inksight_settings.GetString("persona", "");
        if (persona.empty()) {
            persona = "WEATHER";
        } else if (persona == "auto" || persona == "AUTO") {
            persona.clear();
        }
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        std::string error;
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            if (inksight_cancel_.load()) {
                ESP_LOGI(kTag, "InkSight color refresh cancelled by user");
                SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                cleanup();
                return;
            }
            if (attempt > 1) {
                vTaskDelay(pdMS_TO_TICKS(500));
                display_->dashboard().SetInkSightFooter("重试中…");
            }
            ok = InkSightClient::Load2bpp(GetNetwork(), inksight_server_,
                                          persona, color_bw_, color_red_,
                                          error);
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
        display_->dashboard().SetInkSightFooter("正在刷彩屏，请稍候…");
        const bool refreshed =
            color_screen_->ShowFrame(color_bw_, color_red_);
        display_->dashboard().SetInkSightFooter(
            refreshed ? std::string("模式: ") +
                            (persona.empty() ? "auto" : persona) +
                            " · OK刷新 · 长按返回"
                      : "彩屏刷新失败 OK再试");
        ESP_LOGI(kTag, "InkSight color screen refreshed (%s)",
                 initial_full_refresh ? "initial" : "manual");
        cleanup();
    }

    // 三色屏定时自动刷新：每分钟醒来，累计到 NVS 配置的间隔（默认 30
    // 分钟）就拉一次新画面。对话页时顺延，避免语音中抢带宽。
    static void ColorRefreshTask(void* argument) {
        auto* self = static_cast<Ssd2683zaS3Board*>(argument);
        // 等系统就绪（WiFi/资产/首页渲染）后再首刷。
        vTaskDelay(pdMS_TO_TICKS(20000));
        Application::GetInstance().Schedule(
            [self]() { self->RefreshInkSight(/*initial_full=*/true); });
        int minutes_elapsed = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            Settings settings("inksight", /*read_write=*/false);
            const int interval_minutes =
                settings.GetInt("color_refresh_minutes", 30);
            if (interval_minutes <= 0) continue;  // 0 = 关闭自动刷新
            ++minutes_elapsed;
            if (minutes_elapsed < interval_minutes) continue;
            auto& dashboard = self->display_->dashboard();
            if (dashboard.IsConversation()) {
                // 对话中顺延 1 分钟，不累计清零
                continue;
            }
            minutes_elapsed = 0;
            Application::GetInstance().Schedule(
                [self]() { self->RefreshInkSight(false); });
        }
    }
#endif

    // 执行一次 InkSight 拉取+解码+刷屏幕（应用 Schedule 回调中调用）。
    // initial_full_refresh=true: 首次进入用全刷；false: 按 OK 快刷（局部）。
    void RefreshInkSight(bool initial_full_refresh) {
#if EPD_ENABLE_COLOR_SCREEN
        // 双屏模式：InkSight 模式画面归三色屏（UC1679），本函数转发；
        // 快刷屏的 kInkSight 页只显示状态与按键提示。
        if (color_screen_ != nullptr && color_screen_->Available()) {
            RefreshInkSightColor(initial_full_refresh);
            return;
        }
        // 三色屏未插（探测失败）时走下面的 1bpp 路径，画面降级显示在
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
            ESP_LOGW(kTag, "InkSight refresh already in progress, skip");
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
        if (persona.empty()) {
            persona = "WEATHER";  // 默认进入天气页
        } else if (persona == "auto" || persona == "AUTO") {
            persona.clear();
        }
        std::string error;
        // 空闲时 WiFi 处于 LOW_POWER(MAX_MODEM) 最大省电，突发 TCP 下载易被
        // AP 缓冲丢包掐断流（语音对话时框架会临时切 PERFORMANCE 所以没这问题）。
        // 下载期间临时切 PERFORMANCE，结束后恢复。
        SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            if (inksight_cancel_.load()) {
                ESP_LOGI(kTag, "InkSight refresh cancelled by user");
                SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                cleanup();
                return;
            }
            if (attempt > 1) {
                // 服务端渲染结果有缓存，重试通常命中缓存立即返回
                vTaskDelay(pdMS_TO_TICKS(500));
                display_->dashboard().SetInkSightFooter("重试中…");
            }
            ok = InkSightClient::Load(GetNetwork(), inksight_server_,
                                      persona, inksight_frame_, error);
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
        display_->RawFrameRefresh(inksight_frame_,
                                  !initial_full_refresh /* partial */);
        display_->dashboard().SetInkSightFooter("OK刷新 · 长按返回");
        ESP_LOGI(kTag, "InkSight refreshed (%s)",
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
            ESP_LOGW(kTag, "TF book scan empty: /sdcard/xiaozhi/books -> %s",
                     listing.c_str());
            std::string message = "没有找到 TXT 电子书\n\n请放入 /xiaozhi/books\n\n";
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

    // Loads a [[img:...]] illustration PNG from /xiaozhi/books. PNG only;
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
        // NS4150B CTRL is active-high. Keep it permanently enabled and do not
        // let esp_codec_dev toggle it as the microphone/output states change.
        ESP_ERROR_CHECK(gpio_reset_pin(AUDIO_CODEC_PA_CTRL_PIN));
        ESP_ERROR_CHECK(gpio_set_direction(AUDIO_CODEC_PA_CTRL_PIN,
                                           GPIO_MODE_INPUT_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(AUDIO_CODEC_PA_CTRL_PIN, 1));
        ESP_LOGI(kTag, "AUDIO PA: NS4150B enabled permanently: GPIO%d pad=%d",
                 AUDIO_CODEC_PA_CTRL_PIN,
                 gpio_get_level(AUDIO_CODEC_PA_CTRL_PIN));
    }

    void InitializeI2c() {
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
                ESP_LOGI(kTag, "KEY GPIO%d stable=%d", pins[i], value);
                if (value == 0) {
                    pressed_at[i] = now;
                } else {
                    const int64_t held = now - pressed_at[i];
                    if (i == 0) {
                        Application::GetInstance().Schedule(
                            [self]() { self->HandleKeyUp(); });
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
                        ESP_LOGI(kTag, "InkSight server saved: %s", url.c_str());
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
                        ESP_LOGI(kTag, "InkSight persona saved: %s", mode.c_str());
                        display_->dashboard().Show(EpaperDashboard::Page::kInkSight);
                        return true;
                    });
#if EPD_ENABLE_COLOR_SCREEN
        mcp.AddTool("self.epaper.refresh_color_screen",
                    "立即刷新三色屏（UC1679，InkSight 模式画面，全刷约 16 秒）。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        display_->dashboard().Show(EpaperDashboard::Page::kInkSight);
                        Application::GetInstance().Schedule(
                            [this]() { RefreshInkSight(false); });
                        return true;
                    });
        mcp.AddTool("self.epaper.set_color_refresh_minutes",
                    "设置三色屏自动刷新间隔（分钟，1~1440；0=关闭自动刷新）。",
                    PropertyList({Property("minutes", kPropertyTypeInteger, 0, 1440)}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const int minutes =
                            properties["minutes"].value<int>();
                        Settings s("inksight", /*read_write=*/true);
                        s.SetInt("color_refresh_minutes", minutes);
                        ESP_LOGI(kTag, "Color screen refresh interval: %d min",
                                 minutes);
                        return true;
                    });
        mcp.AddTool("self.epaper.screen_status",
                    "查看双屏配置与状态（快刷屏/三色屏在座情况、InkSight 服务器与模式）。",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        std::string status = "bw_screen(fast):ok";
#if EPD_ENABLE_COLOR_SCREEN
                        status += color_screen_ != nullptr &&
                                          color_screen_->Available()
                                      ? "; color_screen(UC1679):ok"
                                      : "; color_screen(UC1679):not_detected";
#else
                        status += "; color_screen:disabled";
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
        mcp.AddTool("self.epaper.show_sd_image", "显示 TF 卡 /xiaozhi/images 中的 PNG/JPEG 图片。",
                    PropertyList({Property("name", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::vector<uint8_t> bytes;
                        std::string error;
                        if (!storage_.Load("images", p["name"].value<std::string>(), bytes,
                                           6 * 1024 * 1024, error)) return error;
                        void* data = heap_caps_malloc(bytes.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!data) data = heap_caps_malloc(bytes.size(), MALLOC_CAP_8BIT);
                        if (!data) return std::string("image memory allocation failed");
                        memcpy(data, bytes.data(), bytes.size());
                        try {
                            display_->SetPreviewImage(std::make_unique<LvglAllocatedImage>(data, bytes.size()));
                        } catch (...) {
                            heap_caps_free(data);
                            return std::string("unsupported or damaged image");
                        }
                        return true;
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
    Ssd2683zaS3Board() {
        InitializePowerAmplifier();
        InitializeI2c();
        display_ = new Ssd1619Display();
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
        if (inksight_server_ == "http://nas.insane.vip:8090" ||
            inksight_server_ == "nas.insane.vip:8090" ||
            inksight_server_ == "http://nas.insane.vip:8080" ||
            inksight_server_ == "nas.insane.vip:8080") {

            inksight_server_ = INKSIGHT_DEFAULT_SERVER_URL;
            InkSightClient::SaveServerUrl(inksight_server_);
            ESP_LOGI(kTag, "Migrated InkSight server to %s",
                     inksight_server_.c_str());
        }
        if (inksight_server_.empty()) {
            inksight_server_ = INKSIGHT_DEFAULT_SERVER_URL;
            if (inksight_server_.empty()) {
                ESP_LOGW(kTag, "InkSight server not set — use MCP "
                               "self.epaper.set_inksight_server to configure, "
                               "e.g. https://dual.xiaowuai.vip/backend");
            } else {
                ESP_LOGI(kTag, "InkSight server from compile-time default: %s",
                         inksight_server_.c_str());
            }
        } else {
            ESP_LOGI(kTag, "InkSight server (NVS): %s", inksight_server_.c_str());
        }
        display_->dashboard().SetInkSightShow(
            [this](bool initial_full) {
                Application::GetInstance().Schedule(
                    [this, initial_full]() { RefreshInkSight(initial_full); });
            });
#if EPD_ENABLE_COLOR_SCREEN
        // 双排座第二屏（FPC2）：构造后探测，不在座则自动降级为单屏模式。
        color_screen_ = new Uc1679Display([this]() {
            return display_ != nullptr && display_->IsPanelBusy();
        });
        color_screen_->Init();
        if (xTaskCreate(ColorRefreshTask, "color_refresh", 4096, this, 3,
                        &color_refresh_task_) != pdPASS) {
            ESP_LOGW(kTag, "Cannot create color refresh task");
        }
#endif
        InitializeButton();
        storage_.Mount();  // Card absence must never prevent normal voice/display startup.
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
        // httpd_start() 需要 TCPIP 已就绪，不能在构造函数里调。
        // 等 WiFi 连上后再启动。
        SetNetworkEventCallback([this](NetworkEvent event, const std::string&) {
            if (event == NetworkEvent::Connected && todo_web_ && !todo_web_->IsRunning()) {
                Application::GetInstance().Schedule([this]() {
                    if (!todo_web_->IsRunning()) {
                        todo_web_->Start();
                    }
                });
            }
        });
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, false, AUDIO_CODEC_PA_VOLTAGE);
        return &codec;
    }

    Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(Ssd2683zaS3Board);
