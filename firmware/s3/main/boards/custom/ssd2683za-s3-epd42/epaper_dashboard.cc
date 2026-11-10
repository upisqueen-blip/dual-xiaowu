#include "epaper_dashboard.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>

#include "esp_lvgl_port.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <font_awesome.h>
#include <cJSON.h>
#include "settings.h"
#include "welcome_png.h"
#include "provisioning_png.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

namespace {
constexpr const char* kDashboardTag = "EpaperDashboard";

struct TextPage {
    std::string text;
};

// At 20 px the body holds about 18 Han characters (or 36 Latin characters)
// on each of eight lines. Paginating here prevents LVGL from clipping a long
// answer below the bottom edge of the 400x300 panel.
std::vector<TextPage> PaginateConversation(const std::string& text) {
    constexpr int kLineUnits = 36;
    constexpr int kLinesPerPage = 8;
    std::vector<TextPage> pages(1);
    int column = 0;
    int line = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = lead < 0x80 ? 1 : ((lead & 0xe0) == 0xc0 ? 2 :
                        ((lead & 0xf0) == 0xe0 ? 3 : 4));
        if (i + length > text.size()) length = 1;
        const bool newline = text[i] == '\n';
        const int units = newline ? 0 : (length == 1 ? 1 : 2);
        if (newline || column + units > kLineUnits) {
            ++line;
            column = 0;
            if (line >= kLinesPerPage) {
                pages.push_back({});
                line = 0;
            }
        }
        if (!newline) column += units;
        pages.back().text.append(text, i, length);
        i += length;
    }
    return pages;
}

// Recognise both "[[img:名称.png]]" and Markdown "![说明](名称.png)" inline
// image markers.  Returns the PNG file name (path component stripped, images
// live next to the book in /xiaozhi/books) and the position after the marker.
bool ParseImageMarker(const std::string& text, size_t i, std::string& name,
                      size_t& next) {
    const std::string png = ".png";
    auto finish = [&](size_t raw_start, size_t raw_end) {
        std::string candidate = text.substr(raw_start, raw_end - raw_start);
        const size_t slash = candidate.find_last_of("/\\");
        if (slash != std::string::npos) candidate = candidate.substr(slash + 1);
        if (candidate.size() < png.size()) return false;
        std::string lower = candidate;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (lower.compare(lower.size() - png.size(), png.size(), png) != 0)
            return false;
        name = candidate;
        next = raw_end;
        if (next < text.size() && text[next] == '\n') ++next;
        return true;
    };
    if (text.compare(i, 6, "[[img:") == 0) {
        const size_t end = text.find("]]", i + 6);
        if (end != std::string::npos && end + 2 <= text.size()) {
            return finish(i + 6, end);
        }
    }
    if (text.compare(i, 2, "![") == 0) {
        const size_t alt_end = text.find("](", i + 2);
        if (alt_end != std::string::npos) {
            const size_t end = text.find(')', alt_end + 2);
            if (end != std::string::npos) {
                return finish(alt_end + 2, end);
            }
        }
    }
    return false;
}

std::tm LocalTime() {
    const std::time_t now = std::time(nullptr);
    std::tm value = {};
    localtime_r(&now, &value);
    return value;
}

// 用户中心 · 口语陪练设置项。取值与 LightAcade Partner API 附录 A 对齐，
// 存储于 NVS namespace "lightacade"，陪练启动时读取（见 ssd2683za_s3_board）。
struct ProfileOption { const char* code; const char* name; };
constexpr ProfileOption kProfileLanguages[] = {
    {"en", "英语"}, {"ja", "日语"}, {"fr", "法语"}, {"de", "德语"},
    {"es", "西语"}, {"pt", "葡语"}, {"fi", "芬兰语"}, {"ar", "阿拉伯语"},
};
constexpr ProfileOption kProfileGrades[] = {
    {"primary_lower", "小学低年级"}, {"primary_upper", "小学高年级"},
    {"junior", "初中"}, {"senior", "高中"},
};
constexpr ProfileOption kProfileVoices[] = {
    {"Puck", "小阳"}, {"Aoede", "阿依"}, {"Leda", "小满"},
    {"Kore", "可蕊"}, {"Charon", "夏老师"}, {"Zephyr", "泽风"},
};

template <size_t N>
const char* ProfileName(const ProfileOption (&opts)[N], const std::string& code) {
    for (const auto& o : opts) {
        if (code == o.code) return o.name;
    }
    return opts[0].name;
}

// 当前字段的下一个可选值 code（循环）。field: 0=语种 1=学段 2=音色。
template <size_t N>
std::string ProfileNextCode(const ProfileOption (&opts)[N],
                            const std::string& current) {
    size_t idx = 0;
    for (size_t i = 0; i < N; ++i) {
        if (current == opts[i].code) { idx = i; break; }
    }
    return opts[(idx + 1) % N].code;
}

// 等宽字体的显示宽度：ASCII 1，CJK/3 字节 UTF-8 算 2。
int DisplayWidth(const std::string& s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { w += 1; i += 1; }
        else if (c < 0xE0) { w += 1; i += 2; }
        else { w += 2; i += 3; }
    }
    return w;
}

// 用空格填充到指定显示宽度（用于弹窗内多列对齐）。
std::string PadToWidth(const std::string& s, int width) {
    std::string result = s;
    int current = DisplayWidth(s);
    while (current < width) { result += ' '; ++current; }
    return result;
}

// 当前字段配置：选项数组、项数、弹窗列数、弹窗标题、NVS 键名、默认值。
struct ProfileFieldInfo {
    const ProfileOption* options;
    size_t count;
    int columns;
    const char* popup_title;
    const char* nvs_key;
    const char* default_value;
};
ProfileFieldInfo GetProfileFieldInfo(int field) {
    switch (field) {
        case 0: return {kProfileLanguages, std::size(kProfileLanguages), 4,
                        "语种选择", "language", "en"};
        case 1: return {kProfileGrades, std::size(kProfileGrades), 2,
                        "学段选择", "grade_band", "junior"};
        default: return {kProfileVoices, std::size(kProfileVoices), 3,
                         "音色选择", "voice", "Puck"};
    }
}

