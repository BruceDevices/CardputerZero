/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * "Hold ESC 3s to return home" banner. Display only: APPLaunch itself ends the
 * app after a 3 s ESC hold; this just tells the user, like the launcher's own
 * pages do. Styled after the launcher toast (launcher_toast.cpp): top-centre,
 * 280x22, navy 0x1F3A5F at 80 %, white 12 px text.
 *
 * Driven by the physical ESC key only (platform::set_esc_key_listener), never
 * by Backspace/grave, which also mean Back but don't trigger the launcher.
 * A timer exists only while ESC is held (hint delay, then a slow check that
 * the key is really still down), so the idle loop is unaffected. Back on a
 * short press is untouched (the nav router still fires it on key down).
 */

#pragma once

#include "esc_hold_policy.h"
#include "lvgl.h"

namespace view::widgets {

class EscHoldHint {
public:
    // Parent should be lv_layer_top() so the banner floats over every screen,
    // popups and run screens included.
    explicit EscHoldHint(lv_obj_t* parent);
    ~EscHoldHint();

    EscHoldHint(const EscHoldHint&) = delete;
    EscHoldHint& operator=(const EscHoldHint&) = delete;

private:
    void on_esc(bool down);
    void arm_timer(uint32_t delay_ms);
    void stop_timer();
    void show();
    void hide();

    static void esc_key_listener(bool down, void* user_data);
    static void timer_cb(lv_timer_t* timer);

    EscHoldPolicy policy_;
    lv_obj_t* box_{nullptr};
    lv_timer_t* timer_{nullptr};
};

} // namespace view::widgets
