/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "terminal_screen.h"

#include "asset_manager.h"
#include "linux_input.h"
#include "theme.h"
#include "tool_catalog.h"
#include "ui_const.h"

#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstring>

namespace screen {
namespace {

constexpr int32_t kHeaderHeight  = 20;
constexpr int32_t kInputHeight   = 28;
constexpr size_t kMaxLogChars    = 6000;
constexpr size_t kMaxHistorySize = 50;

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// A leading "sudo " (after optional blanks) marks the command as a root one;
// the prefix is stripped so the root script never re-runs sudo inside an
// already-elevated shell (root's sudoers entry must not be assumed).
bool strip_sudo_prefix(std::string& command) {
    const std::string stripped = trim(command);
    if (stripped.rfind("sudo ", 0) != 0 && stripped != "sudo") {
        return false;
    }
    command = trim(stripped.substr(5));
    return true;
}

std::string prompt_prefix(bool as_root) {
    if (as_root) {
        return "root@cardputerzero:~# ";
    }
    const char* user = std::getenv("USER");
    if (!user || !user[0]) {
        user = "user";
    }
    return std::string(user) + "@cardputerzero:~$ ";
}

} // namespace

TerminalScreen::TerminalScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

TerminalScreen::~TerminalScreen() {
    runner_.stop();
    if (sudo_modal_) {
        close_sudo_dialog();
    }
    if (typing_) {
        leave_typing_mode();
    }
}

void TerminalScreen::build_content(lv_obj_t* content) {
    category_index_ = view_model().active_category();
    item_index_     = view_model().active_item();
    const auto& action = model::tool_catalog()[static_cast<size_t>(category_index_)]
                             .actions[static_cast<size_t>(item_index_)];

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_label_set_text(header, action.label);

    const int32_t content_height = view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight;
    log_ta_ = lv_textarea_create(content);
    lv_obj_set_size(log_ta_, LV_PCT(100), content_height - kHeaderHeight - kInputHeight);
    lv_obj_align(log_ta_, LV_ALIGN_TOP_MID, 0, kHeaderHeight);
    lv_textarea_set_cursor_click_pos(log_ta_, false);
    lv_obj_clear_flag(log_ta_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(log_ta_, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(log_ta_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(log_ta_, LV_SCROLLBAR_MODE_AUTO);
    lv_textarea_set_text(log_ta_,
                         "Bruce terminal - type a command, Enter runs it\n"
                         "'sudo cmd' runs as root; Up/Down history; ESC leaves input\n\n");

    input_ta_ = lv_textarea_create(content);
    lv_obj_set_size(input_ta_, LV_PCT(100), kInputHeight);
    lv_obj_align(input_ta_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_textarea_set_one_line(input_ta_, true);
    lv_textarea_set_cursor_click_pos(input_ta_, true);
    lv_obj_set_style_text_font(input_ta_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_width(input_ta_, 1, 0);
    lv_obj_add_event_cb(input_ta_, input_ready_cb, LV_EVENT_READY, this);

    // 0=Back/leave-input  1=Log up  2=Run/Stop  3=Log down  4=Type
    nav_bar().set_icon(1, view::NAV_UP, scroll_up_cb, this);
    nav_bar().set_icon(2, view::NAV_RUN, run_stop_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, scroll_down_cb, this);
    nav_bar().set_icon(4, "Type", type_cb, this);
    configure_common_nav(view::NAV_BACK, back_cb, this);
}

void TerminalScreen::run_command() {
    if (runner_.running()) {
        return;
    }
    std::string command = lv_textarea_get_text(input_ta_);
    command = trim(command);
    if (command.empty()) {
        return;
    }
    push_history(command);
    lv_textarea_set_text(input_ta_, "");
    const bool as_root = strip_sudo_prefix(command);
    append_log(prompt_prefix(as_root) + command + "\n");
    start_command(command, as_root);
}

void TerminalScreen::start_command(const std::string& command, bool as_root) {
    auto argv = model::terminal_command_argv(command, as_root);
    run_output_.clear();
    std::string stdin_payload;
    if (as_root && model::terminal_auth_mode() == model::TerminalAuth::Password) {
        if (cached_password_.empty()) {
            // Ask on-device first; run_command already echoed the prompt line.
            pending_command_ = command;
            pending_root_    = true;
            show_sudo_dialog();
            return;
        }
        stdin_payload                    = cached_password_ + "\n";
        last_run_used_sudo_password_     = true;
    } else {
        last_run_used_sudo_password_ = false;
    }
    if (!runner_.start(argv, &TerminalScreen::on_process_update, this, stdin_payload)) {
        append_log("[failed to launch]\n");
    }
    update_run_label();
}

void TerminalScreen::clear_log() {
    if (log_ta_) {
        lv_textarea_set_text(log_ta_, "");
        follow_tail_ = true;
    }
}

void TerminalScreen::append_log(const std::string& text) {
    if (!log_ta_ || text.empty()) {
        return;
    }

    if (lv_textarea_get_text(log_ta_)[0] != '\0' && strlen(lv_textarea_get_text(log_ta_)) > kMaxLogChars) {
        lv_textarea_set_text(log_ta_, "");
    }

    const lv_coord_t prev_scroll = lv_obj_get_scroll_y(log_ta_);
    lv_textarea_add_text(log_ta_, text.c_str());
    if (!follow_tail_) {
        lv_obj_scroll_to_y(log_ta_, prev_scroll, LV_ANIM_OFF);
    }
}

void TerminalScreen::scroll_log(int32_t delta) {
    if (!log_ta_) {
        return;
    }
    lv_obj_scroll_by(log_ta_, 0, delta, LV_ANIM_OFF);
    follow_tail_ = delta <= 0 && lv_obj_get_scroll_bottom(log_ta_) < 2;
}

void TerminalScreen::update_run_label() {
    nav_bar().set_text(2, runner_.running() ? view::NAV_STOP : view::NAV_RUN);
}

void TerminalScreen::enter_typing_mode() {
    if (typing_ || sudo_modal_) {
        return;
    }
    typing_ = true;
    platform::set_text_input_mode(true);

    // While typing every key (digits included) must reach the input line, so
    // all nav slots go dark; ESC keeps nav meaning and lands on repointed
    // slot 0 = leave input.
    nav_bar().set_icon(0, "Esc", type_cb, this);
    nav_bar().set_enabled(1, false);
    nav_bar().set_enabled(2, false);
    nav_bar().set_enabled(3, false);
    nav_bar().set_enabled(4, false);

    typing_group_ = lv_group_create();
    lv_group_add_obj(typing_group_, input_ta_);
    lv_group_focus_obj(input_ta_);
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, typing_group_);
        }
    }
}

void TerminalScreen::leave_typing_mode() {
    if (!typing_) {
        return;
    }
    typing_ = false;
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, nullptr);
        }
    }
    lv_group_delete(typing_group_);
    typing_group_ = nullptr;
    platform::set_text_input_mode(false);

