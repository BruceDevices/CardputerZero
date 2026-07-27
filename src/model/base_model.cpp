/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "base_model.h"

namespace model {

const char* BaseModel::app_title() const {
    return "Bruce CardputerZero";
}

bool BaseModel::dark_mode() const {
    return dark_mode_;
}

void BaseModel::set_dark_mode(bool enabled) {
    dark_mode_ = enabled;
}

void BaseModel::toggle_dark_mode() {
    dark_mode_ = !dark_mode_;
}

AppPage BaseModel::current_page() const {
    return current_page_;
}

void BaseModel::set_current_page(AppPage page) {
    current_page_ = page;
}

int BaseModel::active_category() const {
    return active_category_;
}

void BaseModel::set_active_category(int index) {
    active_category_ = index;
}

int BaseModel::active_item() const {
    return active_item_;
}

void BaseModel::set_active_item(int index) {
    active_item_ = index;
}

} // namespace model
