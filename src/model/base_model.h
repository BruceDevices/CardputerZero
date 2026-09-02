/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

namespace model {

enum class AppPage {
    Menu    = 0,
    ToolRun = 1,
    About   = 2,
    Config  = 3,
};

class BaseModel {
public:
    const char* app_title() const;

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    AppPage current_page() const;
    void set_current_page(AppPage page);

    int active_category() const;
    void set_active_category(int index);

    int active_item() const;
    void set_active_item(int index);

private:
    bool dark_mode_ = true;
    AppPage current_page_ = AppPage::Menu;
    int active_category_ = -1;
    int active_item_ = -1;
};

} // namespace model
