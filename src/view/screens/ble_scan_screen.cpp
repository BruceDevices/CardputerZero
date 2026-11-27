/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "ble_scan_screen.h"

#include "asset_manager.h"
#include "ble_scan_parser.h"
#include "linux_input.h"
#include "theme.h"
#include "ui_const.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace screen {
namespace {

constexpr int32_t kContentH = view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight;
constexpr size_t kMaxDevices = 100; // firmware MAX_DISPLAY_DEVICES

// bluetoothctl is the supported D-Bus client. `--timeout 5` matches the
// firmware's SCANTIME; the outer timeout is only a hard cap. sed removes the
// ANSI colors/readline markers before the C++ parser sees them.
constexpr const char* kScanScript =
    "timeout 8 bluetoothctl --timeout 5 scan le 2>&1 | "
    "sed -u -e 's/\\x1b\\[[0-9;]*m//g' -e 's/[\\x01\\x02]//g'";

void log_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fputs("[ble-scan] ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
}

} // namespace

BleScanScreen::BleScanScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

BleScanScreen::~BleScanScreen() {
    runner_.stop();
    if (sudo_group_) {
        lv_group_delete(sudo_group_);
        sudo_group_ = nullptr;
    }
}

void BleScanScreen::build_content(lv_obj_t* content) {
    content_root_ = content;
    category_index_ = view_model().active_category();
    item_index_ = view_model().active_item();
    // Firmware ble_scan(): scan on entry, no Run press.
    run_scan();
}

// --- scanning ---------------------------------------------------------------

void BleScanScreen::run_scan() {
    if (runner_.running()) {
        return;
    }
    // Same command the catalog row uses as its fallback, minus the awk filter:
    // the parser needs every [NEW]/[CHG] line to collect names and RSSI.
    scan_action_ = model::ToolAction{"BLE Scan", "Scan nearby BLE devices (5s)",
                                     {"sh", "-c", kScanScript}, true};
    selected_ = 0;
    start_scan(scan_action_);
}

void BleScanScreen::start_scan(const model::ToolAction& action) {
    phase_ = Phase::Scanning;
    scan_error_.clear();
    devices_.clear();
    truncated_ = false;
    used_password_ = false;
    show_scanning_banner();

    const bool needs_root = model::action_needs_root(action);
    pending_argv_ = needs_root ? model::sudo_argv_with_password(action) : action.argv;
    const bool passwordless = needs_root && model::sudo_passwordless();

    if (needs_root && !passwordless) {
        show_sudo_dialog();
        return;
    }
    launch_scan(model::launch_argv(action), "");
}

void BleScanScreen::launch_scan(const std::vector<std::string>& argv,
                                const std::string& stdin_payload) {
    scan_buffer_.clear();
    if (!runner_.start(argv, &BleScanScreen::process_cb, this, stdin_payload)) {
        build_empty_view("Scan failed", "Failed to launch the BLE scan.");
    }
}

void BleScanScreen::process_cb(void* owner, const std::string& text, bool exited) {
    auto* self = static_cast<BleScanScreen*>(owner);
    if (!self) {
        return;
    }
    self->on_scan_output(text);
    if (exited) {
        self->on_scan_exit();
    }
}

void BleScanScreen::on_scan_output(const std::string& text) {
    scan_buffer_ += text;
}

void BleScanScreen::on_scan_exit() {
    log_line("scan exited, %zu bytes", scan_buffer_.size());

    // sudo refused (no passwordless rule) or rejected the entered password:
    // show the same on-device prompt as ToolRunScreen/WifiScanScreen.
    const bool refused = scan_buffer_.find("a password is required") != std::string::npos ||
                         scan_buffer_.find("a terminal is required") != std::string::npos;
    const bool rejected = used_password_ &&
                          (scan_buffer_.find("incorrect password") != std::string::npos ||
                           scan_buffer_.find("Sorry, try again") != std::string::npos);
    if (rejected || (!used_password_ && refused)) {
        show_sudo_dialog();
        return;
    }

    parse_scan(scan_buffer_);
    log_line("parsed %zu devices%s", devices_.size(), truncated_ ? " (truncated)" : "");

    if (devices_.empty()) {
        const std::string title = scan_error_.empty() ? "No devices found" : "Scan failed";
        const std::string body =
            scan_error_.empty() ? "Enable Bluetooth and scan again."
                                : scan_error_.substr(0, 180);
        build_empty_view(title, body);
        return;
    }
    build_list_view();
}

