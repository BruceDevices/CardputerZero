/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Minimal stand-in for the LVGL timer API, for tests that compile
 * src/platform/process_runner.cpp without LVGL (tests/process_runner_selfcheck.cpp).
 * Timers never fire on their own: the test calls lv_test_fire_timers(), so it
 * decides exactly when "the next poll" happens.
 */

#pragma once

#include <cstdint>
#include <vector>

struct lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t*);

struct lv_timer_t {
    lv_timer_cb_t cb;
    uint32_t period;
    void* user_data;
    bool deleted;
};

inline std::vector<lv_timer_t*>& lv_test_timers() {
    static std::vector<lv_timer_t*> timers;
    return timers;
}

inline lv_timer_t* lv_timer_create(lv_timer_cb_t cb, uint32_t period, void* user_data) {
    auto* t = new lv_timer_t{cb, period, user_data, false};
    lv_test_timers().push_back(t);
    return t;
}

inline void lv_timer_delete(lv_timer_t* t) {
    auto& v = lv_test_timers();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == t) {
            v.erase(v.begin() + static_cast<long>(i));
            break;
        }
    }
    delete t;
}

inline void* lv_timer_get_user_data(lv_timer_t* t) { return t->user_data; }

// Run every live timer once ("the next poll"); returns how many ran.
inline size_t lv_test_fire_timers() {
    auto copy = lv_test_timers();
    for (auto* t : copy) {
        t->cb(t);
    }
    return copy.size();
}
