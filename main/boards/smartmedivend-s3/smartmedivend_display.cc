#include "smartmedivend_display.h"

#include "application.h"
#include "boards/common/board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "smartmedivend_artwork.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstring>

namespace {
constexpr char kTag[] = "SmartMediVendUI";
constexpr uint32_t kInk = 0x134C92;
constexpr uint32_t kIceWhite = 0xF8FCFF;
constexpr uint32_t kSkyBlue = 0x1574CE;
constexpr uint32_t kStatusBlue = 0x185DA8;
constexpr uint32_t kFramePeriodMs = 40; // 25Hz maximum; unchanged eyes cause no LCD transfer.

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* value, int x, int y,
                    int width, int height, uint32_t color, const lv_font_t* font,
                    lv_text_align_t alignment = LV_TEXT_ALIGN_CENTER) {
    auto* obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_align(obj, alignment, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_label_set_text(obj, value);
    return obj;
}

const lv_image_dsc_t* Artwork() {
    static const lv_image_dsc_t picture = [] {
        lv_image_dsc_t value{};
        value.header.magic = LV_IMAGE_HEADER_MAGIC;
        value.header.cf = LV_COLOR_FORMAT_RGB565;
        value.header.w = smartmedivend_artwork::kWidth;
        value.header.h = smartmedivend_artwork::kHeight;
        value.header.stride = smartmedivend_artwork::kWidth * sizeof(uint16_t);
        value.data_size = sizeof(smartmedivend_artwork::kPixels);
        value.data = reinterpret_cast<const uint8_t*>(smartmedivend_artwork::kPixels);
        return value;
    }();
    return &picture;
}

// Ensure the text preview ends on a UTF-8 boundary. This is only a visual
// summary: the full cloud transcript is left untouched in the Xiaozhi core.
void SetPreview(lv_obj_t* label, const char* text) {
    if (!label) return;
    if (!text) { lv_label_set_text(label, ""); return; }
    constexpr size_t kMaxBytes = 176;
    size_t count = 0;
    while (count < kMaxBytes && text[count] != '\0') ++count;
    if (count == kMaxBytes) {
        while (count > 0 && (static_cast<unsigned char>(text[count]) & 0xC0) == 0x80) {
            --count;
        }
    }
    char preview[kMaxBytes + 1];
    memcpy(preview, text, count);
    preview[count] = '\0';
    lv_label_set_text(label, preview);
}
} // namespace

