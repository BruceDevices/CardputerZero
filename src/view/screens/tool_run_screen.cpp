/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "tool_run_screen.h"

#include "asset_manager.h"
#include "theme.h"
#include "tool_catalog.h"
#include "ui_const.h"

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

    log_ta_ = lv_textarea_create(content);
    lv_obj_set_size(log_ta_,
                    LV_PCT(100),
                    view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(log_ta_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_textarea_set_cursor_click_pos(log_ta_, false);
    lv_obj_clear_flag(log_ta_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(log_ta_, &lv_font_montserrat_12, 0);
    lv_textarea_set_text(log_ta_, action.description);
    lv_textarea_add_text(log_ta_, "\n\n");

    nav_bar().set_icon(2, view::NAV_RUN, run_stop_cb, this);
    nav_bar().set_icon(3, view::NAV_CLEAR, clear_cb, this);
    configure_common_nav(view::NAV_BACK, back_cb);
}

void ToolRunScreen::toggle_run() {
    if (runner_.running()) {
        runner_.stop();
        append_log("\n[stopped]\n");
    }
    else {
        const auto& action = model::tool_catalog()[static_cast<size_t>(category_index_)]
                                 .actions[static_cast<size_t>(item_index_)];
        if (!runner_.start(action.argv, &ToolRunScreen::on_process_update, this)) {
            append_log("\n[failed to launch]\n");
        }
    }

    update_run_label();
}

void ToolRunScreen::clear_log() {
    if (log_ta_) {
        lv_textarea_set_text(log_ta_, "");
    }
}

void ToolRunScreen::append_log(const std::string& text) {
    if (!log_ta_ || text.empty()) {
        return;
    }

    if (lv_textarea_get_text(log_ta_)[0] != '\0' && strlen(lv_textarea_get_text(log_ta_)) > kMaxLogChars) {
        lv_textarea_set_text(log_ta_, "");
    }

    lv_textarea_add_text(log_ta_, text.c_str());
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

void ToolRunScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<ToolRunScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->runner_.stop();
        self->view_model().go_back();
    }
}

} // namespace screen
