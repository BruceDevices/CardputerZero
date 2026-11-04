/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Toast bubble on the layer it is parented to (usually lv_layer_top()). Ported
 * from Template's Toast, trimmed to the surface the app wires: show,
 * show_highlighted and hide. Fonts are LVGL built-ins; colors come from the
 * Bruce palette.
 */

#pragma once

#include "base_widget.h"

#include <cstdint>
#include <string>

namespace viewmodel {
class BaseViewModel;
}

namespace view::widgets {

enum class ToastTone {
    Default,
    Success,
    Warning,
    Error,
};

struct ToastConfig {
    int32_t min_width{96};
    int32_t max_width{292};
    int32_t min_height{36};
    int32_t horizontal_padding{14};
    int32_t vertical_padding{8};
    int32_t bottom_offset{38};
    uint32_t duration_ms{2600};
};

class Toast : public BaseWidgets {
public:
    Toast(lv_obj_t* parent,
          viewmodel::BaseViewModel& view_model,
          ToastConfig config = {});
    ~Toast() override;

    void build() override;
    void show(const std::string& message,
              ToastTone tone = ToastTone::Default,
              uint32_t duration_ms = 0);
    void show_highlighted(const std::string& prefix,
                          const std::string& highlighted,
                          const std::string& suffix,
                          ToastTone tone = ToastTone::Default,
                          uint32_t duration_ms = 0,
                          bool scroll = false);
    void hide();

private:
    void show_current_message(uint32_t duration_ms);
    void update_label_text();
    void update_size();
    void apply_theme();
    static void hide_timer_cb(lv_timer_t* timer);
    static void theme_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    viewmodel::BaseViewModel& view_model_;
    ToastConfig config_;
    ToastTone tone_{ToastTone::Default};
    std::string prefix_;
    std::string highlighted_;
    std::string suffix_;
    bool scroll_{false};
    lv_obj_t* label_{nullptr};
    const lv_font_t* font_{nullptr};
    lv_timer_t* hide_timer_{nullptr};
};

} // namespace view::widgets
