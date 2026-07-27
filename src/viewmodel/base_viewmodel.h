/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "subjects.h"
#include "base_model.h"

#include "lvgl.h"

namespace viewmodel {

class BaseViewModel {
public:
    BaseViewModel();
    ~BaseViewModel();

    BaseViewModel(const BaseViewModel&) = delete;
    BaseViewModel& operator=(const BaseViewModel&) = delete;

    lv_subject_t* title_subject();
    lv_subject_t* dark_mode_subject();
    lv_subject_t* current_page_subject();
    // Increments on every navigation so ScreenManager always rebuilds - even
    // when the AppPage is unchanged (e.g. root menu -> a category, both Menu).
    lv_subject_t* nav_token_subject();
    lv_subject_t* quit_requested_subject();

    bool is_dark_mode() const;
    void toggle_dark_mode();

    model::AppPage current_page() const;
    int active_category() const;
    int active_item() const;

    // Root menu (categories) when index == -1, otherwise the category's tool list.
    void show_menu(int category_index);
    void show_tool_run(int category_index, int item_index);
    void show_about();
    void go_back();
    void request_quit();

private:
    void publish_all();

    model::BaseModel model_;
    reactive::StringSubject<32> title_subject_;
    reactive::BoolSubject dark_mode_subject_;
    reactive::IntSubject current_page_subject_;
    reactive::IntSubject nav_token_subject_;
    reactive::BoolSubject quit_requested_subject_;
};

} // namespace viewmodel