void SmartMediVendDisplay::SetupUI() {
    if (setup_ui_called_) return;
    DisplayLockGuard lock(this);
    if (!lock || current_theme_ == nullptr) return;
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (!theme->text_font() || !theme->icon_font() ||
        !theme->text_font()->font() || !theme->icon_font()->font()) return;
    Display::SetupUI();
    const lv_font_t* font = theme->text_font()->font();
    const lv_font_t* icons = theme->icon_font()->font();
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kIceWhite), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kInk), 0);
    lv_obj_set_style_text_font(screen, font, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Full static image (153600 bytes in flash) painted ONCE by LVGL. All
    // later frames invalidate only the 2x 54x54 eye sprites, not 240x320.
    auto* background = lv_image_create(screen);
    lv_image_set_src(background, Artwork());
    lv_obj_set_pos(background, 0, 0);
    lv_obj_remove_flag(background, LV_OBJ_FLAG_SCROLLABLE);

    eyes_.Reset(static_cast<uint32_t>(esp_timer_get_time() / 1000), esp_random());
    for (int i = 0; i < 2; ++i) {
        const size_t bytes = smartmedivend_eyes::kSprite *
                             smartmedivend_eyes::kSprite * sizeof(uint16_t);
        eye_pixels_[i] = static_cast<uint16_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!eye_pixels_[i]) {
            eye_pixels_[i] = static_cast<uint16_t*>(
                heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!eye_pixels_[i]) {
            ESP_LOGE(kTag, "Cannot allocate %u-byte eye buffer %d", (unsigned)bytes, i);
            // Continue showing the safe background and text; do not reboot or
            // introduce another task if eye animation memory is unavailable.
            continue;
        }
        eye_canvas_[i] = lv_canvas_create(screen);
        lv_canvas_set_buffer(eye_canvas_[i], eye_pixels_[i],
                             smartmedivend_eyes::kSprite, smartmedivend_eyes::kSprite,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(eye_canvas_[i],
            smartmedivend_eyes::kCentersX[i] - smartmedivend_eyes::kSprite / 2,
            smartmedivend_eyes::kCenterY - smartmedivend_eyes::kSprite / 2);
        lv_obj_remove_flag(eye_canvas_[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    // Reference typography remains in the immutable artwork during idle.
    // When a real transcript arrives, a white overlay avoids overlapping it.
    chat_panel_ = lv_obj_create(screen);
    lv_obj_set_pos(chat_panel_, 13, 182);
    lv_obj_set_size(chat_panel_, 214, 78);
    lv_obj_set_style_bg_color(chat_panel_, lv_color_hex(kIceWhite), 0);
    lv_obj_set_style_bg_opa(chat_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chat_panel_, 0, 0);
    lv_obj_set_style_radius(chat_panel_, 5, 0);
    lv_obj_set_style_pad_all(chat_panel_, 0, 0);
    lv_obj_remove_flag(chat_panel_, LV_OBJ_FLAG_SCROLLABLE);
    welcome_label_ = MakeLabel(chat_panel_, "Xin chào!", 6, 4, 202, 24, kInk, font);
    message_label_ = MakeLabel(chat_panel_, "", 6, 30, 202, 46, kStatusBlue, font);
    lv_obj_add_flag(chat_panel_, LV_OBJ_FLAG_HIDDEN);

    // Labels used by the base class's status/notification timer must exist.
    // Keep these unobtrusive on the clear space above the bottom wave.
    network_label_ = MakeLabel(screen, "", 209, 59, 21, 19, kSkyBlue, icons);
    status_label_ = MakeLabel(screen, "", 14, 238, 212, 20, kStatusBlue, font);
    notification_label_ = MakeLabel(screen, "", 14, 238, 212, 20, kSkyBlue, font);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    // Read-only status sheet: retains the medical header and bottom wave.
    // Created once, toggled with the physical double click; no new task/heap churn.
    status_panel_ = lv_obj_create(screen);
    lv_obj_set_pos(status_panel_, 12, 70);
    lv_obj_set_size(status_panel_, 216, 202);
    lv_obj_set_style_bg_color(status_panel_, lv_color_hex(0xF4FBFF), 0);
    lv_obj_set_style_bg_opa(status_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_panel_, 0, 0);
    lv_obj_set_style_radius(status_panel_, 12, 0);
    lv_obj_set_style_pad_all(status_panel_, 0, 0);
    lv_obj_remove_flag(status_panel_, LV_OBJ_FLAG_SCROLLABLE);
    status_title_ = MakeLabel(status_panel_, "TRẠNG THÁI", 7, 8, 202, 23, kInk, font);
    status_wifi_ = MakeLabel(status_panel_, "Wi-Fi: đang kiểm tra", 8, 48,
                             200, 23, kStatusBlue, font, LV_TEXT_ALIGN_LEFT);
    status_cloud_ = MakeLabel(status_panel_, "Xiaozhi: đang kiểm tra", 8, 77,
                              200, 24, kStatusBlue, font, LV_TEXT_ALIGN_LEFT);
    status_vending_ = MakeLabel(status_panel_, "Cấp thuốc: ĐANG KHÓA", 8, 110,
                                200, 25, kStatusBlue, font, LV_TEXT_ALIGN_LEFT);
    status_hint_ = MakeLabel(status_panel_, "Nhấn đúp để quay về", 8, 165, 200, 21,
                             kSkyBlue, font);
    lv_obj_add_flag(status_panel_, LV_OBJ_FLAG_HIDDEN);

    TickEyes(); // Create the initial open-eye image BEFORE the timer starts.
    if (eye_canvas_[0] || eye_canvas_[1]) {
        eye_timer_ = lv_timer_create(EyeTimerCallback, kFramePeriodMs, this);
        if (!eye_timer_) ESP_LOGW(kTag, "Eye timer unavailable: static eyes retained");
    }
}

void SmartMediVendDisplay::EyeTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<SmartMediVendDisplay*>(lv_timer_get_user_data(timer));
    if (self) self->TickEyes();
}

void SmartMediVendDisplay::TickEyes() {
    if (!setup_ui_called_) return;
    // LVGL calls its timers in the same GUI task. No nested display lock,
    // extra FreeRTOS task, blocking delay or SPI transaction is needed here.
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (status_page_visible_) {
        if (static_cast<int32_t>(now - next_status_refresh_ms_) >= 0) {
            next_status_refresh_ms_ = now + 750;
            RefreshStatusPage();
        }
        return; // Keep the eye buffers untouched behind the opaque sheet.
    }
    const auto frame = eyes_.Tick(now);
    if (have_drawn_frame_ && frame == drawn_frame_) return;
    for (int i = 0; i < 2; ++i) {
        if (!eye_canvas_[i] || !eye_pixels_[i]) continue;
        smartmedivend_eyes::DrawEye(eye_pixels_[i], smartmedivend_artwork::kPixels,
                                   i, frame);
        lv_obj_invalidate(eye_canvas_[i]);
    }
    drawn_frame_ = frame;
    have_drawn_frame_ = true;
}

void SmartMediVendDisplay::RefreshStatusPage() {
    if (!status_page_visible_ || !status_panel_) return;
    const auto state = Application::GetInstance().GetDeviceState();
    // This page reports states the firmware actually knows. No guessed RSSI/IP.
    const char* wifi = "Wi-Fi: đang kiểm tra";
    if (state == kDeviceStateWifiConfiguring) wifi = "Wi-Fi: đang cấu hình";
    else if (state == kDeviceStateStarting || state == kDeviceStateConnecting)
        wifi = "Wi-Fi: đang kết nối";
    else if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
             state == kDeviceStateActivating) wifi = "Wi-Fi: đã kết nối";

    const char* cloud = "Xiaozhi: đang khởi động";
    switch (state) {
        case kDeviceStateListening: cloud = "Xiaozhi: đang nghe"; break;
        case kDeviceStateSpeaking: cloud = "Xiaozhi: đang trả lời"; break;
        case kDeviceStateActivating: cloud = "Xiaozhi: đang kích hoạt"; break;
        case kDeviceStateWifiConfiguring: cloud = "Xiaozhi: chờ Wi-Fi"; break;
        case kDeviceStateFatalError: cloud = "Xiaozhi: lỗi hệ thống"; break;
        case kDeviceStateConnecting: cloud = "Xiaozhi: đang kết nối"; break;
        case kDeviceStateStarting: cloud = "Xiaozhi: đang khởi động"; break;
        default: cloud = "Xiaozhi: sẵn sàng"; break;
    }
    if (status_wifi_) lv_label_set_text(status_wifi_, wifi);
    if (status_cloud_) lv_label_set_text(status_cloud_, cloud);
    if (status_vending_) lv_label_set_text(status_vending_, "Cấp thuốc: ĐANG KHÓA");
}

void SmartMediVendDisplay::ToggleStatusPage() {
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !status_panel_) return;
    status_page_visible_ = !status_page_visible_;
    if (status_page_visible_) {
        lv_obj_remove_flag(status_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(status_panel_); // foreground even after chat/notification
        RefreshStatusPage();
        next_status_refresh_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 750;
    } else {
        lv_obj_add_flag(status_panel_, LV_OBJ_FLAG_HIDDEN);
        // Force one redraw to repaint live eyes immediately on return.
        have_drawn_frame_ = false;
        TickEyes();
    }
    ESP_LOGI(kTag, "Status page %s", status_page_visible_ ? "shown" : "hidden");
}

void SmartMediVendDisplay::SetTheme(Theme* theme) {
    if (!theme) return;
    DisplayLockGuard lock(this);
    if (!lock) return;
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    const auto text_owner = lvgl_theme->text_font();
    const auto icon_owner = lvgl_theme->icon_font();
    if (!text_owner || !icon_owner || !text_owner->font() || !icon_owner->font()) return;

    // The parent LcdDisplay::SetTheme assumes default widgets (mute/battery)
    // are allocated, which they are not in this custom layout.
    if (setup_ui_called_) {
        lv_obj_set_style_text_font(lv_screen_active(), text_owner->font(), 0);
        for (auto* label : {welcome_label_, message_label_, status_title_, status_hint_,
                             status_wifi_, status_cloud_, status_vending_, status_label_,
                             notification_label_}) {
            if (label) lv_obj_set_style_text_font(label, text_owner->font(), 0);
        }
        if (network_label_) lv_obj_set_style_text_font(network_label_, icon_owner->font(), 0);
    }
    Display::SetTheme(theme);
}

void SmartMediVendDisplay::SetChatMessage(const char* role, const char* content) {
    if (!role || !content || !content[0]) return;
    const bool assistant = strcmp(role, "assistant") == 0;
    const bool user = strcmp(role, "user") == 0;
    if (!assistant && !user) return;
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !chat_panel_) return;
    lv_label_set_text(welcome_label_, assistant ? "Smart Medi Vending" : "Bạn vừa nói");
    SetPreview(message_label_, content);
    lv_obj_remove_flag(chat_panel_, LV_OBJ_FLAG_HIDDEN);
}

void SmartMediVendDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !chat_panel_) return;
    lv_obj_add_flag(chat_panel_, LV_OBJ_FLAG_HIDDEN);
}

