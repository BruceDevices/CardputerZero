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
    void start_tool(std::vector<std::string> argv, const std::string& stdin_payload);
    void clear_log();
    void append_log(const std::string& text);
    void scroll_log(int32_t delta);
    void update_run_label();

    // On-device sudo password prompt (modal overlay on this screen). Shown
    // when a root tool launches while passwordless sudo is unavailable; on
    // submit the tool starts with `sudo -S` and the password fed via stdin.
    void show_sudo_dialog();
    void close_sudo_dialog();
    void submit_sudo_dialog();

    static void on_process_update(void* owner, const std::string& text, bool exited);
    static void run_stop_cb(lv_event_t* event);
    static void clear_cb(lv_event_t* event);
    static void scroll_up_cb(lv_event_t* event);
    static void scroll_down_cb(lv_event_t* event);
    static void back_cb(lv_event_t* event);
    static void sudo_submit_cb(lv_event_t* event);
    static void sudo_cancel_cb(lv_event_t* event);

    int category_index_{-1};
    int item_index_{-1};
    lv_obj_t* log_ta_{nullptr};
    bool follow_tail_{true}; // auto-scroll to newest output until the user scrolls up
    platform::ProcessRunner runner_;

    lv_obj_t* sudo_modal_{nullptr};
    lv_obj_t* sudo_ta_{nullptr};
    lv_group_t* sudo_group_{nullptr};
};

} // namespace screen
