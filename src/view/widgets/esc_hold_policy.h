/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Timing of the "Hold ESC 3s to return home" hint, free of LVGL so it can be
 * unit-tested (tests/esc_hold_selfcheck.cpp). The hint only *announces* the
 * launcher's own exit: APPLaunch SIGTERMs the app after ESC is held 3 s
 * (cp0_esc_exit_policy: hint at 500 ms, terminate at 3000 ms). Bruce never
 * quits on its own here.
 *
 * Ticks are milliseconds from a wrapping uint32 clock (lv_tick_get()).
 */

#pragma once

#include <cstdint>

namespace view::widgets {

class EscHoldPolicy {
public:
    // Same delay as the launcher's hint, so a quick Back tap shows nothing.
    static constexpr uint32_t kHintDelayMs = 500;

    // Physical ESC went down. Returns false if a hold is already running
    // (duplicate down events never restart the clock).
    bool press(uint32_t now) {
        if (holding_) {
            return false;
        }
        holding_ = true;
        hint_visible_ = false;
        down_tick_ = now;
        return true;
    }

    // Physical ESC went up. Returns true if the hint was showing (hide it).
    bool release() {
        const bool was_visible = hint_visible_;
        holding_ = false;
        hint_visible_ = false;
        return was_visible;
    }

    // Timer check. Returns true exactly once per hold, when the hint is due.
    bool poll(uint32_t now) {
        if (!holding_ || hint_visible_ || remaining_ms(now) > 0) {
            return false;
        }
        hint_visible_ = true;
        return true;
    }

    // Milliseconds until the hint is due (0 when due or not holding).
    uint32_t remaining_ms(uint32_t now) const {
        if (!holding_) {
            return 0;
        }
        const uint32_t elapsed = now - down_tick_;
        return elapsed >= kHintDelayMs ? 0 : kHintDelayMs - elapsed;
    }

    bool holding() const { return holding_; }
    bool hint_visible() const { return hint_visible_; }

private:
    uint32_t down_tick_{0};
    bool holding_{false};
    bool hint_visible_{false};
};

} // namespace view::widgets
