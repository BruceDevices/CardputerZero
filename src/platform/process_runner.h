/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "lvgl.h"

namespace platform {

// Runs one external command (curl, nc, responder, smbclient, ...) as a child
// process and streams its stdout/stderr back to the caller. This is the
// entire "port" of Bruce's network tools to CardputerZero: instead of
// reimplementing each protocol in C++, embedded Linux already ships the real
// binaries, so ProcessRunner just launches and supervises them.
//
// Threading model: a background reader thread only ever touches a
// mutex-protected buffer - it never calls LVGL (which is not thread-safe). An
// lv_timer created on the LVGL thread polls that buffer and invokes the owner
// callback, so callers can touch LVGL objects directly without their own
// locking.
class ProcessRunner {
public:
    // Called on the LVGL thread with newly captured text (may be empty) and
    // whether the process has exited.
    using UpdateCallback = void (*)(void* owner, const std::string& text, bool exited);

    ProcessRunner();
    ~ProcessRunner();

    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    // Starts argv[0] with the remaining entries as arguments. Must be called
    // from the LVGL thread. Returns false if a process is already running or
    // fork/pipe setup failed. When `stdin_payload` is non-empty it is written
    // to the child's stdin and the pipe is closed right away - that is how the
    // sudo password reaches `sudo -S` (via pipe, never argv, never logged).
    bool start(const std::vector<std::string>& argv, UpdateCallback on_update, void* owner,
               const std::string& stdin_payload = {});

    // Sends SIGTERM to the process group and waits for the reader thread to
    // finish. Safe to call when nothing is running. Must be called from the
    // LVGL thread.
    void stop();

    bool running() const;

private:
    static void poll_timer_cb(lv_timer_t* timer);
    void reader_thread_main();
    void join_reader();

    std::mutex mutex_;
    std::string pending_text_;
    bool pending_exit_ = false;

    std::thread reader_thread_;
    UpdateCallback on_update_ = nullptr;
    void* owner_ = nullptr;
    lv_timer_t* poll_timer_ = nullptr;
    std::atomic<pid_t> child_pid_{-1};
    int read_fd_ = -1;
    std::atomic<bool> running_{false};
};

} // namespace platform
