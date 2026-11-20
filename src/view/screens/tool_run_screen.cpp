/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "tool_run_screen.h"

#include "asset_manager.h"
#include "linux_input.h"
#include "theme.h"
#include "tool_catalog.h"
#include "ui_const.h"

#include <cstdint>
#include <cstring>

namespace screen {
namespace {

constexpr int32_t kHeaderHeight = 20;
constexpr size_t kMaxLogChars   = 6000;

} // namespace

ToolRunScreen::ToolRunScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

ToolRunScreen::~ToolRunScreen() {
    runner_.stop();
}

void ToolRunScreen::build_content(lv_obj_t* content) {
    category_index_ = view_model().active_category();
    item_index_     = view_model().active_item();
    const auto& action = model::tool_catalog()[static_cast<size_t>(category_index_)]
                             .actions[static_cast<size_t>(item_index_)];

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_label_set_text(header, action.label);

    // Plain label in a scrollable box: a textarea snaps back to its cursor (always
    // at the end) on every size/style change and fights manual scrolling. Scrolling
    // up pauses tail-follow so new output cannot yank the view back down.
    log_ta_ = lv_obj_create(content);
    lv_obj_set_size(log_ta_,
                    LV_PCT(100),
                    view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(log_ta_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(log_ta_, 4, 0);
    lv_obj_add_flag(log_ta_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(log_ta_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(log_ta_, LV_SCROLLBAR_MODE_AUTO);

    log_label_ = lv_label_create(log_ta_);
    lv_obj_set_width(log_label_, LV_PCT(100));
    lv_label_set_long_mode(log_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(log_label_, &lv_font_montserrat_12, 0);
    if (action.auto_run) {
        // Autonomous rows run on entry: show what is executing until output
        // starts arriving, instead of the idle description + a Run press.
        const std::string banner =
            "Running command: \"" + model::action_command_text(action) + "\", wait...\n\n";
        lv_label_set_text(log_label_, banner.c_str());
    } else {
        lv_label_set_text(log_label_, action.description);
        lv_label_ins_text(log_label_, LV_LABEL_POS_LAST, "\n\n");
    }

    // 4=Back  5=Log up  6=Run/Stop  7=Log down  8=Clear
    nav_bar().set_icon(1, view::NAV_UP, scroll_up_cb, this);
    nav_bar().set_icon(2, view::NAV_RUN, run_stop_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, scroll_down_cb, this);
    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(4, view::NAV_CLEAR, clear_cb, this); // common nav sets Theme; Clear wins here

    // Autonomous rows start immediately (sudo prompt still appears if needed).
    if (action.auto_run && !runner_.running()) {
        toggle_run();
    }
}

void ToolRunScreen::toggle_run() {
    if (runner_.running()) {
        runner_.stop();
        append_log("\n[stopped]\n");
        update_run_label();
        return;
    }

    const auto& action = model::tool_catalog()[static_cast<size_t>(category_index_)]
                             .actions[static_cast<size_t>(item_index_)];
    // Root tool while passwordless sudo does not work (sudoers rule missing):
    // ask for the sudo password on-device instead of failing with
    // "sudo: a password is required" in the log.
    if (model::action_needs_root(action) && !model::sudo_passwordless()) {
        show_sudo_dialog();
        return;
    }
    start_tool(model::launch_argv(action), "");
}

void ToolRunScreen::start_tool(std::vector<std::string> argv, const std::string& stdin_payload) {
    follow_tail_ = true;
    if (!runner_.start(std::move(argv), &ToolRunScreen::on_process_update, this, stdin_payload)) {
        append_log("\n[failed to launch]\n");
    }
    update_run_label();
}

void ToolRunScreen::show_sudo_dialog() {
    if (sudo_modal_) {
        return;
    }
    platform::set_text_input_mode(true);

    // Full-screen overlay: hides title/log/nav while typing so no stale nav
    // icons suggest the wrong button mapping (slots are re-pointed below).
    sudo_modal_ = lv_obj_create(root());
    lv_obj_set_size(sudo_modal_, view::kScreenWidth, view::kScreenHeight);
    lv_obj_set_pos(sudo_modal_, 0, 0);
    lv_obj_set_style_bg_color(sudo_modal_, lv_obj_get_style_bg_color(root(), LV_PART_MAIN), 0);
    lv_obj_set_style_bg_opa(sudo_modal_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sudo_modal_, 0, 0);
    lv_obj_set_style_pad_all(sudo_modal_, 0, 0); // theme padding would shift every row below
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
    lv_obj_add_event_cb(sudo_ta_, sudo_submit_cb, LV_EVENT_READY, this); // Enter in one-line field

    auto* hint = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_label_set_text(hint, "device default is often 'pi'");
    // Stack each row under the previous one so they can never overlap.
    lv_obj_align_to(hint, sudo_ta_, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    auto* keys = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_12, 0);
    lv_label_set_text(keys, "ESC cancel - Enter OK");
    lv_obj_align_to(keys, hint, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    // Re-point the nav slots: ESC = cancel, Enter/slot 2 = submit. Typing keys
    // reach the textarea through the indev group (see platform::set_text_input_mode).
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

void ToolRunScreen::close_sudo_dialog() {
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

    // Restore this screen's normal nav mapping.
    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, scroll_up_cb, this);
    nav_bar().set_icon(2, view::NAV_RUN, run_stop_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, scroll_down_cb, this);
    nav_bar().set_icon(4, view::NAV_CLEAR, clear_cb, this);
}

void ToolRunScreen::submit_sudo_dialog() {
    if (!sudo_modal_) {
        return; // Enter can arrive twice (group READY + nav slot 2); one wins
    }
    const std::string password = lv_textarea_get_text(sudo_ta_);
    if (password.empty()) {
        // The Enter that pressed Run can also reach this dialog right after it
        // opens (press + release / double dispatch). Ignore an empty submit and
        // keep the dialog up; ESC cancels.
        return;
    }
    close_sudo_dialog();
    const auto& action = model::tool_catalog()[static_cast<size_t>(category_index_)]
                             .actions[static_cast<size_t>(item_index_)];
    // The password travels through the child's stdin pipe (ProcessRunner),
    // never in argv and never into the log. sudo -S makes one attempt, so a
    // wrong password shows "1 incorrect password attempt" in the log.
    start_tool(model::sudo_argv_with_password(action), password + "\n");
}

void ToolRunScreen::clear_log() {
    if (log_label_) {
        lv_label_set_text(log_label_, "");
        follow_tail_ = true;
    }
}

void ToolRunScreen::append_log(const std::string& text) {
    if (!log_label_ || text.empty()) {
        return;
    }

    // Trim from the front (on a line boundary) so scrollback survives overflow.
    std::string current = lv_label_get_text(log_label_);
    if (current.size() + text.size() > kMaxLogChars) {
        size_t cut = current.size() + text.size() - kMaxLogChars + kMaxLogChars / 4;
        if (cut >= current.size()) {
            current.clear();
        } else {
            const size_t nl = current.find('\n', cut);
            current.erase(0, nl == std::string::npos ? cut : nl + 1);
        }
        lv_label_set_text(log_label_, current.c_str());
    }

    lv_obj_update_layout(log_ta_);
    const lv_coord_t prev_scroll = lv_obj_get_scroll_y(log_ta_);
    lv_label_ins_text(log_label_, LV_LABEL_POS_LAST, text.c_str());
    lv_obj_update_layout(log_ta_);
    if (follow_tail_) {
        lv_obj_scroll_to_y(log_ta_, lv_obj_get_scroll_y(log_ta_) + lv_obj_get_scroll_bottom(log_ta_),
                           LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(log_ta_, prev_scroll, LV_ANIM_OFF);
    }
}

void ToolRunScreen::scroll_log(int32_t delta) {
    if (!log_ta_) {
        return;
    }
    lv_obj_update_layout(log_ta_);
    // delta > 0 reveals older output (content moves down); clamp to the real limits.
    const lv_coord_t max_y = lv_obj_get_scroll_y(log_ta_) + lv_obj_get_scroll_bottom(log_ta_);
    lv_coord_t target      = lv_obj_get_scroll_y(log_ta_) - delta;
    target                 = target < 0 ? 0 : (target > max_y ? max_y : target);
    lv_obj_scroll_to_y(log_ta_, target, LV_ANIM_OFF);
    follow_tail_ = target >= max_y - 2;
}

void ToolRunScreen::update_run_label() {
    nav_bar().set_text(2, runner_.running() ? view::NAV_STOP : view::NAV_RUN);
}

void ToolRunScreen::on_process_update(void* owner, const std::string& text, bool exited) {
    auto* self = static_cast<ToolRunScreen*>(owner);
    if (!self) {
        return;
    }

    self->append_log(text);
    if (exited) {
        self->append_log("\n[exited]\n");
        self->update_run_label();
    }
}

void ToolRunScreen::run_stop_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->toggle_run();
    }
}

void ToolRunScreen::clear_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->clear_log();
    }
}

void ToolRunScreen::scroll_up_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self && self->log_ta_) {
        self->scroll_log(lv_obj_get_height(self->log_ta_) * 3 / 4); // toward older output
    }
}

void ToolRunScreen::scroll_down_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self && self->log_ta_) {
        self->scroll_log(-lv_obj_get_height(self->log_ta_) * 3 / 4); // toward newest output
    }
}

void ToolRunScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->runner_.stop();
        self->view_model().go_back();
    }
}

void ToolRunScreen::sudo_submit_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->submit_sudo_dialog();
    }
}

void ToolRunScreen::sudo_cancel_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->close_sudo_dialog();
    }
}

} // namespace screen
