#pragma once

#include <lvgl.h>

#include <array>
#include <cstddef>
#include <esp_heap_caps.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lvgl_image.h"
#include "word_machine.h"

// Book pages are numerous small strings; keep them in PSRAM so a fully
// loaded novel does not starve the internal heap (Opus/network need it).
template <class T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <class U>
    PsramAllocator(const PsramAllocator<U>&) {}
    T* allocate(std::size_t n) {
        return static_cast<T*>(
            heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    void deallocate(T* p, std::size_t) { heap_caps_free(p); }
};
template <class T, class U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) {
    return true;
}
template <class T, class U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) {
    return false;
}
using PsramString =
    std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

class EpaperDashboard {
public:
    enum class Page {
        kHome, kConversation, kSpeaking, kReading, kWords, kSchedule,
        kCalendar, kBookReader, kNotes, kTodo, kQuota, kInkSight,
        kAnalysis, kProfile, kActivation, kProvisioning, kWelcome, kCount
    };

    EpaperDashboard();
    ~EpaperDashboard();

    void Show(Page page);
    void Show(const std::string& page_name);
    void Next();
    void Previous();
    void Enter();
    void Back();
    void BeginConversation();
    void SetConversationMessage(const char* role, const char* content,
                                bool refresh = true);
    void CommitConversation();
    void OpenBook(const std::string& title, const std::string& utf8_text);
    void ShowBookList(const std::vector<std::string>& books);
    void ShowBookError(const std::string& message);
    // TXT 图片页支持：标记 [[img:文件名.png]] 的 PNG 放在 /xiaozhi/books。
    void SetBookImageLoader(
        std::function<std::unique_ptr<LvglImage>(const std::string&)> loader);
    bool CurrentBookPageIsImage() const;
    void BookListNext();
    void BookListPrevious();
    std::string SelectedBook() const;
    void BookNext();
    void BookPrevious();
    bool IsHome() const { return page_ == Page::kHome; }
    bool IsWelcome() const { return page_ == Page::kWelcome; }
    bool IsConversation() const { return page_ == Page::kConversation; }
    bool IsBookList() const { return page_ == Page::kReading; }
    bool IsBookOpen() const { return page_ == Page::kBookReader; }
    bool IsWords() const { return page_ == Page::kWords; }
    bool IsInkSight() const { return page_ == Page::kInkSight; }
    bool IsProfile() const { return page_ == Page::kProfile; }
    // 用户中心（口语设置）按键：上下移动字段光标，OK 循环切换当前字段的值。
    void ProfilePrev();
    void ProfileNext();
    void ProfileConfirm();
    // Word trainer keys: next/previous move the learn cursor (or score a
    // recall), Enter starts/reveals/skips the recall flow.
    void SetWordListLoader(std::function<std::string()> loader);
    void WordNext();
    void WordPrevious();
    void WordEnter();
    int SelectedItem() const { return selected_item_; }
    // true requests the panel's fast/partial waveform; false requests a
    // full waveform (used when changing between pages).
    void SetRefreshCallback(std::function<void(bool, bool)> callback) {
        refresh_callback_ = std::move(callback);
    }
    void HideForVoice();
    void RestoreAfterVoice();
    void SetSchedule(const std::string& schedule);
    void SetTodos(const std::string& todos_json);
    void SetAlbumCaption(const std::string& caption);
    void SetQuota(double balance, double used, int requests, long long tokens);
    std::string CurrentPageName() const;
    // InkSight 今日看板专用：页面渲染回调由 Board 提供（因为需要调用
    // Ssd1619Display::RawFrameRefresh + HTTP），Dashboard 负责页面进入/退出
    // 时向 Board 发送信号并让 footer 表现一致。
    using InkSightShowFn = std::function<void(bool /*initial_full_refresh*/)>;
    void SetInkSightShow(InkSightShowFn fn) { inksight_show_ = std::move(fn); }
    void SetInkSightFooter(std::string footer);
    void SetSpeakingStatus(std::string status);
    void ShowActivation(std::string message, std::string code);
    void HideActivation();
    // 未配网时的整屏配网指引页：热点名 + 配网地址 + 后续激活码引导。
    // 用独立页面而不是 Alert 提示框，因为墨水屏上 Alert 只画一小条覆盖层，
    // 用户根本看不清要连哪个热点、打开什么网址。
    void ShowProvisioning(std::string ssid, std::string url);

private:
    // One reader page: plain text, or a full-screen image when image is set.
    struct BookPage {
        PsramString text;
        PsramString image;
    };

    Page page_ = Page::kHome;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* footer_ = nullptr;
    lv_obj_t* activation_code_label_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;   // 顶部状态栏容器（WiFi 信号 + 电量/USB）
    lv_obj_t* status_label_ = nullptr;  // 状态栏右侧文字
    lv_obj_t* status_line_ = nullptr;   // 状态栏下方分割线
    lv_obj_t* menu_ = nullptr;
    static constexpr int kMenuItemCount = 6;
    std::array<lv_obj_t*, kMenuItemCount> cards_{};
    std::array<lv_obj_t*, kMenuItemCount> card_icons_{};
    std::array<lv_obj_t*, kMenuItemCount> card_labels_{};
    int selected_item_ = 0;
    // 用户中心当前选中的设置项：0=语种 1=学段 2=音色
    int profile_field_ = 0;
    // 弹窗模式：OK 打开选项浮层，上/下=左/右移动，OK 保存关闭，长按=放弃关闭
    bool profile_popup_ = false;
    int profile_popup_sel_ = 0;  // 弹窗内当前选中索引
    bool visible_ = false;
    bool hidden_for_voice_ = false;
    std::string user_message_;
    std::string assistant_message_;
    std::string book_title_;
    std::vector<std::string> book_names_;
    size_t book_list_item_ = 0;
    std::vector<BookPage> book_pages_;
    size_t book_page_ = 0;
    lv_obj_t* book_image_ = nullptr;
    lv_obj_t* welcome_image_ = nullptr;
    std::unique_ptr<LvglImage> book_image_data_;
    std::unique_ptr<LvglImage> welcome_image_data_;
    std::unique_ptr<LvglImage> provisioning_image_data_;
    std::string loaded_book_image_;
    std::function<std::unique_ptr<LvglImage>(const std::string&)>
        book_image_loader_;
    std::unique_ptr<WordMachine> word_machine_;
    // Full refreshes visibly blink 2-3 times, so page transitions use the
    // fast partial waveform and only every kTransitionsPerFull-th transition
    // performs a cleaning full refresh.
    static constexpr int kTransitionsPerFull = 6;
    int transitions_since_full_ = 0;
    std::string last_render_key_;
    std::function<void(bool, bool)> refresh_callback_;
    InkSightShowFn inksight_show_;
    std::string inksight_footer_ = "OK刷新 · 长按返回";
    std::string speaking_status_;
    std::string activation_message_;
    std::string activation_code_;
    std::string provisioning_ssid_;
    std::string provisioning_url_;

    void EnsureUi();
    void Render();
    void EnsureBookImageLoaded(const std::string& name);
    void EnsureWelcomeImageLoaded();
    void EnsureProvisioningImageLoaded();
    std::string RenderHome() const;
    std::string RenderSpeaking() const;
    std::string RenderReading() const;
    std::string RenderSchedule() const;
    std::string RenderNotes() const;
    std::string RenderTodos() const;
    std::string RenderQuota() const;
    std::string RenderAnalysis() const;
    std::string RenderProfile() const;
    std::string RenderSettings() const;
    std::string RenderWelcome() const;
    void RenderMenu();
    static const char* PageName(Page page);
};
