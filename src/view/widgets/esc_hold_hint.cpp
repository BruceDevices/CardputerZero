/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "esc_hold_hint.h"

#include "linux_input.h"

namespace view::widgets {
namespace {

// Launcher toast metrics (APPLaunch launcher_toast.cpp).
constexpr int32_t kWidth = 280;
constexpr int32_t kHeight = 22;
constexpr int32_t kTopOffset = 4;
constexpr uint32_t kBackgroundColor = 0x1F3A5F;
constexpr uint32_t kTextColor = 0xFFFFFF;
constexpr const char* kText = "Hold ESC 3s to return home";

// While a hold is active (and only then) the timer re-checks the physical key
// at this rate, so a lost release can't leave the banner up (cf. the
// launcher's EscHoldLifecycleModel::poll hardware check).
constexpr uint32_t kVerifyPeriodMs = 250;

} // namespace

EscHoldHint::EscHoldHint(lv_obj_t* parent) {
    if (parent) {
        box_ = lv_obj_create(parent);
    }
    if (box_) {
        lv_obj_remove_style_all(box_);
        lv_obj_set_size(box_, kWidth, kHeight);
        lv_obj_align(box_, LV_ALIGN_TOP_MID, 0, kTopOffset);
        lv_obj_set_style_bg_color(box_, lv_color_hex(kBackgroundColor), 0);
        lv_obj_set_style_bg_opa(box_, LV_OPA_80, 0);
        lv_obj_set_style_radius(box_, 6, 0);
        lv_obj_remove_flag(box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(box_, LV_OBJ_FLAG_CLICKABLE);

        auto* label = lv_label_create(box_);
        lv_label_set_text(label, kText);
        lv_obj_set_style_text_color(label, lv_color_hex(kTextColor), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(label, kWidth - 12);
        lv_obj_center(label);

        lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
    }
    platform::set_esc_key_listener(esc_key_listener, this);
}

EscHoldHint::~EscHoldHint() {
    platform::clear_esc_key_listener(esc_key_listener, this);
    stop_timer();
    // The banner lives exactly as long as its controller.
    if (box_ && lv_obj_is_valid(box_)) {
        lv_obj_delete(box_);
    }
    box_ = nullptr;
}

void EscHoldHint::on_esc(bool down) {
    if (down) {
        if (policy_.press(lv_tick_get())) {
            arm_timer(EscHoldPolicy::kHintDelayMs);
        }
        return;
    }
    stop_timer();
    if (policy_.release()) {
        hide();
    }
}

void EscHoldHint::arm_timer(uint32_t delay_ms) {
    stop_timer();
    // Repeats until the hold ends (release deletes it); timer_cb picks each
    // next period. No timer exists while ESC is up.
    timer_ = lv_timer_create(timer_cb, delay_ms, this);
}

void EscHoldHint::stop_timer() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void EscHoldHint::show() {
    if (box_ && lv_obj_is_valid(box_)) {
        lv_obj_move_foreground(box_);
        lv_obj_remove_flag(box_, LV_OBJ_FLAG_HIDDEN);
    }
}

void EscHoldHint::hide() {
    if (box_ && lv_obj_is_valid(box_)) {
        lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
    }
}

void EscHoldHint::esc_key_listener(bool down, void* user_data) {
    auto* hint = static_cast<EscHoldHint*>(user_data);
    if (hint) {
        hint->on_esc(down);
    }
}

void EscHoldHint::timer_cb(lv_timer_t* timer) {
    auto* hint = static_cast<EscHoldHint*>(lv_timer_get_user_data(timer));
    if (!hint || hint->timer_ != timer) {
        return;
    }
    // Is ESC really still down? A stale hold is released through the
    // listener (on_esc(false)), which deletes this timer.
    platform::reconcile_esc_key();
    if (hint->timer_ != timer || !hint->policy_.holding()) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if (hint->policy_.poll(now)) {
        hint->show();
    }
    const uint32_t remaining = hint->policy_.remaining_ms(now);
    lv_timer_set_period(timer, hint->policy_.hint_visible() || remaining == 0 ? kVerifyPeriodMs
                                                                              : remaining);
}

} // namespace view::widgets