void BleScanScreen::parse_scan(const std::string& out) {
    // The parser is shared with BLE wardriving (platform/ble_scan_parser.h).
    const platform::ble::ScanResult r = platform::ble::parse_scan(out, {kMaxDevices, false});
    devices_.clear();
    for (const auto& d : r.devices) {
        devices_.push_back({d.name, d.mac, d.rssi});
    }
    truncated_ = r.truncated;
    scan_error_ = r.error;
}

// --- views ------------------------------------------------------------------

void BleScanScreen::clear_content() {
    if (content_root_) {
        lv_obj_clean(content_root_);
    }
    list_ = nullptr;
    rows_.clear();
    main_menu_row_ = nullptr;
    detail_box_ = nullptr;
    detail_title_ = nullptr;
    detail_label_ = nullptr;
    detail_hint_ = nullptr;
}

void BleScanScreen::show_scanning_banner() {
    clear_content();
    const auto pal = view::palette(view_model().is_dark_mode());

    auto* title = lv_label_create(content_root_);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, pal.primary, 0);
    lv_label_set_text(title, "Scanning..");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -8);

    auto* sub = lv_label_create(content_root_);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, pal.text_disabled, 0);
    lv_label_set_text(sub, "Bluetooth LE  -  5 s");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 16);

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, "", nullptr, nullptr);
    nav_bar().set_icon(2, "", nullptr, nullptr);
    nav_bar().set_icon(3, "", nullptr, nullptr);
    nav_bar().set_icon(4, "", nullptr, nullptr);
    nav_bar().set_enabled(4, false);
}

void BleScanScreen::build_list_view() {
    clear_content();
    phase_ = Phase::List;
    if (selected_ > devices_.size()) {
        selected_ = 0;
    }
    const auto pal = view::palette(view_model().is_dark_mode());

    auto* header = lv_label_create(content_root_);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(header, pal.text, 0);
    lv_label_set_text(header, "BLE Scan");
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 8, 2);

    char count[32];
    std::snprintf(count, sizeof(count), "%zu%s found", devices_.size(), truncated_ ? "+" : "");
    auto* count_label = lv_label_create(content_root_);
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(count_label, pal.text_disabled, 0);
    lv_label_set_text(count_label, count);
    lv_obj_align(count_label, LV_ALIGN_TOP_RIGHT, -8, 3);

    list_ = lv_list_create(content_root_);
    lv_obj_set_size(list_, LV_PCT(100), kContentH - 22);
    lv_obj_align(list_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 1, 0);
    lv_obj_set_style_pad_row(list_, 1, 0);

    populate_list();

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
}