// 默认课程表：成都七中初三16班 2026-2027 学年上期（与网页 /schedule 内置默认
// 一致）。semester_start=2026-09-01 本地零点的 unix 秒。家长可通过网页
// http://<设备IP>:8080/schedule 修改，保存后以此默认值不再生效。
constexpr char kDefaultSchedule[] =
    "{\"semester_start\":1788192000,\"courses\":["
    "{\"day\":1,\"start\":\"08:00\",\"end\":\"08:40\",\"name\":\"升旗仪式\",\"room\":\"\"},"
    "{\"day\":1,\"start\":\"08:50\",\"end\":\"09:30\",\"name\":\"数学\",\"room\":\"戴蔚华\"},"
    "{\"day\":1,\"start\":\"09:45\",\"end\":\"10:25\",\"name\":\"英语\",\"room\":\"胡敏\"},"
    "{\"day\":1,\"start\":\"11:00\",\"end\":\"11:35\",\"name\":\"语文\",\"room\":\"李文巧\"},"
    "{\"day\":1,\"start\":\"11:50\",\"end\":\"12:25\",\"name\":\"体育\",\"room\":\"熊伟\"},"
    "{\"day\":1,\"start\":\"14:10\",\"end\":\"14:50\",\"name\":\"化学\",\"room\":\"刘佳璐\"},"
    "{\"day\":1,\"start\":\"15:05\",\"end\":\"15:45\",\"name\":\"物理\",\"room\":\"何薇帆\"},"
    "{\"day\":1,\"start\":\"16:00\",\"end\":\"16:40\",\"name\":\"班会/生态安全\",\"room\":\"何薇帆\"},"
    "{\"day\":1,\"start\":\"16:55\",\"end\":\"17:35\",\"name\":\"英自\",\"room\":\"胡敏\"},"
    "{\"day\":1,\"start\":\"17:50\",\"end\":\"18:30\",\"name\":\"语自\",\"room\":\"李文巧\"},"
    "{\"day\":2,\"start\":\"08:00\",\"end\":\"08:40\",\"name\":\"物理\",\"room\":\"何薇帆\"},"
    "{\"day\":2,\"start\":\"08:50\",\"end\":\"09:30\",\"name\":\"语文\",\"room\":\"李文巧\"},"
    "{\"day\":2,\"start\":\"09:45\",\"end\":\"10:25\",\"name\":\"体育\",\"room\":\"熊伟\"},"
    "{\"day\":2,\"start\":\"11:00\",\"end\":\"11:35\",\"name\":\"数学\",\"room\":\"戴蔚华\"},"
    "{\"day\":2,\"start\":\"11:50\",\"end\":\"12:25\",\"name\":\"数学\",\"room\":\"戴蔚华\"},"
    "{\"day\":2,\"start\":\"14:10\",\"end\":\"14:50\",\"name\":\"化自\",\"room\":\"刘佳璐\"},"
    "{\"day\":2,\"start\":\"15:05\",\"end\":\"15:45\",\"name\":\"英语\",\"room\":\"胡敏\"},"
    "{\"day\":2,\"start\":\"16:00\",\"end\":\"16:40\",\"name\":\"自习\",\"room\":\"\"},"
    "{\"day\":2,\"start\":\"16:55\",\"end\":\"17:35\",\"name\":\"自习\",\"room\":\"\"},"
    "{\"day\":2,\"start\":\"17:50\",\"end\":\"18:30\",\"name\":\"历史\",\"room\":\"吴倩\"},"
    "{\"day\":3,\"start\":\"08:00\",\"end\":\"08:40\",\"name\":\"数学\",\"room\":\"戴蔚华\"},"
    "{\"day\":3,\"start\":\"08:50\",\"end\":\"09:30\",\"name\":\"语文\",\"room\":\"李文巧\"},"
    "{\"day\":3,\"start\":\"09:45\",\"end\":\"10:25\",\"name\":\"英语\",\"room\":\"胡敏\"},"
    "{\"day\":3,\"start\":\"11:00\",\"end\":\"11:35\",\"name\":\"化学\",\"room\":\"刘佳璐\"},"
    "{\"day\":3,\"start\":\"11:50\",\"end\":\"12:25\",\"name\":\"历史\",\"room\":\"吴倩\"},"
    "{\"day\":3,\"start\":\"14:10\",\"end\":\"14:50\",\"name\":\"语自\",\"room\":\"李文巧\"},"
    "{\"day\":3,\"start\":\"15:05\",\"end\":\"15:45\",\"name\":\"体育\",\"room\":\"熊伟\"},"
    "{\"day\":3,\"start\":\"16:00\",\"end\":\"16:40\",\"name\":\"物自\",\"room\":\"何薇帆\"},"
    "{\"day\":3,\"start\":\"16:55\",\"end\":\"17:35\",\"name\":\"数自\",\"room\":\"戴蔚华\"},"
    "{\"day\":3,\"start\":\"17:50\",\"end\":\"18:30\",\"name\":\"道德与法治\",\"room\":\"肖芳\"},"
    "{\"day\":4,\"start\":\"08:00\",\"end\":\"08:40\",\"name\":\"英语\",\"room\":\"胡敏\"},"
    "{\"day\":4,\"start\":\"08:50\",\"end\":\"09:30\",\"name\":\"英语\",\"room\":\"胡敏\"},"
    "{\"day\":4,\"start\":\"09:45\",\"end\":\"10:25\",\"name\":\"物理\",\"room\":\"何薇帆\"},"
    "{\"day\":4,\"start\":\"11:00\",\"end\":\"11:35\",\"name\":\"数学\",\"room\":\"戴蔚华\"},"
    "{\"day\":4,\"start\":\"11:50\",\"end\":\"12:25\",\"name\":\"化学\",\"room\":\"刘佳璐\"},"
    "{\"day\":4,\"start\":\"14:10\",\"end\":\"14:50\",\"name\":\"语文\",\"room\":\"李文巧\"},"
    "{\"day\":4,\"start\":\"15:05\",\"end\":\"15:45\",\"name\":\"道德与法治\",\"room\":\"肖芳\"},"
    "{\"day\":4,\"start\":\"16:00\",\"end\":\"16:40\",\"name\":\"体育\",\"room\":\"熊伟\"},"
    "{\"day\":4,\"start\":\"16:55\",\"end\":\"17:35\",\"name\":\"年级测试\",\"room\":\"\"},"
    "{\"day\":4,\"start\":\"17:50\",\"end\":\"18:30\",\"name\":\"年级测试\",\"room\":\"\"},"
    "{\"day\":5,\"start\":\"08:00\",\"end\":\"08:40\",\"name\":\"语文\",\"room\":\"李文巧\"},"
    "{\"day\":5,\"start\":\"08:50\",\"end\":\"09:30\",\"name\":\"语文\",\"room\":\"李文巧\"},"
    "{\"day\":5,\"start\":\"09:45\",\"end\":\"10:25\",\"name\":\"英语\",\"room\":\"胡敏\"},"
    "{\"day\":5,\"start\":\"11:00\",\"end\":\"11:35\",\"name\":\"物理\",\"room\":\"何薇帆\"},"
    "{\"day\":5,\"start\":\"11:50\",\"end\":\"12:25\",\"name\":\"数学\",\"room\":\"戴蔚华\"},"
    "{\"day\":5,\"start\":\"14:10\",\"end\":\"14:50\",\"name\":\"体育\",\"room\":\"熊伟\"},"
    "{\"day\":5,\"start\":\"15:05\",\"end\":\"15:45\",\"name\":\"英自\",\"room\":\"胡敏\"},"
    "{\"day\":5,\"start\":\"16:00\",\"end\":\"16:40\",\"name\":\"化学\",\"room\":\"刘佳璐\"},"
    "{\"day\":5,\"start\":\"16:55\",\"end\":\"17:35\",\"name\":\"物自\",\"room\":\"何薇帆\"},"
    "{\"day\":5,\"start\":\"17:50\",\"end\":\"18:30\",\"name\":\"数自\",\"room\":\"戴蔚华\"}]}";
}  // namespace

EpaperDashboard::EpaperDashboard() = default;

EpaperDashboard::~EpaperDashboard() {
    if (overlay_ && lvgl_port_lock(1000)) {
        lv_obj_delete(overlay_);
        lvgl_port_unlock();
    }
}

const char* EpaperDashboard::PageName(Page page) {
    switch (page) {
        case Page::kHome: return "首页";
        case Page::kConversation: return "小悟对话";
        case Page::kSpeaking: return "口语陪练";
        case Page::kReading: return "阅读";
        case Page::kWords: return "单词";
        case Page::kSchedule: return "课程表";
        case Page::kCalendar: return "日历";
        case Page::kBookReader: return "阅读";
        case Page::kNotes: return "便签";
        case Page::kTodo: return "待办事项";
        case Page::kQuota: return "Codex 额度";
        case Page::kInkSight: return "今日看板";
        case Page::kAnalysis: return "学情分析";
        case Page::kProfile: return "用户中心";
        case Page::kActivation: return "设备配网与激活";
        case Page::kProvisioning: return "配网指引";
        case Page::kWelcome: return "欢迎";
        default: return "首页";
    }
}

std::string EpaperDashboard::CurrentPageName() const { return PageName(page_); }

