#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>

#include "display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    ESP_LOGW(TAG, "SetStatus: %s", status);
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::UpdateStatusBar(bool update_all) {
}


void Display::SetEmotion(const char* emotion) {
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::ShowActivationUi(const char* message, const char* code) {
    std::string content = message ? message : "";
    if (code && code[0] != '\0') {
        content += "\n\n激活码：";
        content += code;
    }
    SetStatus(Lang::Strings::ACTIVATION);
    SetChatMessage("system", content.c_str());
}

void Display::ShowProvisioningGuide(const char* ssid, const char* url) {
    std::string content = "设备未联网，请按下面步骤配网\n\n";
    content += "1 手机连接热点 ";
    content += (ssid && ssid[0] != '\0') ? ssid : "Xiaowu-XXXX";
    content += "\n2 浏览器打开 ";
    content += (url && url[0] != '\0') ? url : "192.168.4.1";
    content += "\n3 选择家庭 WiFi 并输入密码";
    content += "\n\n配网成功后屏幕显示 6 位激活码，\n打开 xiaozhi.me 输入即可绑定";
    ShowActivationUi(content.c_str(), "");
}

void Display::HideActivationUi() { SetChatMessage("system", ""); }

void Display::ClearChatMessages() {
    // Default empty implementation, override in subclasses if needed
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}
