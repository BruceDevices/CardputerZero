/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the "Hold ESC 3s to return home" hint timing
 * (src/view/widgets/esc_hold_policy.h). Not part of the CMake build. Run
 * manually (Linux):
 *
 *   g++ -std=c++17 -Isrc/view/widgets tests/esc_hold_selfcheck.cpp \
 *       -o /tmp/esc_hold_check && /tmp/esc_hold_check
 *
 * Covers: no hint for a short tap, the hint exactly once at 500 ms, hide on
 * release, duplicate downs not restarting the clock, a bounce restarting the
 * hold, and the uint32 tick wrap.
 */

#include "esc_hold_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using view::widgets::EscHoldPolicy;

void short_tap_shows_nothing() {
    EscHoldPolicy p;
    check(p.press(1000), "tap: press starts a hold");
    check(p.holding(), "tap: holding");
    check(p.remaining_ms(1000) == 500, "tap: 500 ms to go at press");
    check(!p.poll(1200), "tap: no hint at 200 ms");
    check(!p.poll(1499), "tap: no hint at 499 ms");
    check(!p.release(), "tap: release has nothing to hide");
    check(!p.holding(), "tap: not holding after release");
    check(!p.poll(5000), "tap: no hint after release");
}

void hold_shows_once_and_hides_on_release() {
    EscHoldPolicy p;
    p.press(0);
    check(p.remaining_ms(100) == 400, "hold: 400 ms left at 100 ms");
    check(p.poll(500), "hold: hint due at exactly 500 ms");
    check(p.hint_visible(), "hold: hint visible");
    check(p.remaining_ms(600) == 0, "hold: nothing left once due");
    check(!p.poll(600), "hold: hint reported once");
    check(!p.poll(3000), "hold: still once at 3 s (launcher exits, not us)");
    check(p.release(), "hold: release hides the hint");
    check(!p.hint_visible(), "hold: hint gone after release");
}

void late_timer_still_shows() {
    EscHoldPolicy p;
    p.press(10);
    check(p.poll(900), "late: hint shown when the timer runs late");
}

void duplicate_down_keeps_clock() {
    EscHoldPolicy p;
    check(p.press(0), "dup: first press");
    check(!p.press(300), "dup: second down ignored");
    check(p.poll(500), "dup: hint still due 500 ms after the first down");
}

void bounce_restarts_hold() {
    EscHoldPolicy p;
    p.press(0);
    check(!p.poll(400), "bounce: no hint at 400 ms");
    check(!p.release(), "bounce: release before hint");
    check(p.press(420), "bounce: new hold");
    check(!p.poll(600), "bounce: clock restarted (180 ms in)");
    check(p.poll(920), "bounce: hint 500 ms after the new down");
}

void second_hold_after_release() {
    EscHoldPolicy p;
    p.press(0);
    check(p.poll(500), "again: first hint");
    check(p.release(), "again: hidden");
    check(p.press(2000), "again: second hold");
    check(!p.poll(2100), "again: hint not carried over");
    check(p.poll(2500), "again: second hint at 500 ms");
}

void tick_wraparound() {
    EscHoldPolicy p;
    const uint32_t near_max = UINT32_MAX - 100;
    p.press(near_max);
    check(p.remaining_ms(near_max + 200) == 300, "wrap: remaining across overflow");
    check(!p.poll(near_max + 200), "wrap: not due 200 ms in");
    check(p.poll(near_max + 500), "wrap: due 500 ms in across overflow");
}

void idle_policy_is_inert() {
    EscHoldPolicy p;
    check(!p.holding(), "idle: not holding");
    check(p.remaining_ms(123) == 0, "idle: nothing scheduled");
    check(!p.poll(123), "idle: never shows");
    check(!p.release(), "idle: stray release is harmless");
}

} // namespace

int main() {
    short_tap_shows_nothing();
    hold_shows_once_and_hides_on_release();
    late_timer_still_shows();
    duplicate_down_keeps_clock();
    bounce_restarts_hold();
    second_hold_after_release();
    tick_wraparound();
    idle_policy_is_inert();

    std::printf("esc_hold_selfcheck: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
