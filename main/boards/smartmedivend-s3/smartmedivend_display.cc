#include "smartmedivend_display.h"

#include "application.h"
#include "boards/common/board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include <material_symbols.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t kBg = 0x10263A;
constexpr uint32_t kPanel = 0x1A3A53;
constexpr uint32_t kMint = 0x46E8CE;
constexpr uint32_t kWhite = 0xF8F9FF;
constexpr uint32_t kMuted = 0xB8D8F5;

lv_obj_t* Panel(lv_obj_t* parent, int y, int h) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, 12, y);
    lv_obj_set_size(obj, 216, h);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kPanel), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, int w, int x, int y,
                uint32_t color, const lv_font_t* font, lv_text_align_t align) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

// Bound UI text on every cloud/user message; do not truncate within a UTF-8 sequence.
void SetPreview(lv_obj_t* label, const char* text) {
    if (!label) return;
    if (!text) { lv_label_set_text(label, ""); return; }
    constexpr size_t kMax = 176;
    size_t n = 0;
    while (n < kMax && text[n] != '\0') ++n;
    if (text[n] != '\0') {
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    }
    char preview[kMax + 1];
    memcpy(preview, text, n);
    preview[n] = '\0';
    lv_label_set_text(label, preview);
}
} // namespace

void SmartMediVendDisplay::SetupUI() {
    if (setup_ui_called_) return;
    DisplayLockGuard lock(this);
    if (!lock) return;
    Display::SetupUI();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* font = theme->text_font()->font();
    const lv_font_t* icon_font = theme->icon_font()->font();
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kWhite), 0);
    lv_obj_set_style_text_font(screen, font, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    header_label_ = Label(
        screen, "SmartMediVend",
        184, 13, 16,
        kMint, font,
        LV_TEXT_ALIGN_LEFT
    );
    network_label_ = Label(screen, "", 30, 199, 14, kMint, icon_font, LV_TEXT_ALIGN_CENTER);
    auto* top = Panel(screen, 53, 153);
    robot_label_ = Label(
        top, MATERIAL_SYMBOLS_ROBOT_2,
        62, 77, 14,
        kMint, icon_font,
        LV_TEXT_ALIGN_CENTER
    );
    welcome_label_ = Label(top, "Xin chào!", 196, 10, 59, kWhite, font, LV_TEXT_ALIGN_CENTER);
    message_label_ = Label(top, "Tôi có thể hỗ trợ thông tin sức khỏe\nvà hướng dẫn mua thuốc.",
                           194, 11, 88, kMuted, font, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(message_label_, 58);
    lv_label_set_long_mode(message_label_, LV_LABEL_LONG_DOT);

    auto* bottom = Panel(screen, 218, 66);
    constexpr int heights[5] = {10, 17, 28, 17, 10};
    for (int i = 0; i < 5; ++i) {
        activity_bars_[i] = lv_obj_create(bottom);
        lv_obj_set_pos(activity_bars_[i], 88 + i * 10, 5 + (28 - heights[i]) / 2);
        lv_obj_set_size(activity_bars_[i], 5, heights[i]);
        lv_obj_set_style_radius(activity_bars_[i], 4, 0);
        lv_obj_set_style_bg_color(activity_bars_[i], lv_color_hex(kMint), 0);
        lv_obj_set_style_border_width(activity_bars_[i], 0, 0);
        lv_obj_set_style_pad_all(activity_bars_[i], 0, 0);
        lv_obj_add_flag(activity_bars_[i], LV_OBJ_FLAG_HIDDEN);
    }
    mode_label_ = Label(bottom, "Sẵn sàng", 198, 9, 39, kWhite, font, LV_TEXT_ALIGN_CENTER);
    status_label_ = Label(screen, "", 216, 12, 291, kMuted, font, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    notification_label_ = Label(screen, "", 216, 12, 291, kMint, font, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
}


void SmartMediVendDisplay::SetTheme(Theme* theme) {
    if (theme == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }

    auto* lvgl_theme =
        static_cast<LvglTheme*>(theme);

    auto text_font_owner =
        lvgl_theme->text_font();

    auto icon_font_owner =
        lvgl_theme->icon_font();

    if (!text_font_owner ||
        !icon_font_owner ||
        !text_font_owner->font() ||
        !icon_font_owner->font()) {
        return;
    }

    const lv_font_t* text_font =
        text_font_owner->font();

    const lv_font_t* icon_font =
        icon_font_owner->font();

    if (setup_ui_called_) {

        lv_obj_set_style_text_font(
            lv_screen_active(), text_font, 0);

        lv_obj_t* text_labels[] = {
            header_label_,
            welcome_label_,
            message_label_,
            mode_label_,
            status_label_,
            notification_label_
        };

        for (auto* label : text_labels) {
            if (label != nullptr) {
                lv_obj_set_style_text_font(
                    label, text_font, 0);
            }
        }

        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(
                network_label_, icon_font, 0);
        }

        if (robot_label_ != nullptr) {
            lv_obj_set_style_text_font(
                robot_label_, icon_font, 0);
        }
    }

    Display::SetTheme(theme);
}

void SmartMediVendDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !message_label_) return;
    if (!role || !content || content[0] == '\0') return;
    if (strcmp(role, "assistant") == 0) {
        lv_label_set_text(welcome_label_, "SmartMediVend");
        SetPreview(message_label_, content);
    } else if (strcmp(role, "user") == 0) {
        lv_label_set_text(welcome_label_, "Bạn vừa nói");
        SetPreview(message_label_, content);
    }
}

void SmartMediVendDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !message_label_) return;
    lv_label_set_text(welcome_label_, "Xin chào!");
    lv_label_set_text(message_label_, "Tôi có thể hỗ trợ thông tin sức khỏe\nvà hướng dẫn mua thuốc.");
}

void SmartMediVendDisplay::SetEmotion(const char* /*emotion*/) {
    // Icon stays a stable, low-cost robot illustration; no animated assets needed.
}

void SmartMediVendDisplay::UpdateStatusBar(bool /*update_all*/) {
    auto& board = Board::GetInstance();
    const auto state = Application::GetInstance().GetDeviceState();
    DisplayLockGuard lock(this);
    if (!lock || !setup_ui_called_ || !mode_label_) return;
    const char* network_icon = board.GetNetworkStateIcon();
    if (network_icon && network_label_) lv_label_set_text(network_label_, network_icon);
    if (last_state_ == static_cast<int>(state)) return;
    last_state_ = static_cast<int>(state);
    const char* mode = "Sẵn sàng";
    bool listening = false;
    switch (state) {
        case kDeviceStateListening: mode = "Đang lắng nghe..."; listening = true; break;
        case kDeviceStateSpeaking: mode = "Đang trả lời..."; listening = true; break;
        case kDeviceStateConnecting: mode = "Đang kết nối Xiaozhi..."; break;
        case kDeviceStateWifiConfiguring: mode = "Cài đặt Wi-Fi"; break;
        case kDeviceStateStarting: mode = "Đang khởi động..."; break;
        case kDeviceStateActivating: mode = "Kích hoạt thiết bị"; break;
        case kDeviceStateUpgrading: mode = "Đang cập nhật"; break;
        case kDeviceStateFatalError: mode = "Lỗi hệ thống"; break;
        case kDeviceStateAudioTesting: mode = "Kiểm tra âm thanh"; break;
        case kDeviceStateNotifying: mode = "Thông báo"; break;
        default: break;
    }
    lv_label_set_text(mode_label_, mode);
    for (auto* bar : activity_bars_) {
        if (listening) lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
}
