/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "process_runner.h"

#include <deque>
#include <string>

namespace screen {

// Interactive shell behind the catalog's "Terminal" rows: a command input line
// plus a tail-following output log. Commands typed with a leading "sudo " run
// as root through model::terminal_command_argv (the sudoers-scoped `--root-sh`
// passthrough, or `sudo -S` with an on-device password prompt when no rule is
// installed - the password is cached for the screen's lifetime, never logged).
// Every command runs in a fresh `sh -c`, like Bruce's serial console one-shots;
// cd/env do not persist between runs.
class TerminalScreen : public BaseScreen {
public:
    TerminalScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~TerminalScreen() override;

private:
    void build_content(lv_obj_t* content) override;

    void run_command();
    void start_command(const std::string& command, bool as_root);
    void clear_log();
    void append_log(const std::string& text);
    void scroll_log(int32_t delta);
    void update_run_label();

    // Typing mode: full keyboard reaches the input line (letters/digits arrive
    // via the indev group), nav keys yield; ESC re-enters nav mode via slot 0.
    void enter_typing_mode();
    void leave_typing_mode();

    // Command history (Up = older, Down = newer while typing).
    void push_history(const std::string& command);
    void recall_history(int delta);

    // On-device sudo password prompt (same overlay as ToolRunScreen). Shown
    // when a root command needs a password that is not cached yet; on submit
    // the command starts with `sudo -S` and the password fed via stdin.
    void show_sudo_dialog();
    void close_sudo_dialog();
    void submit_sudo_dialog();

    static void on_process_update(void* owner, const std::string& text, bool exited);
    static void run_stop_cb(lv_event_t* event);
    static void clear_cb(lv_event_t* event);
    static void scroll_up_cb(lv_event_t* event);
    static void scroll_down_cb(lv_event_t* event);
    static void back_cb(lv_event_t* event);
    static void type_cb(lv_event_t* event);
    static void input_ready_cb(lv_event_t* event);
    static void sudo_submit_cb(lv_event_t* event);
    static void sudo_cancel_cb(lv_event_t* event);

    int category_index_{-1};
    int item_index_{-1};
    lv_obj_t* log_ta_{nullptr};
    lv_obj_t* input_ta_{nullptr};
    bool follow_tail_{true}; // auto-scroll to newest output until the user scrolls up
    platform::ProcessRunner runner_;

    std::deque<std::string> history_;
    size_t history_index_{0}; // one-past-last = drafting a new line
    std::string history_draft_;

    std::string cached_password_; // sudo password for this screen's lifetime
    std::string pending_command_; // root command waiting for the password dialog
    bool pending_root_{false};
    bool last_run_used_sudo_password_{false}; // to sniff a wrong-password failure
    std::string run_output_; // whole output of the current run (sudo -S errors can straddle chunks)

    lv_obj_t* sudo_modal_{nullptr};
    lv_obj_t* sudo_ta_{nullptr};
    lv_group_t* sudo_group_{nullptr};
    bool typing_{false};
    lv_group_t* typing_group_{nullptr};
};

} // namespace screen