void BleScanScreen::populate_list() {
    rows_.clear();
    main_menu_row_ = nullptr;
    const auto pal = view::palette(view_model().is_dark_mode());

    for (const auto& dev : devices_) {
        const std::string label = dev.name.empty() ? dev.address : dev.name;
        auto* row = lv_list_add_button(list_, nullptr, label.c_str());
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        rows_.push_back(row);
    }

    if (truncated_) {
        auto* row = lv_list_add_text(list_, "... and more devices");
        lv_obj_set_style_text_font(row, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(row, pal.text_disabled, 0);
        rows_.push_back(row); // never selected; kept so apply_highlight resets it
    }

    // Bruce firmware appends addOptionToMainMenu() to the scan list.
    main_menu_row_ = lv_list_add_button(list_, nullptr, "Main Menu");
    lv_obj_clear_flag(main_menu_row_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(main_menu_row_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_ver(main_menu_row_, 2, 0);
    rows_.push_back(main_menu_row_);

    apply_highlight();
}

lv_obj_t* BleScanScreen::selected_row() const {
    if (selected_ < devices_.size() && selected_ < rows_.size()) {
        return rows_[selected_];
    }
    return main_menu_row_;
}

void BleScanScreen::apply_highlight() {
    const auto pal = view::palette(view_model().is_dark_mode());
    lv_obj_t* selected = selected_row();
    for (lv_obj_t* row : rows_) {
        if (row == selected) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, pal.primary, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_text_color(row, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_remove_local_style_prop(row, LV_STYLE_TEXT_COLOR, 0);
        }
    }
    if (selected) {
        lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
    }
}

std::string BleScanScreen::detail_text(size_t index) const {
    const Device& dev = devices_[index];
    std::string out = "Name: ";
    out += dev.name.empty() ? "<no name>" : dev.name;
    out += "\nAdresse: ";
    out += dev.address;
    char line[32];
    std::snprintf(line, sizeof(line), "\nSignal: %d dBm", dev.rssi);
    out += line;
    return out;
}

void BleScanScreen::build_detail_view(size_t index) {
    if (index >= devices_.size()) {
        return;
    }
    clear_content();
    phase_ = Phase::Detail;
    const auto pal = view::palette(view_model().is_dark_mode());

    detail_title_ = lv_label_create(content_root_);
    lv_obj_set_width(detail_title_, LV_PCT(100));
    lv_obj_set_style_text_font(detail_title_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(detail_title_, pal.primary, 0);
    lv_obj_set_style_text_align(detail_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(detail_title_, "-=Information=-");
    lv_obj_align(detail_title_, LV_ALIGN_TOP_MID, 0, 2);

    detail_box_ = lv_obj_create(content_root_);
    lv_obj_set_size(detail_box_, LV_PCT(100), kContentH - 42);
    lv_obj_align(detail_box_, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_pad_all(detail_box_, 4, 0);
    lv_obj_set_style_bg_opa(detail_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_box_, 0, 0);
    lv_obj_set_scroll_dir(detail_box_, LV_DIR_VER);

    detail_label_ = lv_label_create(detail_box_);
    lv_obj_set_width(detail_label_, LV_PCT(100));
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(detail_label_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(detail_label_, pal.text, 0);
    lv_label_set_text(detail_label_, detail_text(index).c_str());

    detail_hint_ = lv_label_create(content_root_);
    lv_obj_set_width(detail_hint_, LV_PCT(100));
    lv_obj_set_style_text_font(detail_hint_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(detail_hint_, pal.text_disabled, 0);
    lv_obj_set_style_text_align(detail_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(detail_hint_, "Press [Sel] to act");
    lv_obj_align(detail_hint_, LV_ALIGN_BOTTOM_MID, 0, -2);

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
}

void BleScanScreen::build_empty_view(const std::string& title, const std::string& body) {
    clear_content();
    phase_ = Phase::List;
    devices_.clear();
    truncated_ = false;
    selected_ = 0;
    main_menu_row_ = nullptr;
    const auto pal = view::palette(view_model().is_dark_mode());

    auto* title_label = lv_label_create(content_root_);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, pal.text, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title_label, title.c_str());
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, -16);

    auto* body_label = lv_label_create(content_root_);
    lv_obj_set_width(body_label, LV_PCT(88));
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(body_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(body_label, pal.text_disabled, 0);
    lv_obj_set_style_text_align(body_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(body_label, body.c_str());
    lv_obj_align(body_label, LV_ALIGN_CENTER, 0, 10);

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, "", nullptr, nullptr);
    nav_bar().set_icon(2, "", nullptr, nullptr);
    nav_bar().set_icon(3, "", nullptr, nullptr);
    nav_bar().set_icon(4, "Scan", scan_cb, this);
}

// --- navigation -------------------------------------------------------------

void BleScanScreen::move_selection(int delta) {
    if (phase_ == Phase::Detail) {
        if (detail_box_) {
            lv_obj_scroll_by(detail_box_, 0, -delta * 24, LV_ANIM_ON);
        }
        return;
    }
    if (phase_ != Phase::List || devices_.empty()) {
        return;
    }
    const int count = static_cast<int>(devices_.size()) + 1; // + Main Menu
    int next = (static_cast<int>(selected_) + delta) % count;
    if (next < 0) {
        next += count;
    }
    selected_ = static_cast<size_t>(next);
    apply_highlight();
}

void BleScanScreen::select_current() {
    if (phase_ == Phase::Detail) {
        build_list_view();
        return;
    }
    if (phase_ != Phase::List || devices_.empty()) {
        return;
    }
    if (selected_ < devices_.size()) {
        build_detail_view(selected_);
        return;
    }
    // "Main Menu" row (firmware addOptionToMainMenu()).
    runner_.stop();
    view_model().show_menu(-1);
}

void BleScanScreen::handle_back() {
    if (phase_ == Phase::Detail) {
        build_list_view();
        return;
    }
    runner_.stop();
    view_model().go_back();
}

// --- sudo password prompt (mirrors ToolRunScreen) ---------------------------

void BleScanScreen::show_sudo_dialog() {
    if (sudo_modal_) {
        return;
    }
    platform::set_text_input_mode(true);

    sudo_modal_ = lv_obj_create(root());
    lv_obj_set_size(sudo_modal_, view::kScreenWidth, view::kScreenHeight);
    lv_obj_set_pos(sudo_modal_, 0, 0);
    lv_obj_set_style_bg_color(sudo_modal_, lv_obj_get_style_bg_color(root(), LV_PART_MAIN), 0);
    lv_obj_set_style_bg_opa(sudo_modal_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sudo_modal_, 0, 0);
    lv_obj_set_style_pad_all(sudo_modal_, 0, 0);
    lv_obj_clear_flag(sudo_modal_, LV_OBJ_FLAG_SCROLLABLE);

    auto* title = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Sudo password needed");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    sudo_ta_ = lv_textarea_create(sudo_modal_);
    lv_obj_set_size(sudo_ta_, LV_PCT(88), 34);
    lv_obj_align(sudo_ta_, LV_ALIGN_TOP_MID, 0, 34);
    lv_textarea_set_one_line(sudo_ta_, true);
    lv_textarea_set_password_mode(sudo_ta_, true);
    lv_obj_set_style_text_font(sudo_ta_, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(sudo_ta_, sudo_submit_cb, LV_EVENT_READY, this);

    auto* hint = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_label_set_text(hint, "device default is often 'pi'");
    lv_obj_align_to(hint, sudo_ta_, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    auto* keys = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_12, 0);
    lv_label_set_text(keys, "ESC cancel - Enter OK");
    lv_obj_align_to(keys, hint, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    nav_bar().set_icon(0, view::NAV_BACK, sudo_cancel_cb, this);
    nav_bar().set_icon(2, view::NAV_RUN, sudo_submit_cb, this);
    nav_bar().set_enabled(1, false);
    nav_bar().set_enabled(3, false);
    nav_bar().set_enabled(4, false);

    sudo_group_ = lv_group_create();
    lv_group_add_obj(sudo_group_, sudo_ta_);
    lv_group_focus_obj(sudo_ta_);
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, sudo_group_);
        }
    }
}

void BleScanScreen::close_sudo_dialog() {
    if (!sudo_modal_) {
        return;
    }
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, nullptr);
        }
    }
    lv_group_delete(sudo_group_);
    sudo_group_ = nullptr;
    lv_obj_delete(sudo_modal_);
    sudo_modal_ = nullptr;
    sudo_ta_ = nullptr;
    platform::set_text_input_mode(false);
    nav_bar().set_enabled(4, true);
}

void BleScanScreen::submit_sudo_dialog() {
    if (!sudo_modal_) {
        return;
    }
    const std::string password = lv_textarea_get_text(sudo_ta_);
    if (password.empty()) {
        return;
    }
    close_sudo_dialog();
    used_password_ = true;
    launch_scan(pending_argv_, password + "\n");
}

// --- nav callbacks ----------------------------------------------------------

void BleScanScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->handle_back();
    }
}

void BleScanScreen::up_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(-1);
    }
}

void BleScanScreen::down_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(1);
    }
}

void BleScanScreen::select_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->select_current();
    }
}

void BleScanScreen::scan_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self && !self->runner_.running()) {
        self->run_scan();
    }
}

void BleScanScreen::sudo_submit_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->submit_sudo_dialog();
    }
}

void BleScanScreen::sudo_cancel_cb(lv_event_t* event) {
    auto* self = static_cast<BleScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->close_sudo_dialog();
        self->handle_back();
    }
}

} // namespace screen
