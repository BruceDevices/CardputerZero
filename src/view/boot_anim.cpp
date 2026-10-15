/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "boot_anim.h"

#include "asset_manager.h"
#include "assets/bruce_img.h"
#include "theme.h"

namespace view {
namespace {

void scale_cb(void* obj, int32_t v) {
    lv_image_set_scale(static_cast<lv_obj_t*>(obj), static_cast<uint16_t>(v));
}

void opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

void delete_overlay_cb(lv_anim_t* a) {
    auto* overlay = static_cast<lv_obj_t*>(a->var);
    if (overlay && lv_obj_is_valid(overlay)) {
        lv_obj_delete(overlay);
    }
}

} // namespace

void play_boot_animation(app::AssetManager& assets, lv_color_t accent) {
    // Full-screen black overlay on the top layer, above the active screen.
    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Shark, pivoting about its own centre so it grows in place ("coming
    // towards the screen").
    lv_obj_t* shark = lv_image_create(overlay);
    lv_image_set_src(shark, &bruce_img);
    lv_image_set_pivot(shark, bruce_img.header.w / 2, bruce_img.header.h / 2);
    lv_obj_align(shark, LV_ALIGN_CENTER, 0, -22);
    lv_image_set_scale(shark, 40);
    lv_obj_set_style_opa(shark, LV_OPA_TRANSP, 0);

    lv_obj_t* title = lv_label_create(overlay);
    if (lv_font_t* f = assets.load_font("inter-bold.ttf", 26)) {
        lv_obj_set_style_text_font(title, f, 0);
    }
    lv_obj_set_style_text_color(title, accent, 0);
    lv_label_set_text(title, "BRUCE");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 52);
    lv_obj_set_style_opa(title, LV_OPA_TRANSP, 0);

    lv_obj_t* tag = lv_label_create(overlay);
    if (lv_font_t* f = assets.load_font("inter-regular.ttf", 11)) {
        lv_obj_set_style_text_font(tag, f, 0);
    }
    lv_obj_set_style_text_color(tag, lv_color_white(), 0);
    lv_obj_set_style_text_opa(tag, LV_OPA_70, 0);
    lv_label_set_text(tag, "PREDATORY SYSTEMS");
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 74);
    lv_obj_set_style_opa(tag, LV_OPA_TRANSP, 0);

    lv_anim_t a;

    // Shark zoom in (ease-out, slight overshoot past 100%).
    lv_anim_init(&a);
    lv_anim_set_var(&a, shark);
    lv_anim_set_exec_cb(&a, scale_cb);
    lv_anim_set_values(&a, 40, 230); // 230/256 -> ~171x124 px, leaves room for the wordmark
    lv_anim_set_time(&a, 850);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    // Shark fade in.
    lv_anim_init(&a);
    lv_anim_set_var(&a, shark);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 400);
    lv_anim_start(&a);

    // Wordmark + tagline fade in, staggered.
    lv_anim_init(&a);
    lv_anim_set_var(&a, title);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 350);
    lv_anim_set_delay(&a, 650);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, tag);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 350);
    lv_anim_set_delay(&a, 900);
    lv_anim_start(&a);

    // Hold, then fade the whole overlay out and delete it (reveals the menu).
    lv_anim_init(&a);
    lv_anim_set_var(&a, overlay);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 350);
    lv_anim_set_delay(&a, 1750);
    lv_anim_set_completed_cb(&a, delete_overlay_cb);
    lv_anim_start(&a);
}

} // namespace view