void SmartMediVendDisplay::SetEmotion(const char* /*emotion*/) {
    // Eye animation is self-timed by LVGL; server-provided emotions cannot
    // interrupt GUI ownership or drive the vending hardware.
}

void SmartMediVendDisplay::UpdateStatusBar(bool /*update_all*/) {
    auto& board = Board::GetInstance();
    const auto state = Application::GetInstance().GetDeviceState();
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !status_label_) return;
    const char* icon = board.GetNetworkStateIcon();
    if (network_label_) lv_label_set_text(network_label_, icon ? icon : "");
    if (last_state_ == static_cast<int>(state)) return;
    last_state_ = static_cast<int>(state);
    const char* status = nullptr;
    switch (state) {
        case kDeviceStateListening: status = "Đang lắng nghe..."; break;
        case kDeviceStateSpeaking: status = "Đang trả lời..."; break;
        case kDeviceStateConnecting: status = "Đang kết nối Xiaozhi..."; break;
        case kDeviceStateWifiConfiguring: status = "Cài đặt Wi-Fi"; break;
        case kDeviceStateStarting: status = "Đang khởi động..."; break;
        case kDeviceStateActivating: status = "Kích hoạt thiết bị"; break;
        case kDeviceStateUpgrading: status = "Đang cập nhật"; break;
        case kDeviceStateFatalError: status = "Lỗi hệ thống"; break;
        case kDeviceStateAudioTesting: status = "Kiểm tra âm thanh"; break;
        default: break;
    }
    if (status) {
        lv_label_set_text(status_label_, status);
        // Leave an active notification visible until its original timeout.
        if (lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_label_set_text(status_label_, "");
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
}
