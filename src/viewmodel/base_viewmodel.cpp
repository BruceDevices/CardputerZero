/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "base_viewmodel.h"

namespace viewmodel {
namespace {

int page_to_int(model::AppPage page) {
    return static_cast<int>(page);
}

} // namespace

BaseViewModel::BaseViewModel()
    : title_subject_(model_.app_title()),
      dark_mode_subject_(model_.dark_mode()),
      current_page_subject_(page_to_int(model_.current_page())),
      nav_token_subject_(0),
      quit_requested_subject_(false) {}

BaseViewModel::~BaseViewModel() = default;

lv_subject_t* BaseViewModel::title_subject() {
    return title_subject_.native();
}

lv_subject_t* BaseViewModel::dark_mode_subject() {
    return dark_mode_subject_.native();
}

lv_subject_t* BaseViewModel::current_page_subject() {
    return current_page_subject_.native();
}

lv_subject_t* BaseViewModel::nav_token_subject() {
    return nav_token_subject_.native();
}

lv_subject_t* BaseViewModel::quit_requested_subject() {
    return quit_requested_subject_.native();
}

bool BaseViewModel::is_dark_mode() const {
    return model_.dark_mode();
}

void BaseViewModel::toggle_dark_mode() {
    model_.toggle_dark_mode();
    dark_mode_subject_.set(model_.dark_mode());
}

model::AppPage BaseViewModel::current_page() const {
    return model_.current_page();
}

int BaseViewModel::active_category() const {
    return model_.active_category();
}

int BaseViewModel::active_item() const {
    return model_.active_item();
}

void BaseViewModel::show_menu(int category_index) {
    model_.set_current_page(model::AppPage::Menu);
    model_.set_active_category(category_index);
    model_.set_active_item(-1);
    publish_all();
}

void BaseViewModel::show_tool_run(int category_index, int item_index) {
    model_.set_current_page(model::AppPage::ToolRun);
    model_.set_active_category(category_index);
    model_.set_active_item(item_index);
    publish_all();
}

void BaseViewModel::show_about() {
    model_.set_current_page(model::AppPage::About);
    publish_all();
}

void BaseViewModel::go_back() {
    switch (model_.current_page()) {
        case model::AppPage::ToolRun:
            show_menu(model_.active_category());
            break;
        case model::AppPage::About:
            show_menu(-1);
            break;
        case model::AppPage::Menu:
            if (model_.active_category() != -1) {
                show_menu(-1);
            }
            break;
    }
}

void BaseViewModel::request_quit() {
    quit_requested_subject_.set(true);
}

void BaseViewModel::publish_all() {
    current_page_subject_.set(page_to_int(model_.current_page()));
    nav_token_subject_.set(nav_token_subject_.value() + 1);
}

} // namespace viewmodel
