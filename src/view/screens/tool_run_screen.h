/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "process_runner.h"

#include <string>

namespace screen {

// Launches one model::ToolAction's argv through platform::ProcessRunner and
// streams stdout/stderr into a scrollable log. This is the screen behind
// every curl/nc/responder/smbclient/iw/... entry in the catalog.
class ToolRunScreen : public BaseScreen {
public:
    ToolRunScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~ToolRunScreen() override;

private:
    void build_content(lv_obj_t* content) override;
    void toggle_run();
    void clear_log();
    void append_log(const std::string& text);
    void update_run_label();

    static void on_process_update(void* owner, const std::string& text, bool exited);
    static void run_stop_cb(lv_event_t* event);
    static void clear_cb(lv_event_t* event);
    static void back_cb(lv_event_t* event);

    int category_index_{-1};
    int item_index_{-1};
    lv_obj_t* log_ta_{nullptr};
    platform::ProcessRunner runner_;
};

} // namespace screen