    nav_bar().set_icon(0, view::NAV_BACK, back_cb, this);
    nav_bar().set_enabled(1, true);
    nav_bar().set_enabled(2, true);
    nav_bar().set_enabled(3, true);
    nav_bar().set_enabled(4, true);
}

void TerminalScreen::push_history(const std::string& command) {
    if (!history_.empty() && history_.back() == command) {
        history_index_ = history_.size();
        return;
    }
    if (history_.size() >= kMaxHistorySize) {
        history_.pop_front();
    }
    history_.push_back(command);
    history_index_ = history_.size();
}

void TerminalScreen::recall_history(int delta) {
    if (history_.empty()) {
        return;
    }
    // One-past-last is the drafting slot; stash the draft the first time the
    // user steps back so an unfinished line survives the round trip.
    if (history_index_ == history_.size()) {
        history_draft_ = lv_textarea_get_text(input_ta_);
    }
    const int signed_index = static_cast<int>(history_index_) + delta;
    if (signed_index < 0) {
        history_index_ = 0;
    } else if (signed_index > static_cast<int>(history_.size())) {
        history_index_ = history_.size();
    } else {
        history_index_ = static_cast<size_t>(signed_index);
    }
    lv_textarea_set_text(input_ta_,
                         history_index_ == history_.size() ? history_draft_.c_str()
                                                           : history_[history_index_].c_str());
}