void EpaperDashboard::EnsureUi() {
    if (overlay_) return;
    overlay_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(overlay_, 4, 0);

    title_ = lv_label_create(overlay_);
    lv_obj_set_width(title_, LV_PCT(60));
    lv_obj_set_style_text_color(title_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(title_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_border_width(title_, 0, 0);
    lv_obj_align(title_, LV_ALIGN_TOP_LEFT, 2, -2);

    // 顶部状态栏分割线：title_ 下方 1px 横线，连接整个屏幕宽度。
    // 只在沉浸式页面（陪练/看板）时显示，普通页面 title_ 居中不画。
    status_line_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(status_line_);
    lv_obj_set_size(status_line_, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(status_line_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status_line_, LV_OPA_COVER, 0);
    lv_obj_align(status_line_, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_add_flag(status_line_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏，Render() 里按页面显隐

    // 顶部状态栏右侧：WiFi 信号图标 + USB/电量指示。
    // 只在陪练/对话页面等需要沉浸式显示的页面显示，其余页面隐藏。
    status_label_ = lv_label_create(overlay_);
    lv_obj_set_width(status_label_, LV_PCT(38));
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_ICON_FONT, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, -2, -2);
    lv_label_set_text(status_label_, "");

    body_ = lv_label_create(overlay_);
    lv_obj_set_size(body_, LV_PCT(100), LV_PCT(72));
    lv_label_set_long_mode(body_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_line_space(body_, 5, 0);
    lv_obj_set_style_text_font(body_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_align(body_, LV_ALIGN_TOP_LEFT, 2, 24);

    activation_code_label_ = lv_label_create(overlay_);
    lv_obj_set_width(activation_code_label_, LV_PCT(88));
    lv_obj_set_style_bg_color(activation_code_label_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(activation_code_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(activation_code_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(activation_code_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(activation_code_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_pad_all(activation_code_label_, 10, 0);
    lv_obj_set_style_radius(activation_code_label_, 4, 0);
    lv_obj_align(activation_code_label_, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_add_flag(activation_code_label_, LV_OBJ_FLAG_HIDDEN);

    menu_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(menu_);
    lv_obj_set_size(menu_, LV_PCT(100), 170);
    lv_obj_align(menu_, LV_ALIGN_TOP_MID, 0, 21);
    static constexpr const char* icons[] = {
        FONT_AWESOME_MICROPHONE,
        FONT_AWESOME_GLASSES,
        FONT_AWESOME_PEN_TO_SQUARE,
        FONT_AWESOME_CALCULATOR,
        FONT_AWESOME_MAGNIFYING_GLASS,
        FONT_AWESOME_USER,
    };
    static constexpr const char* labels[] = {
        "口语陪练", "阅读", "单词", "课程表", "学情分析", "用户中心"
    };
    for (int i = 0; i < kMenuItemCount; ++i) {
        cards_[i] = lv_obj_create(menu_);
        lv_obj_remove_style_all(cards_[i]);
        lv_obj_set_size(cards_[i], 94, 78);
        lv_obj_set_pos(cards_[i], (i % 4) * 98, (i / 4) * 84);
        lv_obj_set_style_border_width(cards_[i], 1, 0);
        lv_obj_set_style_border_color(cards_[i], lv_color_black(), 0);
        lv_obj_set_style_radius(cards_[i], 3, 0);
        card_icons_[i] = lv_label_create(cards_[i]);
        lv_label_set_text(card_icons_[i], icons[i]);
        lv_obj_set_style_text_font(card_icons_[i], &BUILTIN_ICON_FONT, 0);
        lv_obj_align(card_icons_[i], LV_ALIGN_TOP_MID, 0, 7);
        card_labels_[i] = lv_label_create(cards_[i]);
        lv_label_set_text(card_labels_[i], labels[i]);
        lv_obj_set_width(card_labels_[i], 90);
        lv_label_set_long_mode(card_labels_[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(card_labels_[i], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_align(card_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(card_labels_[i], LV_ALIGN_BOTTOM_MID, 0, -7);
    }

    footer_ = lv_label_create(overlay_);
    lv_obj_set_width(footer_, LV_PCT(100));
    lv_obj_set_style_text_color(footer_, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_align(footer_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Full-screen illustration canvas for [[img:...]] book pages.
    book_image_ = lv_image_create(overlay_);
    lv_obj_add_flag(book_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(book_image_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(book_image_, LV_OPA_COVER, 0);

    // 欢迎页全屏烘焙图（logo + 欢迎语 + 售后群），复用 PNG 解码通道。
    welcome_image_ = lv_image_create(overlay_);
    lv_obj_add_flag(welcome_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(welcome_image_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(welcome_image_, LV_OPA_COVER, 0);
}

void EpaperDashboard::Show(Page page) {
    // A selection, page turn, or transcript update on the current page can
    // use the panel's partial waveform.  Ordinary page transitions also use
    // the fast partial waveform because a full waveform visibly blinks 2-3
    // times; every kTransitionsPerFull-th transition falls back to a full
    // refresh to clear accumulated ghosting.
    // Three transitions always get the cleaning full waveform: opening a
    // book (the reading page must not inherit the menu/list ghosts visible
    // in the photo), returning to the home menu (the inverted selection
    // card leaves strong ghosts) and entering the conversation page from
    // another page (e.g. wake word while reading).
    const bool same_page = overlay_ != nullptr && page_ == page;
    bool partial_refresh = same_page;
    if (!same_page && overlay_ != nullptr) {
        const bool always_full =
            page == Page::kHome || page == Page::kBookReader ||
            page == Page::kConversation || page == Page::kInkSight ||
            page == Page::kSpeaking || page == Page::kActivation ||
            page == Page::kWelcome || page == Page::kProvisioning ||
            transitions_since_full_ >= kTransitionsPerFull;
        if (always_full) transitions_since_full_ = 0;
        else ++transitions_since_full_;
        partial_refresh = !always_full;
    }
    const bool is_inksight = page == Page::kInkSight;
    if (page != page_ && page == Page::kSpeaking) {
        // 进入陪练页时清空上次残留状态，显示居中欢迎语。
        speaking_status_.clear();
    }
    page_ = page;
    visible_ = true;
    hidden_for_voice_ = false;
    if (!lvgl_port_lock(3000)) return;
    EnsureUi();
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    Render();
    // One user action can trigger several Show calls (e.g. a state change
    // right after a key press).  When the composed page is byte-identical to
    // what is already on the panel, skip the physical refresh entirely.
    std::string key = PageName(page_);
    key += '|';
    key += lv_label_get_text(title_);
    key += '|';
    key += lv_label_get_text(body_);
    key += '|';
    if (activation_code_label_) key += lv_label_get_text(activation_code_label_);
    key += '|';
    key += std::to_string(selected_item_);
    key += '|';
    key += lv_obj_has_flag(book_image_, LV_OBJ_FLAG_HIDDEN) ? "0" : "1";
    if (key == last_render_key_) {
        lvgl_port_unlock();
        return;
    }
    last_render_key_ = key;
    // This display is registered manually rather than through an esp_lcd
    // panel driver. Force the complete first dashboard frame now; relying on
    // the periodic invalid-area scan can leave the controller test pattern
    // on screen when only small label/card regions are considered dirty.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(lv_display_get_default());
    lvgl_port_unlock();
    // Physical e-paper refresh is explicitly committed once per page event.
    // LVGL flushes only update the RAM framebuffer.
    // For InkSight, the Board takes full control of the framebuffer and the
    // physical refresh (so we don't waste time drawing a LVGL frame over the
    // network-loaded image).
    if (is_inksight) {
        if (inksight_show_) {
            inksight_show_(!partial_refresh /* true on the first jump */);
        }
    } else if (refresh_callback_) {
        refresh_callback_(partial_refresh, false);
    }
}

void EpaperDashboard::Show(const std::string& name) {
    if (name == "speaking" || name == "english" || name == "英语陪练" || name == "口语陪练") Show(Page::kSpeaking);
    else if (name == "reading" || name == "阅读") Show(Page::kReading);
    else if (name == "words" || name == "单词") Show(Page::kWords);
    else if (name == "schedule" || name == "timetable" || name == "课程表") Show(Page::kSchedule);
    else if (name == "calendar" || name == "日历") Show(Page::kCalendar);
    else if (name == "notes" || name == "便签") Show(Page::kNotes);
    else if (name == "todo" || name == "todos" || name == "待办" ||
             name == "待办事项") Show(Page::kTodo);
    else if (name == "quota" || name == "额度" || name == "ai" ||
             name == "AI副屏") Show(Page::kQuota);
    else if (name == "analysis" || name == "学情" ||
             name == "学情分析") Show(Page::kAnalysis);
    else if (name == "profile" || name == "user" || name == "用户中心" ||
             name == "个人中心" || name == "口语设置") Show(Page::kProfile);
    else if (name == "inksight" || name == "ink" || name == "看板" ||
             name == "今日看板") Show(Page::kInkSight);
    else if (name == "welcome" || name == "欢迎") Show(Page::kWelcome);
    else Show(Page::kHome);
}

void EpaperDashboard::ShowActivation(std::string message, std::string code) {
    activation_message_ = std::move(message);
    activation_code_ = std::move(code);
    Show(Page::kActivation);
}

void EpaperDashboard::HideActivation() {
    if (page_ == Page::kActivation) Show(Page::kHome);
    // 配网指引页在配网完成后必须退出，否则会一直停在"请连接热点"，
    // 用户会以为没配上。回到欢迎页作为默认首屏（随后若需要激活，
    // 服务器下发的激活码页会覆盖它；已绑定则停在欢迎页等后台配内容）。
    else if (page_ == Page::kProvisioning) Show(Page::kWelcome);
}

void EpaperDashboard::ShowProvisioning(std::string ssid, std::string url) {
    provisioning_ssid_ = std::move(ssid);
    provisioning_url_ = std::move(url);
    Show(Page::kProvisioning);
}

void EpaperDashboard::Next() {
    // 欢迎页是根页面，没有可选项，方向键不跳转。
    if (page_ == Page::kWelcome) return;
    selected_item_ = (selected_item_ + 1) % kMenuItemCount;
    ESP_LOGI(kDashboardTag, "Menu selection: %d/%d", selected_item_ + 1,
             kMenuItemCount);
    Show(Page::kHome);
}

void EpaperDashboard::Previous() {
    // 欢迎页是根页面，没有可选项，方向键不跳转。
    if (page_ == Page::kWelcome) return;
    selected_item_ = (selected_item_ + kMenuItemCount - 1) % kMenuItemCount;
    ESP_LOGI(kDashboardTag, "Menu selection: %d/%d", selected_item_ + 1,
             kMenuItemCount);
    Show(Page::kHome);
}

void EpaperDashboard::Enter() {
    static constexpr Page pages[] = {Page::kSpeaking, Page::kReading, Page::kWords,
                                     Page::kSchedule, Page::kAnalysis,
                                     Page::kProfile};
    // 欢迎页按下 OK 进入功能菜单（口语陪练/阅读/…）。
    if (page_ == Page::kWelcome) {
        Show(Page::kHome);
        return;
    }
    if (page_ == Page::kHome) Show(pages[selected_item_]);
}

void EpaperDashboard::Back() {
    // 用户中心弹窗打开时，长按 OK = 放弃修改并关闭弹窗（不回首页）
    if (page_ == Page::kProfile && profile_popup_) {
        profile_popup_ = false;
        Show(Page::kProfile);
        return;
    }
    // 功能菜单返回欢迎页（根页面）。
    if (page_ == Page::kHome) {
        Show(Page::kWelcome);
        return;
    }
    // 欢迎页是根页面，返回键无操作。
    if (page_ == Page::kWelcome) return;
    Show(Page::kHome);
}

void EpaperDashboard::OpenBook(const std::string& title,
                               const std::string& utf8_text) {
    book_title_ = title;
    book_pages_.clear();
    book_page_ = 0;
    loaded_book_image_.clear();
    book_image_data_.reset();
    PsramString page;
    size_t characters = 0;
    for (size_t i = 0; i < utf8_text.size();) {
        // "[[img:名称.png]]" or Markdown "![说明](名称.png)" becomes a
        // full-screen image page; the PNG file is loaded from /xiaozhi/books
        // when the page is shown.
        std::string image_name;
        size_t after_marker = 0;
        if (ParseImageMarker(utf8_text, i, image_name, after_marker)) {
            if (characters > 0) {
                book_pages_.push_back({std::move(page), {}});
                page.clear();
                characters = 0;
            }
            book_pages_.push_back(
                {{}, PsramString(image_name.data(), image_name.size())});
            i = after_marker;
            continue;
        }
        const unsigned char lead = static_cast<unsigned char>(utf8_text[i]);
        const size_t length = lead < 0x80 ? 1 : ((lead & 0xe0) == 0xc0 ? 2 :
                              ((lead & 0xf0) == 0xe0 ? 3 : 4));
        if (i + length > utf8_text.size()) break;
        page.append(utf8_text.data() + i, length);
        i += length;
        if (++characters >= 160) {
            book_pages_.push_back({std::move(page), {}});
            page.clear();
            characters = 0;
        }
    }
    if (!page.empty()) book_pages_.push_back({std::move(page), {}});
    if (book_pages_.empty())
        book_pages_.push_back({PsramString("电子书内容为空"), {}});
    Show(Page::kBookReader);
}

void EpaperDashboard::ShowBookList(const std::vector<std::string>& books) {
    book_names_ = books;
    book_list_item_ = 0;
    Show(Page::kReading);
}

void EpaperDashboard::BookListNext() {
    if (!IsBookList() || book_names_.empty()) return;
    book_list_item_ = (book_list_item_ + 1) % book_names_.size();
    ESP_LOGI(kDashboardTag, "Book selection: %u/%u %s",
             static_cast<unsigned>(book_list_item_ + 1),
             static_cast<unsigned>(book_names_.size()),
             book_names_[book_list_item_].c_str());
    Show(Page::kReading);
}

void EpaperDashboard::BookListPrevious() {
    if (!IsBookList() || book_names_.empty()) return;
    book_list_item_ = (book_list_item_ + book_names_.size() - 1) %
                      book_names_.size();
    ESP_LOGI(kDashboardTag, "Book selection: %u/%u %s",
             static_cast<unsigned>(book_list_item_ + 1),
             static_cast<unsigned>(book_names_.size()),
             book_names_[book_list_item_].c_str());
    Show(Page::kReading);
}

std::string EpaperDashboard::SelectedBook() const {
    return book_names_.empty() ? std::string() : book_names_[book_list_item_];
}

void EpaperDashboard::ShowBookError(const std::string& message) {
    book_title_ = "电子书";
    book_pages_ = {{PsramString(message.data(), message.size()), {}}};
    book_page_ = 0;
    Show(Page::kBookReader);
}

void EpaperDashboard::SetBookImageLoader(
    std::function<std::unique_ptr<LvglImage>(const std::string&)> loader) {
    book_image_loader_ = std::move(loader);
}

bool EpaperDashboard::CurrentBookPageIsImage() const {
    return IsBookOpen() && !book_pages_[book_page_].image.empty();
}

void EpaperDashboard::EnsureBookImageLoaded(const std::string& name) {
    if (loaded_book_image_ == name) return;
    book_image_data_.reset();
    loaded_book_image_ = name;
    if (book_image_loader_) book_image_data_ = book_image_loader_(name);
}

void EpaperDashboard::EnsureWelcomeImageLoaded() {
    if (welcome_image_data_ != nullptr) return;
    // 编译期烘焙的欢迎页 PNG（welcome_png.h），经 LVGL PNG 解码通道渲染。
    try {
        welcome_image_data_ = std::make_unique<LvglAllocatedImage>(
            const_cast<uint8_t*>(welcome_png), welcome_png_len);
    } catch (...) {
        welcome_image_data_.reset();
    }
}

void EpaperDashboard::EnsureProvisioningImageLoaded() {
    if (provisioning_image_data_ != nullptr) return;
    // 配网指引同样走烘焙 PNG：这块 SSD1619 上 LVGL 中文正文几乎不出墨
    // （黑底白字条能看见、正文刷不出来），图片通道才可靠。
    try {
        provisioning_image_data_ = std::make_unique<LvglAllocatedImage>(
            const_cast<uint8_t*>(provisioning_png), provisioning_png_len);
    } catch (...) {
        provisioning_image_data_.reset();
    }
}

void EpaperDashboard::BookNext() {
    if (!IsBookOpen() || book_page_ + 1 >= book_pages_.size()) return;
    ++book_page_;
    Show(Page::kBookReader);
}

void EpaperDashboard::BookPrevious() {
    if (!IsBookOpen() || book_page_ == 0) return;
    --book_page_;
    Show(Page::kBookReader);
}

void EpaperDashboard::SetWordListLoader(
    std::function<std::string()> loader) {
    if (word_machine_ == nullptr) {
        word_machine_ = std::make_unique<WordMachine>();
    }
    word_machine_->SetWordListLoader(std::move(loader));
}

void EpaperDashboard::WordNext() {
    if (word_machine_ == nullptr || !IsWords()) return;
    word_machine_->EnsureLoaded();
    word_machine_->Next();
    Show(Page::kWords);
}

void EpaperDashboard::WordPrevious() {
    if (word_machine_ == nullptr || !IsWords()) return;
    word_machine_->EnsureLoaded();
    word_machine_->Previous();
    Show(Page::kWords);
}

void EpaperDashboard::WordEnter() {
    if (word_machine_ == nullptr || !IsWords()) return;
    word_machine_->EnsureLoaded();
    word_machine_->Enter();
    Show(Page::kWords);
}

void EpaperDashboard::BeginConversation() {
    if (page_ == Page::kConversation) return;
    user_message_.clear();
    assistant_message_.clear();
    Show(Page::kConversation);
}

void EpaperDashboard::SetConversationMessage(const char* role,
                                             const char* content,
                                             bool refresh) {
    if (!role || !content || content[0] == '\0') return;
    if (std::strcmp(role, "user") == 0) {
        if (user_message_ == content && assistant_message_.empty()) return;
        user_message_ = content;
        assistant_message_.clear();
    } else if (std::strcmp(role, "assistant") == 0) {
        const std::string sentence(content);
        if (assistant_message_ == sentence ||
            (assistant_message_.size() >= sentence.size() &&
             assistant_message_.compare(assistant_message_.size() - sentence.size(),
                                        sentence.size(), sentence) == 0)) {
            return;
        }
        if (!assistant_message_.empty()) assistant_message_ += " ";
        assistant_message_ += sentence;
        // Retain several screen pages while bounding RAM usage. Never cut in
        // the middle of a UTF-8 character.
        if (assistant_message_.size() > 2048) {
            size_t cut = assistant_message_.size() - 2048;
            while (cut < assistant_message_.size() &&
                   (static_cast<unsigned char>(assistant_message_[cut]) & 0xc0) ==
                       0x80) {
                ++cut;
            }
            assistant_message_.erase(0, cut);
        }
    } else {
        return;
    }
    // Returning home is an explicit user action. Late chat packets may update
    // the cached transcript, but must not steal the menu back from the user.
    if (refresh && page_ == Page::kConversation) Show(Page::kConversation);
}

void EpaperDashboard::CommitConversation() {
    Show(Page::kConversation);
}

void EpaperDashboard::HideForVoice() {
    if (!visible_ || !overlay_) return;
    hidden_for_voice_ = true;
    if (lvgl_port_lock(3000)) {
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void EpaperDashboard::RestoreAfterVoice() {
    if (!visible_ || !hidden_for_voice_) return;
    hidden_for_voice_ = false;
    Show(page_);
}

void EpaperDashboard::SetSchedule(const std::string& schedule) {
    Settings settings("epd_dash", true);
    settings.SetString("schedule", schedule);
    if (page_ == Page::kSchedule && visible_) Show(page_);
}

void EpaperDashboard::SetTodos(const std::string& todos_json) {
    Settings settings("epd_dash", true);
    settings.SetString("todos_json", todos_json);
    if (page_ == Page::kTodo && visible_) Show(page_);
}

void EpaperDashboard::SetAlbumCaption(const std::string& caption) {
    Settings settings("epd_dash", true);
    settings.SetString("note_text", caption);
    if (page_ == Page::kNotes && visible_) Show(page_);
}

void EpaperDashboard::SetQuota(double balance, double used, int requests, long long tokens) {
    Settings settings("epd_dash", true);
    settings.SetString("quota_balance", std::to_string(balance));
    settings.SetString("quota_used", std::to_string(used));
    settings.SetInt("quota_requests", requests);
    settings.SetString("quota_tokens", std::to_string(tokens));
    if (page_ == Page::kQuota && visible_) Show(page_);
}

std::string EpaperDashboard::RenderHome() const {
    return {};
}

std::string EpaperDashboard::RenderSpeaking() const {
    if (!speaking_status_.empty()) return speaking_status_;
    return "";
}

void EpaperDashboard::SetSpeakingStatus(std::string status) {
    // "结束"/"错误"类状态不显示在屏幕上，清空状态让画面回到居中"小悟AI"。
    // 回调方仍可凭 status 内容触发 RestoreAudioService 等后续逻辑。
    const bool is_ending =
        status.find("结束") != std::string::npos ||
        status.find("错误") != std::string::npos ||
        status.find("失败") != std::string::npos;
    if (is_ending) {
        speaking_status_.clear();
    } else {
        speaking_status_ = std::move(status);
    }
    if (page_ != Page::kSpeaking || !visible_) return;
    // 节流：陪练实时对话中 transcript 事件密集到达，每次都触发完整
    // Show() → lv_refr_now() → 300ms 物理刷新会让屏幕和 UI 都卡。
    // 重要状态（错误/结束）立即刷新；普通 transcript 文本合并到
    // 800ms 窗口内只刷一次，既保证及时又不爆刷。
    static int64_t s_last_speaking_update_ms = 0;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (is_ending) {
        Show(page_);
        return;
    }
    if (now_ms - s_last_speaking_update_ms < 800) return;
    s_last_speaking_update_ms = now_ms;
    Show(page_);
}

std::string EpaperDashboard::RenderReading() const {
    if (book_names_.empty())
        return "没有找到 TXT 电子书\n\n请放入 /xiaozhi/books";
    std::string result = "TF 卡书库\n\n";
    for (size_t i = 0; i < book_names_.size(); ++i) {
        result += i == book_list_item_ ? "> " : "  ";
        result += book_names_[i];
        result += '\n';
        if (i >= 6) { result += "…"; break; }
    }
    return result;
}

std::string EpaperDashboard::RenderSchedule() const {
    Settings settings("epd_dash", false);
    const std::string schedule = settings.GetString("schedule", kDefaultSchedule);
    // Backwards compatible: old firmware stored human-readable lines.
    cJSON* root = cJSON_Parse(schedule.c_str());
    if (!root && schedule != kDefaultSchedule) {
        // 自愈：NVS 里的课表数据损坏（如旧版本写入的坏 JSON）时回退默认课表，
        // 避免把原始 JSON 文本当正文渲染到屏幕上。
        root = cJSON_Parse(kDefaultSchedule);
    }
    if (!root) return "今日课程\n\n课程表数据异常\n请在网页重新保存";
    const time_t now_s = time(nullptr);
    const tm now = LocalTime();
    const int today = (now.tm_wday == 0 ? 7 : now.tm_wday);  // 1=周一 … 7=周日
    const int minutes = now.tm_hour * 60 + now.tm_min;

    const cJSON* semester = cJSON_GetObjectItem(root, "semester_start");
    int week = 0;
    if (now.tm_year + 1900 >= 2024 && cJSON_IsNumber(semester) &&
        now_s >= static_cast<time_t>(semester->valuedouble)) {
        week = static_cast<int>((now_s - static_cast<time_t>(semester->valuedouble)) /
                                (7 * 24 * 3600)) + 1;
    }
    static const char* kWeekNames[] = {"", "星期一", "星期二", "星期三", "星期四",
                                       "星期五", "星期六", "星期日"};
    std::string out;
    if (week > 0) out += "第" + std::to_string(week) + "周 ";
    out += std::string(kWeekNames[today]) + " 今日课程\n";

    struct Course {
        std::string name, teacher, start, end;
        int start_m = -1, end_m = -1;
    };
    std::vector<Course> courses_today;
    const cJSON* courses = cJSON_GetObjectItem(root, "courses");
    if (cJSON_IsArray(courses)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, courses) {
            const cJSON* day = cJSON_GetObjectItem(item, "day");
            if (!cJSON_IsNumber(day) || day->valueint != today) continue;
            const cJSON* name = cJSON_GetObjectItem(item, "name");
            if (!cJSON_IsString(name)) continue;
            Course c;
            c.name = name->valuestring;
            const cJSON* room = cJSON_GetObjectItem(item, "room");
            if (cJSON_IsString(room)) c.teacher = room->valuestring;
            const cJSON* sj = cJSON_GetObjectItem(item, "start");
            const cJSON* ej = cJSON_GetObjectItem(item, "end");
            if (cJSON_IsString(sj)) c.start = sj->valuestring;
            if (cJSON_IsString(ej)) c.end = ej->valuestring;
            int sh = 0, sm = 0, eh = 0, em = 0;
            if (std::sscanf(c.start.c_str(), "%d:%d", &sh, &sm) == 2) c.start_m = sh * 60 + sm;
            if (std::sscanf(c.end.c_str(), "%d:%d", &eh, &em) == 2) c.end_m = eh * 60 + em;
            courses_today.push_back(std::move(c));
        }
    }
    cJSON_Delete(root);
    std::stable_sort(courses_today.begin(), courses_today.end(),
                     [](const Course& a, const Course& b) { return a.start_m < b.start_m; });

    if (today >= 6) {
        return out + "\n今天没有课程\n\n周末休息\n劳逸结合";
    }
    if (courses_today.empty()) {
        return out + "\n今天没有课程\n\n可用网页 :8080/schedule\n编辑课程表";
    }

    // 定位当前节与下一节
    int current_idx = -1, next_idx = -1;
    for (size_t i = 0; i < courses_today.size(); ++i) {
        const auto& c = courses_today[i];
        if (c.start_m >= 0 && c.end_m >= 0 && minutes >= c.start_m && minutes <= c.end_m) {
            current_idx = static_cast<int>(i);
        }
    }
    for (size_t i = (current_idx >= 0 ? current_idx + 1 : 0); i < courses_today.size(); ++i) {
        if (courses_today[i].start_m > minutes) { next_idx = static_cast<int>(i); break; }
    }

    // 固定节次开始时间（与网页 /schedule 网格一致）。按时间把课程归入
    // 10 个节次槽位，删掉某节课时槽位留空但不错位；匹配不上的课程挂到
    // 空闲槽位末尾。
    static const int kPeriodStartMins[10] = {
        8 * 60, 8 * 60 + 50, 9 * 60 + 45, 11 * 60, 11 * 60 + 50,
        14 * 60 + 10, 15 * 60 + 5, 16 * 60, 16 * 60 + 55, 17 * 60 + 50};
    const Course* slots[10] = {};
    for (const auto& c : courses_today) {
        int slot = -1;
        for (int p = 0; p < 10; ++p) {
            if (c.start_m == kPeriodStartMins[p]) { slot = p; break; }
        }
        if (slot < 0) {
            for (int p = 0; p < 10; ++p) {
                if (!slots[p]) { slot = p; break; }
            }
        }
        if (slot >= 0) slots[slot] = &c;
    }
    // 槽位 -> courses_today 索引（用于当前节/下节详情）
    auto slot_index = [&](int p) -> int {
        if (!slots[p]) return -1;
        for (size_t i = 0; i < courses_today.size(); ++i) {
            if (&courses_today[i] == slots[p]) return static_cast<int>(i);
        }
        return -1;
    };
    auto period_current = [&](int p) {
        const int idx = slot_index(p);
        return idx >= 0 && idx == current_idx;
    };

    // 等宽字体下按 UTF-8 显示宽度（ASCII=1，中文=2）补齐/截断
    auto utf8_width = [](const std::string& s) {
        int w = 0;
        for (size_t i = 0; i < s.size();) {
            const unsigned char ch = static_cast<unsigned char>(s[i]);
            const size_t len = ch < 0x80 ? 1 : ((ch & 0xe0) == 0xc0 ? 2
                                            : ((ch & 0xf0) == 0xe0 ? 3 : 4));
            w += (len == 1) ? 1 : 2;
            i += len;
        }
        return w;
    };
    auto pad_to = [&](std::string s, int target) {
        for (int w = utf8_width(s); w < target; ++w) s += ' ';
        return s;
    };
    // 格子里科目名最长 5 个汉字宽，超出截断加省略号
    auto short_name = [&](const std::string& name) {
        std::string r;
        int w = 0;
        for (size_t i = 0; i < name.size();) {
            const unsigned char ch = static_cast<unsigned char>(name[i]);
            const size_t len = ch < 0x80 ? 1 : ((ch & 0xe0) == 0xc0 ? 2
                                            : ((ch & 0xf0) == 0xe0 ? 3 : 4));
            const int cw = (len == 1) ? 1 : 2;
            if (w + cw > 10) { r += "…"; break; }
            r.append(name, i, len);
            w += cw;
            i += len;
        }
        return r;
    };

    // 双列课表：一行两个固定节次槽位，当前节行首用 '>' 高亮；
    // 空槽位（未排课）留白但占位，保证节次号永远对齐。
    for (int p = 0; p < 10; p += 2) {
        auto cell = [&](int period) {
            const int period_no = period + 1;
            std::string s = period_current(period) ? ">" : " ";
            s += period_no >= 10 ? std::to_string(period_no)
                                 : " " + std::to_string(period_no);
            if (slots[period]) {
                s += " " + short_name(slots[period]->name);
            }
            return pad_to(std::move(s), 15);
        };
        out += cell(p);
        out += "|" + cell(p + 1);
        out += '\n';
    }

    // 底部两行：当前节（含老师/下课时间）与下节预告。节次号取槽位号，
    // 与课表格子中的编号一致。
    auto with_teacher = [](const Course& c) {
        return c.teacher.empty() ? c.name : c.name + "(" + c.teacher + ")";
    };
    auto slot_of_course = [&](int course_idx) {
        for (int p = 0; p < 10; ++p) {
            if (slot_index(p) == course_idx) return p + 1;
        }
        return course_idx + 1;  // 回退：列表序号
    };
    if (current_idx >= 0) {
        const auto& c = courses_today[current_idx];
        out += ">第" + std::to_string(slot_of_course(current_idx)) + "节 " +
               with_teacher(c) + " 至" + c.end + "\n";
        if (next_idx >= 0) {
            const auto& nx = courses_today[next_idx];
            out += "下节 第" + std::to_string(slot_of_course(next_idx)) + "节 " +
                   with_teacher(nx) + " " + nx.start + "\n";
        } else {
            out += "今天最后一节，加油\n";
        }
    } else if (next_idx >= 0) {
        const auto& nx = courses_today[next_idx];
        out += "即将 第" + std::to_string(slot_of_course(next_idx)) + "节 " +
               with_teacher(nx) + " " + nx.start + "\n";
        out += "课前做好准备\n";
    } else {
        out += "今天课程已结束\n辛苦了，记得复习\n";
    }
    return out;
}

std::string EpaperDashboard::RenderNotes() const {
    Settings settings("epd_dash", false);
    return settings.GetString("note_text", "暂无便签\n\n可通过语音添加便签");
}

std::string EpaperDashboard::RenderTodos() const {
    Settings settings("epd_dash", false);
    const std::string json = settings.GetString("todos_json", "[]");
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return "暂无待办\n\n可通过网页或 MCP 添加"; }
    struct Item { int priority; bool done; std::string text; };
    std::vector<Item> items;
    cJSON* node = nullptr;
    cJSON_ArrayForEach(node, root) {
        const cJSON* text = cJSON_GetObjectItem(node, "text");
        if (!cJSON_IsString(text)) continue;
        const cJSON* priority = cJSON_GetObjectItem(node, "priority");
        const cJSON* done = cJSON_GetObjectItem(node, "completed");
        items.push_back({cJSON_IsNumber(priority) ? priority->valueint : 2,
                         static_cast<bool>(cJSON_IsTrue(done)), text->valuestring});
    }
    cJSON_Delete(root);
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.done != b.done) return !a.done;
        return a.priority < b.priority;
    });
    std::string out;
    int shown = 0;
    for (const auto& item : items) {
        out += item.done ? "[x] " : "[ ] ";
        out += item.priority <= 1 ? "高 " : (item.priority == 2 ? "中 " : "低 ");
        out += item.text; out += '\n';
        if (++shown >= 7) break;
    }
    if (items.empty()) out = "暂无待办\n\n可通过网页或 MCP 添加";
    else out += "\n显示 " + std::to_string(shown) + "/" + std::to_string(items.size());
    return out;
}

std::string EpaperDashboard::RenderQuota() const {
    Settings settings("epd_dash", false);
    const auto balance = settings.GetString("quota_balance", "--");
    const auto used = settings.GetString("quota_used", "--");
    const auto requests = settings.GetInt("quota_requests", 0);
    const auto tokens = settings.GetString("quota_tokens", "0");
    char text[320];
    std::snprintf(text, sizeof(text),
                  "余额 %s   用量 %s\n请求 %ld   令牌 %s\n\n支持中转站额度接口",
                  balance.c_str(), used.c_str(), static_cast<long>(requests), tokens.c_str());
    return text;
}

std::string EpaperDashboard::RenderAnalysis() const {
    // 学情分析占位页：数据来源（口语陪练/单词学习记录）尚未打通，
    // 先给出明确提示，后续在此聚合真实学习数据。
    return "学情分析\n\n暂无学习数据\n\n口语陪练与单词学习记录\n将在这里汇总展示";
}

std::string EpaperDashboard::RenderProfile() const {
    // 两态模型：浏览态（选字段）和编辑态（选值）。
    // profile_popup_ 充当"编辑态"标志，popup_sel_ 是临时选中项。
    Settings settings("lightacade", false);
    const std::string language = settings.GetString("language", "en");
    const std::string grade = settings.GetString("grade_band", "junior");
    const std::string voice = settings.GetString("voice", "Puck");

    std::string body = "口语陪练设置\n\n";
    auto field_line = [&](int idx, const std::string& value) {
        std::string line;
        if (!profile_popup_) {
            // 浏览态：> 标记当前字段
            line += (profile_field_ == idx ? "> " : "  ");
        } else {
            // 编辑态：当前被编辑的字段用 [ ] 框住
            line += (profile_field_ == idx ? "[ " : "  ");
        }
        line += (idx == 0 ? "语种  " : idx == 1 ? "学段  " : "音色  ");
        line += value;
        if (profile_popup_ && profile_field_ == idx) line += " ]";
        return line;
    };
    body += field_line(0, ProfileName(kProfileLanguages, language)) + "\n";
    body += field_line(1, ProfileName(kProfileGrades, grade)) + "\n";
    body += field_line(2, ProfileName(kProfileVoices, voice)) + "\n";

    if (!profile_popup_) {
        // ── 浏览态：底部预览当前字段所有可选项（多列网格）──
        const auto fi = GetProfileFieldInfo(profile_field_);
        body += "\n";
        int col = 0;
        for (size_t i = 0; i < fi.count; ++i) {
            body += fi.options[i].name;
            if (++col >= fi.columns) { col = 0; body += "\n"; }
            else { body += "  "; }
        }
        if (col != 0) body += "\n";
        body += "\n上下键选字段 | OK进入编辑\n长按OK返回首页";
    } else {
        // ── 编辑态：底部单行水平显示所有选项，> < 高亮选中项 ──
        const auto fi = GetProfileFieldInfo(profile_field_);
        body += "\n";
        for (size_t i = 0; i < fi.count; ++i) {
            if (static_cast<int>(i) == profile_popup_sel_) {
                body += ">"; body += fi.options[i].name; body += "<";
            } else {
                body += " "; body += fi.options[i].name; body += " ";
            }
            body += " ";
        }
        body += "\n\n上下键切选项 | OK确认保存\n长按OK放弃返回";
    }
    return body;
}

std::string EpaperDashboard::RenderWelcome() const {
    // 配网成功后默认进入的欢迎页（A 屏黑白快刷）。
    // 风格：亲切、有社区感；同时给出售后群号，方便用户第一时间加群。
    // 注意：A 屏为黑白墨水屏，正文不含 emoji（字库无彩色表情），
    // 行数控制在 4.2" 屏可完整显示的范围内。
    // footer 另有"按 OK 进入功能菜单"提示，此处正文只放引导文案。
    return "你好大佬，欢迎来到小悟众创！\n\n"
           "这是你的小悟AI墨水屏——\n"
           "一块会思考的电子纸。\n\n"
           "售后群  1056214329\n"
           "有问题，随时戳我们\n\n"
           "一起进小悟众创\n"
           "把脑洞变成屏幕上的现实";
}

void EpaperDashboard::ProfilePrev() {
    if (page_ != Page::kProfile) return;
    if (profile_popup_) {
        // 弹窗内：上键 = 左移选中项
        const auto fi = GetProfileFieldInfo(profile_field_);
        profile_popup_sel_ = (profile_popup_sel_ + static_cast<int>(fi.count) - 1) % fi.count;
    } else {
        // 主页面：上键 = 移到上一个字段
        profile_field_ = (profile_field_ + 2) % 3;
    }
    Show(Page::kProfile);
}

void EpaperDashboard::ProfileNext() {
    if (page_ != Page::kProfile) return;
    if (profile_popup_) {
        // 弹窗内：下键 = 右移选中项
        const auto fi = GetProfileFieldInfo(profile_field_);
        profile_popup_sel_ = (profile_popup_sel_ + 1) % fi.count;
    } else {
        // 主页面：下键 = 移到下一个字段
        profile_field_ = (profile_field_ + 1) % 3;
    }
    Show(Page::kProfile);
}

void EpaperDashboard::ProfileConfirm() {
    if (page_ != Page::kProfile) return;
    if (!profile_popup_) {
        // 主页面 OK：进入弹窗，根据当前 NVS 值定位选中项
        profile_popup_ = true;
        const auto fi = GetProfileFieldInfo(profile_field_);
        Settings settings("lightacade", false);
        const std::string current = settings.GetString(fi.nvs_key, fi.default_value);
        profile_popup_sel_ = 0;
        for (size_t i = 0; i < fi.count; ++i) {
            if (current == fi.options[i].code) {
                profile_popup_sel_ = static_cast<int>(i);
                break;
            }
        }
    } else {
        // 弹窗 OK：保存选中项到 NVS 并关闭弹窗
        const auto fi = GetProfileFieldInfo(profile_field_);
        if (profile_popup_sel_ < static_cast<int>(fi.count)) {
            Settings settings("lightacade", true);
            settings.SetString(fi.nvs_key, fi.options[profile_popup_sel_].code);
        }
        profile_popup_ = false;
    }
    Show(Page::kProfile);
}

std::string EpaperDashboard::RenderSettings() const {
    // 设置页：展示设备基本信息（MAC 用于设备识别与授权登记）。
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    wifi_ap_record_t ap = {};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    char text[192];
    std::snprintf(text, sizeof(text),
                  "设备信息\n\nMAC %02X:%02X:%02X:%02X:%02X:%02X\nWiFi %d dBm\n\n更多设置开发中",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
    return text;
}

void EpaperDashboard::Render() {
    std::string body;
    lv_label_set_text(title_, PageName(page_));
    if (welcome_image_) lv_obj_add_flag(welcome_image_, LV_OBJ_FLAG_HIDDEN);
    if (title_) lv_obj_remove_flag(title_, LV_OBJ_FLAG_HIDDEN);
    // 今日看板 + 陪练页：整幅画面/字幕需要全屏，LVGL footer 隐藏；
    // 陪练页还需要顶部状态栏（WiFi/USB）。
    // 今日看板还需要隐藏 title_（服务器渲染画面自带标题/时间/电量）。
    const bool immersive = page_ == Page::kInkSight || page_ == Page::kSpeaking;
    if (immersive) {
        lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        if (page_ == Page::kInkSight) {
            lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(status_line_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(title_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(status_line_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_label_ && page_ == Page::kSpeaking) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            // WiFi 信号分四档：≥ -50 强, -50~-70 中, -70~-80 弱, < -80 无
            int rssi = 0;
            wifi_ap_record_t ap = {};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
            }
            const char* wifi_icon = FONT_AWESOME_WIFI;       // 默认满格
            if (rssi == 0) wifi_icon = FONT_AWESOME_TRIANGLE_EXCLAMATION;  // 未连接
            else if (rssi < -80) wifi_icon = FONT_AWESOME_WIFI_SLASH;
            else if (rssi < -70) wifi_icon = FONT_AWESOME_WIFI_WEAK;
            else if (rssi < -50) wifi_icon = FONT_AWESOME_WIFI_FAIR;
            // 板子 USB 供电，显示 "USB" 代替电池百分比。
            lv_label_set_text(status_label_,
                (std::string(wifi_icon) + " " + "USB").c_str());
        } else if (status_label_) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_remove_flag(title_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        if (status_line_) lv_obj_add_flag(status_line_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_) lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    switch (page_) {
        case Page::kConversation: {
            const std::string transcript =
                user_message_.empty() && assistant_message_.empty()
                    ? "正在聆听…"
                    : "我：" + (user_message_.empty() ? "…" : user_message_) +
                          "\n\n小悟：" +
                          (assistant_message_.empty() ? "正在思考…"
                                                      : assistant_message_);
            const auto pages = PaginateConversation(transcript);
            body = pages.back().text;
            if (pages.size() > 1) {
                char title[48];
                std::snprintf(title, sizeof(title), "小悟对话  %u/%u",
                              static_cast<unsigned>(pages.size()),
                              static_cast<unsigned>(pages.size()));
                lv_label_set_text(title_, title);
            }
            break;
        }
        case Page::kSpeaking: body = RenderSpeaking(); break;
        case Page::kReading: body = RenderReading(); break;
        case Page::kWords: {
            if (word_machine_ == nullptr) {
                word_machine_ = std::make_unique<WordMachine>();
            }
            word_machine_->EnsureLoaded();
            body = word_machine_->Body();
            lv_label_set_text(title_, word_machine_->Title().c_str());
            break;
        }
        case Page::kSchedule: body = RenderSchedule(); break;
        case Page::kCalendar: {
            const auto now = LocalTime();
            char calendar[160];
            std::snprintf(calendar, sizeof(calendar),
                          "%04d年%02d月%02d日\n\n星期%d\n\n上下键选择功能",
                          now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                          now.tm_wday == 0 ? 7 : now.tm_wday);
            body = calendar;
            break;
        }
        case Page::kBookReader: {
            const BookPage& current = book_pages_[book_page_];
            char heading[160];
            std::snprintf(heading, sizeof(heading), "%s  %u/%u\n\n",
                          book_title_.c_str(),
                          static_cast<unsigned>(book_page_ + 1),
                          static_cast<unsigned>(book_pages_.size()));
            if (!current.image.empty()) {
                EnsureBookImageLoaded(
                    std::string(current.image.data(), current.image.size()));
                if (book_image_data_ != nullptr) {
                    const lv_image_dsc_t* dsc =
                        book_image_data_->image_dsc();
                    lv_image_set_src(book_image_, dsc);
                    // Fit inside the area below the title (<=392x260 px).
                    constexpr int kUnit = 256;  // LV_SCALE_NONE
                    int scale = kUnit;
                    if (dsc->header.w > 0) {
                        scale = std::min(scale, kUnit * 392 / dsc->header.w);
                    }
                    if (dsc->header.h > 0) {
                        scale = std::min(scale, kUnit * 260 / dsc->header.h);
                    }
                    lv_image_set_scale(book_image_,
                                       std::max(scale, kUnit / 4));
                    lv_obj_align(book_image_, LV_ALIGN_TOP_MID, 0, 24);
                    lv_obj_remove_flag(book_image_, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(book_image_, LV_OBJ_FLAG_HIDDEN);
                }
                body = heading;
                if (book_image_data_) {
                    body += "[插图]";
                } else {
                    body += "插图无法显示：";
                    body += current.image.c_str();
                }
            } else {
                lv_obj_add_flag(book_image_, LV_OBJ_FLAG_HIDDEN);
                body = heading;
                body.append(current.text.data(), current.text.size());
            }
            break;
        }
        case Page::kNotes: body = RenderNotes(); break;
        case Page::kTodo: body = RenderTodos(); break;
        case Page::kQuota: body = RenderQuota(); break;
        case Page::kAnalysis: body = RenderAnalysis(); break;
        case Page::kProfile: body = RenderProfile(); break;
        case Page::kActivation: body = activation_message_; break;
        case Page::kProvisioning: {
            // 未配网整屏指引。墨水屏全刷要几十秒，必须一屏把用户要做的
            // 全部说清：连哪个热点（下方黑底条）、打开什么网址、之后去哪
            // 输入激活码。字号用全局 20px 字体，400px 宽约容 19 个汉字，
            // 每行控制在 18 字以内避免折行。
            const std::string url =
                provisioning_url_.empty() ? "192.168.4.1" : provisioning_url_;
            body = "设备未联网，请按步骤配网\n\n";
            body += "1 手机连下方热点\n";
            body += "2 浏览器打开 " + url + "\n";
            body += "3 选家庭 WiFi 输密码\n";
            body += "4 屏幕显示 6 位激活码\n";
            body += "5 打开 xiaozhi.me\n";
            body += "  输入激活码完成绑定";
            break;
        }
        case Page::kWelcome: body = RenderWelcome(); break;
        case Page::kInkSight:
            // InkSight 画面直接通过 HTTP 解码后刷显存，LVGL title/body
            // 均隐藏。body 留空占位。
            body.clear();
            break;
        default: body = RenderHome(); break;
    }
    // The reader hides the footer hint line; let the book text use the
    // freed height instead of leaving a blank row above the panel edge.
    // Speaking/InkSight pages also hide footer to give transcript / server
    // frame maximum vertical room.
    if (page_ == Page::kBookReader) {
        lv_obj_set_height(body_, LV_PCT(84));
    } else if (page_ == Page::kSpeaking) {
        // 陪练页：隐藏 footer，字幕区撑满到底（95%）。
        lv_obj_set_height(body_, LV_PCT(95));
    } else if (page_ == Page::kActivation) {
        lv_obj_set_height(body_, LV_PCT(52));
    } else if (page_ == Page::kInkSight) {
        // 看板页：LVGL body 留极小占位，服务器画面直接刷显存覆盖整块。
        lv_obj_set_height(body_, LV_PCT(5));
    } else {
        lv_obj_set_height(body_, LV_PCT(72));
    }
    // InkSight 页面 title 被隐藏，body 顶到屏幕最上方不留 24px 空白。
    // 陪练页无 status 时居中显示"小悟AI"，有 status 时左上对齐显示字幕。
    if (page_ == Page::kSpeaking && body == "小悟AI") {
        // 高度设为内容自适应，对象本身只有一行高，CENTER 才能真正居中。
        lv_obj_set_height(body_, LV_SIZE_CONTENT);
        lv_obj_align(body_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(body_, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_align(body_, LV_ALIGN_TOP_LEFT, 2,
                     (page_ == Page::kInkSight) ? 0 : 24);
        lv_obj_set_style_text_align(body_, LV_TEXT_ALIGN_LEFT, 0);
    }
    lv_label_set_text(body_, body.c_str());
    if (page_ == Page::kActivation) {
        std::string code_text = "激活码  ";
        code_text += activation_code_.empty() ? "请稍候" : activation_code_;
        lv_label_set_text(activation_code_label_, code_text.c_str());
        lv_obj_remove_flag(activation_code_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(activation_code_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (page_ == Page::kHome) {
        static constexpr const char* menu_names[] = {
            "口语陪练", "阅读", "单词", "课程表", "学情分析", "用户中心"
        };
        lv_obj_add_flag(body_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        RenderMenu();
        char footer[96];
        std::snprintf(footer, sizeof(footer), "当前：%s  ·  +/-选择  ·  OK唤醒",
                      menu_names[selected_item_]);
        lv_label_set_text(footer_, footer);
    } else if (page_ == Page::kWelcome || page_ == Page::kProvisioning) {
        // 欢迎页 / 配网页都优先显示编译期烘焙的全屏 PNG（logo、欢迎语，
        // 或配网 5 步指引）。这块 SSD1619 上 LVGL 中文正文几乎不出墨：
        // 实测黑底白字条能看见，20px 正文字形却刷不出来，所以整屏信息
        // 一律走图片通道；只有图片解码失败时才回退到纯文本。
        lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        const bool provisioning = (page_ == Page::kProvisioning);
        if (provisioning) {
            EnsureProvisioningImageLoaded();
        } else {
            EnsureWelcomeImageLoaded();
        }
        LvglImage* banner = provisioning ? provisioning_image_data_.get()
                                         : welcome_image_data_.get();
        if (banner != nullptr) {
            lv_image_set_src(welcome_image_, banner->image_dsc());
            lv_obj_align(welcome_image_, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_remove_flag(welcome_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(body_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(welcome_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(body_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(footer_, provisioning ? "配网成功后自动继续"
                                                    : "按 OK 进入功能菜单");
        }
    } else {
        lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(body_, LV_OBJ_FLAG_HIDDEN);
        if (page_ == Page::kBookReader) {
            // Hide the key hints while reading a book.
            lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        } else if (page_ == Page::kProfile) {
            // 用户中心正文已含完整按键提示，隐藏全局 footer。
            lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        } else if (page_ == Page::kWords) {
            lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(footer_, word_machine_->Footer().c_str());
        } else if (page_ == Page::kInkSight || page_ == Page::kSpeaking) {
            // 页脚已在上方隐藏：看板/陪练只全屏显示服务器画面或字幕。
        } else if (page_ == Page::kActivation) {
            lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(footer_, "连接热点后按页面提示完成配置");
        } else {
            lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(footer_, "OK确认 · 长按返回");
        }
    }
}

void EpaperDashboard::RenderMenu() {
    for (int i = 0; i < kMenuItemCount; ++i) {
        const bool selected = i == selected_item_;
        lv_obj_set_style_bg_opa(cards_[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(cards_[i], lv_color_black(), 0);
        const lv_color_t color = selected ? lv_color_white() : lv_color_black();
        lv_obj_set_style_text_color(card_icons_[i], color, 0);
        lv_obj_set_style_text_color(card_labels_[i], color, 0);
        lv_obj_set_style_border_width(cards_[i], selected ? 2 : 1, 0);
    }
}

void EpaperDashboard::SetInkSightFooter(std::string footer) {
    // 今日看板页已隐藏 LVGL 标题/页脚：状态只记日志，不再重绘、不再触发
    // 物理刷新。否则差分刷新会把标题栏+页脚重新叠加到服务器渲染的画面上
    //（表现为看板刷两下后又被 LVGL 边框覆盖）。
    inksight_footer_ = std::move(footer);
    ESP_LOGI(kDashboardTag, "XiaowuAI: %s", inksight_footer_.c_str());
}
