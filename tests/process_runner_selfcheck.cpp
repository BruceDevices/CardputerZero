/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The real ProcessRunner (with tests/support/lvgl.h for the timer API) and
 * the Wardriving screen's Back decision (platform::wardrive::
 * storage_error_after_back): a child that already exited, whose final
 * "storage" frame and end marker were not delivered by a poll yet, must not
 * lose that error when Back comes first. Not part of the CMake build. Run
 * from the repo root:
 *
 *   g++ -std=c++17 -pthread -Itests/support -Isrc/platform tests/process_runner_selfcheck.cpp \
 *       src/platform/process_runner.cpp src/platform/wardrive.cpp src/platform/ble_scan_parser.cpp \
 *       src/platform/gps_session.cpp src/platform/gps_parser.cpp src/platform/gps_port.cpp \
 *       src/platform/gps_tracker.cpp src/platform/gpx_writer.cpp src/platform/uart_speed.cpp \
 *       -o /tmp/process_runner_check && /tmp/process_runner_check
 */

#include "process_runner.h"
#include "wardrive.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
int g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

struct Screen {
    std::string delivered;
    bool exited = false;
    int callbacks = 0;
};

void on_update(void* owner, const std::string& text, bool exited) {
    auto* s = static_cast<Screen*>(owner);
    s->delivered += text;
    s->exited = s->exited || exited;
    ++s->callbacks;
}

const char* kStorageEnd = "printf '@F Wardriving\\n@1 Scan done. WiFi: 10\\n@P W Failed to write file\\n@G 0\\n@.\\n@X storage\\n'";

bool wait_exit(platform::ProcessRunner& r) {
    for (int i = 0; i < 200 && r.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !r.running();
}
} // namespace

int main() {
    namespace wd = platform::wardrive;

    // 1. exit -> Back -> next poll: the child wrote its storage error and
    //    exited, the poll timer has not run yet. The old guard (child still
    //    running?) is already false here; the screen's own state (result not
    //    handled) must decide, and the output must be drained after stop().
    {
        platform::ProcessRunner r;
        Screen screen;
        CHECK(r.start({"sh", "-c", kStorageEnd}, on_update, &screen));
        CHECK(wait_exit(r));          // exited ...
        CHECK(!r.running());          // ... so a liveness guard would skip the drain
        CHECK(screen.callbacks == 0); // nothing delivered yet
        const bool undelivered = true; // the screen is still in its running state
        r.stop();                      // Back
        CHECK(lv_test_timers().empty()); // no poll will come any more
        CHECK(lv_test_fire_timers() == 0 && screen.callbacks == 0);
        wd::FrameDecoder decoder;
        CHECK(undelivered && wd::storage_error_after_back(decoder, r.take_pending_output()));
        CHECK(decoder.frame().popup_text == "Failed to write file" && decoder.popup_waits());
        CHECK(r.take_pending_output().empty()); // returned once
    }

    // 2. Back while the child runs: it reports the storage error on SIGTERM.
    {
        platform::ProcessRunner r;
        Screen screen;
        CHECK(r.start({"sh", "-c",
                       "trap 'printf \"@F Wardriving\\n@P W Failed to write file\\n@.\\n@X storage\\n\"; exit 1' TERM; "
                       "printf '@F Wardriving\\n@.\\n'; while :; do sleep 0.05; done"},
                      on_update, &screen));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(r.running());
        r.stop();
        wd::FrameDecoder decoder;
        CHECK(wd::storage_error_after_back(decoder, r.take_pending_output()));
    }

    // 3. Normal end delivered by a poll: the screen handled it, so a later
    //    Back (OK) has nothing undelivered; a stopped run is no storage error.
    {
        platform::ProcessRunner r;
        Screen screen;
        CHECK(r.start({"sh", "-c", "printf '@F Wardriving\\n@.\\n@X stopped\\n'"}, on_update, &screen));
        CHECK(wait_exit(r));
        CHECK(lv_test_fire_timers() == 1);
        CHECK(screen.exited && screen.delivered.find("@X stopped") != std::string::npos);
        CHECK(lv_test_timers().empty()); // ProcessRunner removed its timer after the exit
        r.stop();
        CHECK(r.take_pending_output().empty());
        wd::FrameDecoder decoder;
        CHECK(!wd::storage_error_after_back(decoder, screen.delivered));
    }

    std::printf("process_runner self-check OK (%d checks)\n", g_checks);
    return 0;
}