void TerminalScreen::show_sudo_dialog() {
    if (sudo_modal_) {
        return;
    }
    if (typing_) {
        leave_typing_mode();
    }
    platform::set_text_input_mode(true);

    sudo_modal_ = lv_obj_create(root());
    lv_obj_set_size(sudo_modal_, view::kScreenWidth, view::kScreenHeight);
    lv_obj_set_pos(sudo_modal_, 0, 0);
    lv_obj_set_style_bg_color(sudo_modal_, lv_obj_get_style_bg_color(root(), LV_PART_MAIN), 0);
    lv_obj_set_style_bg_opa(sudo_modal_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sudo_modal_, 0, 0);
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
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 78);

    auto* keys = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_12, 0);
    lv_label_set_text(keys, "ESC cancel - Enter OK");
    lv_obj_align(keys, LV_ALIGN_BOTTOM_MID, 0, -8);

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

void TerminalScreen::close_sudo_dialog() {
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
    sudo_ta_    = nullptr;
    platform::set_text_input_mode(false);

    // Restore this screen's normal nav mapping.
    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, scroll_up_cb, this);
    nav_bar().set_icon(2, view::NAV_RUN, run_stop_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, scroll_down_cb, this);
    nav_bar().set_icon(4, "Type", type_cb, this);
}

void TerminalScreen::submit_sudo_dialog() {
    if (!sudo_modal_) {
        return; // Enter can arrive twice (group READY + nav slot 2); one wins
    }
    const std::string password = lv_textarea_get_text(sudo_ta_);
    close_sudo_dialog();
    if (password.empty()) {
        append_log("[sudo: no password entered]\n");
        update_run_label();
        return;
    }
    cached_password_ = password;
    const std::string command = pending_command_;
    const bool as_root        = pending_root_;
    pending_command_.clear();
    pending_root_ = false;
    start_command(command, as_root);
}

void TerminalScreen::on_process_update(void* owner, const std::string& text, bool exited) {
    auto* self = static_cast<TerminalScreen*>(owner);
    if (!self) {
        return;
    }

    self->run_output_ += text;
    self->append_log(text);
    if (exited) {
        // One sudo -S attempt per password: a rejected password must not stay
        // cached, or every later root command fails without ever re-prompting.
        if (self->last_run_used_sudo_password_ &&
            self->run_output_.find("incorrect password attempt") != std::string::npos) {
            self->cached_password_.clear();
            self->append_log("[sudo password rejected - it will be asked again]\n");
        }
        self->run_output_.clear();
        self->last_run_used_sudo_password_ = false;
        self->update_run_label();
    }
}

void TerminalScreen::run_stop_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (!self) {
        return;
    }
    if (self->runner_.running()) {
        self->runner_.stop();
        self->append_log("\n[stopped]\n");
        self->update_run_label();
    } else {
        self->run_command();
    }
}

void TerminalScreen::clear_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->clear_log();
    }
}

void TerminalScreen::scroll_up_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (self && self->log_ta_) {
        self->scroll_log(lv_obj_get_height(self->log_ta_) * 3 / 4);
    }
}

void TerminalScreen::scroll_down_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (self && self->log_ta_) {
        self->scroll_log(-lv_obj_get_height(self->log_ta_) * 3 / 4);
    }
}

void TerminalScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->runner_.stop();
        self->view_model().go_back();
    }
}

void TerminalScreen::type_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (!self) {
        return;
    }
    // Slot 4 enters typing; while typing slot 0 is repointed here so ESC
    // leaves input instead of leaving the screen.
    if (self->typing_) {
        self->leave_typing_mode();
    } else {
        self->enter_typing_mode();
    }
}

void TerminalScreen::input_ready_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->run_command();
    }
}

void TerminalScreen::sudo_submit_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->submit_sudo_dialog();
    }
}

void TerminalScreen::sudo_cancel_cb(lv_event_t* event) {
    auto* self = static_cast<TerminalScreen*>(lv_event_get_user_data(event));
    if (!self) {
        return;
    }
    self->close_sudo_dialog();
    self->pending_command_.clear();
    self->pending_root_ = false;
    self->append_log("[sudo canceled]\n");
    self->update_run_label();
}

} // namespace screen
