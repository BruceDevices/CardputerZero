/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "config_screen.h"

#include "asset_manager.h"
#include "rf/freq_tables.h"
#include "theme.h"
#include "ui_const.h"

#include <cmath>
#include <cstdio>

namespace screen {
namespace {

constexpr int32_t kHeaderHeight = 20;

inline lv_color_t bruce_accent() { return lv_color_hex(view::color::dark::kPrimary); }

// Green-ish accent for edit mode, so a field being changed reads differently
// from a merely-selected one.
inline lv_color_t edit_accent() { return lv_color_hex(0x2ecc71); }

// The upcoming modules, shown disabled so the menu advertises the roadmap
// without pretending they work yet.
const char* const kUpcomingModules[] = {
    "LoRa - soon", "nRF24 - soon", "IR - soon", "GPS (UART) - soon", "PN532 - soon", "ST25R - soon",
};

std::string format_freq(double mhz) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f MHz", mhz);
    return buf;
}

} // namespace

ConfigScreen::ConfigScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    working_ = model::config::device_config(); // edit a copy; commit on Save
    spidev_candidates_ = {"/dev/spidev0.0", "/dev/spidev0.1", "/dev/spidev1.0", "/dev/spidev1.1"};
    chip_candidates_ = {"gpiochip0", "gpiochip1", "gpiochip2", "gpiochip3", "gpiochip4"};
    init();
}

void ConfigScreen::build_content(lv_obj_t* content) {
    if (view_ == View::Cc1101Edit) {
        build_cc1101_editor(content);
    } else {
        build_module_list(content);
    }
}

void ConfigScreen::rebuild() {
    // ConfigScreen switches between the module list and the CC1101 editor without
    // going through the view model, so it rebuilds its own content in place: clear
    // the container captured at build time and re-run the per-view builder.
    if (!content_container_) return;
    lv_obj_clean(content_container_);
    rows_.clear();
    build_content(content_container_);
}

void ConfigScreen::build_module_list(lv_obj_t* content) {
    content_container_ = content;
    view_ = View::ModuleList;
    editing_ = false;
    fields_.clear();

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_label_set_text(header, "Config - modules");

    auto* list = lv_list_create(content);
    lv_obj_set_size(list, LV_PCT(100),
                    view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    // Row 0: CC1101 (active). Then the upcoming modules (disabled look).
    auto* cc = lv_list_add_button(list, nullptr, "CC1101 (Sub-GHz)");
    lv_obj_clear_flag(cc, LV_OBJ_FLAG_CLICKABLE);
    rows_.push_back(cc);
    for (const char* name : kUpcomingModules) {
        // The " - soon" suffix marks these as not-yet-editable; selecting one is
        // a no-op (see on_select), so no extra styling is needed.
        auto* row = lv_list_add_button(list, nullptr, name);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        rows_.push_back(row);
    }

    if (selected_ >= rows_.size()) selected_ = 0;
    apply_selection_highlight();
    configure_nav();
}

void ConfigScreen::enter_cc1101() {
    view_ = View::Cc1101Edit;
    selected_ = 0;
    editing_ = false;

    // Build the field list, pointing at the working copy so edits land directly.
    fields_.clear();
    auto& c = working_.cc1101;
    fields_.push_back({"SPI device", FieldKind::EnumStr, &c.spidev, nullptr, nullptr, &spidev_candidates_, 0, 0, ActionId::None});
    fields_.push_back({"GDO0 chip", FieldKind::EnumStr, &c.aux0.chip, nullptr, nullptr, &chip_candidates_, 0, 0, ActionId::None});
    fields_.push_back({"GDO0 line", FieldKind::IntRange, nullptr, &c.aux0.offset, nullptr, nullptr, -1, 63, ActionId::None});
    fields_.push_back({"RF switch chip", FieldKind::EnumStr, &c.aux1.chip, nullptr, nullptr, &chip_candidates_, 0, 0, ActionId::None});
    fields_.push_back({"RF switch line", FieldKind::IntRange, nullptr, &c.aux1.offset, nullptr, nullptr, -1, 63, ActionId::None});
    fields_.push_back({"GDO2 chip", FieldKind::EnumStr, &c.aux2.chip, nullptr, nullptr, &chip_candidates_, 0, 0, ActionId::None});
    fields_.push_back({"GDO2 line", FieldKind::IntRange, nullptr, &c.aux2.offset, nullptr, nullptr, -1, 63, ActionId::None});
    fields_.push_back({"Frequency", FieldKind::Freq, nullptr, nullptr, &working_.cc1101_freq_mhz, nullptr, 0, 0, ActionId::None});
    fields_.push_back({"Save", FieldKind::Action, nullptr, nullptr, nullptr, nullptr, 0, 0, ActionId::Save});
    fields_.push_back({"Reset defaults", FieldKind::Action, nullptr, nullptr, nullptr, nullptr, 0, 0, ActionId::Reset});

    rebuild();
}

void ConfigScreen::build_cc1101_editor(lv_obj_t* content) {
    content_container_ = content;

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_label_set_text(header, "CC1101 pins / bus");

    auto* list = lv_list_create(content);
    lv_obj_set_size(list, LV_PCT(100),
                    view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    for (size_t i = 0; i < fields_.size(); ++i) {
        auto* row = lv_list_add_button(list, nullptr, "");
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        rows_.push_back(row);
        refresh_row(i);
    }

    if (selected_ >= rows_.size()) selected_ = 0;
    apply_selection_highlight();
    configure_nav();
}

std::string ConfigScreen::field_value_text(const Field& f) const {
    switch (f.kind) {
        case FieldKind::EnumStr: return f.str_target ? *f.str_target : "";
        case FieldKind::IntRange:
            if (!f.int_target) return "";
            return (*f.int_target < 0) ? std::string("unset (NC)") : std::to_string(*f.int_target);
        case FieldKind::Freq: return f.freq_target ? format_freq(*f.freq_target) : "";
        case FieldKind::Action: return "";
    }
    return "";
}

void ConfigScreen::refresh_row(size_t index) {
    if (index >= rows_.size() || index >= fields_.size()) return;
    const Field& f = fields_[index];
    std::string text;
    if (f.kind == FieldKind::Action) {
        text = f.label;
    } else if (editing_ && index == selected_) {
        text = f.label + std::string(": < ") + field_value_text(f) + " >";
    } else {
        text = f.label + std::string(": ") + field_value_text(f);
    }
    // A list button created with no icon holds its label as the first child.
    lv_obj_t* label = lv_obj_get_child(rows_[index], 0);
    if (label) lv_label_set_text(label, text.c_str());
}

void ConfigScreen::apply_selection_highlight() {
    const lv_color_t accent = editing_ ? edit_accent() : bruce_accent();
    for (size_t i = 0; i < rows_.size(); ++i) {
        lv_obj_t* row = rows_[i];
        if (i == selected_) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, accent, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_text_color(row, lv_color_white(), 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_remove_local_style_prop(row, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

void ConfigScreen::move_selection(int delta) {
    const int count = static_cast<int>(rows_.size());
    if (count == 0) return;
    int next = (static_cast<int>(selected_) + delta) % count;
    if (next < 0) next += count;
    selected_ = static_cast<size_t>(next);
    apply_selection_highlight();
}

void ConfigScreen::adjust_value(int delta) {
    if (view_ != View::Cc1101Edit || selected_ >= fields_.size()) return;
    Field& f = fields_[selected_];
    switch (f.kind) {
        case FieldKind::EnumStr: {
            if (!f.str_target || !f.candidates || f.candidates->empty()) break;
            const auto& cand = *f.candidates;
            int idx = 0;
            for (size_t i = 0; i < cand.size(); ++i) {
                if (cand[i] == *f.str_target) { idx = static_cast<int>(i); break; }
            }
            idx = (idx + delta) % static_cast<int>(cand.size());
            if (idx < 0) idx += static_cast<int>(cand.size());
            *f.str_target = cand[idx];
            break;
        }
        case FieldKind::IntRange: {
            if (!f.int_target) break;
            int v = *f.int_target + delta;
            if (v < f.int_min) v = f.int_min;
            if (v > f.int_max) v = f.int_max;
            *f.int_target = v;
            break;
        }
        case FieldKind::Freq: {
            if (!f.freq_target) break;
            const auto n = static_cast<int>(model::rf::kFrequencyCount);
            int idx = 0;
            double best = 1e18;
            for (int i = 0; i < n; ++i) {
                double d = std::fabs(model::rf::kFrequencyList[i] - *f.freq_target);
                if (d < best) { best = d; idx = i; }
            }
            idx = (idx + delta) % n;
            if (idx < 0) idx += n;
            *f.freq_target = model::rf::kFrequencyList[idx];
            break;
        }
        case FieldKind::Action:
            break;
    }
    refresh_row(selected_);
}

void ConfigScreen::on_select() {
    if (view_ == View::ModuleList) {
        if (selected_ == 0) enter_cc1101(); // only CC1101 is editable today
        return;
    }

    // Cc1101Edit view
    if (selected_ >= fields_.size()) return;
    const Field& f = fields_[selected_];

    if (f.kind == FieldKind::Action) {
        if (f.action == ActionId::Save) {
            model::config::device_config() = working_;
            const bool ok = model::config::device_config().save();
            // Reflect the result on the row until the next redraw/navigation.
            lv_obj_t* label = lv_obj_get_child(rows_[selected_], 0);
            if (label) lv_label_set_text(label, ok ? "Save  (saved)" : "Save  (write failed)");
        } else if (f.action == ActionId::Reset) {
            working_.reset_defaults();
            for (size_t i = 0; i < fields_.size(); ++i) refresh_row(i);
        }
        return;
    }

    // Value field: toggle edit mode.
    editing_ = !editing_;
    refresh_row(selected_);
    apply_selection_highlight();
    configure_nav();
}

void ConfigScreen::back_or_up() {
    if (editing_) {
        // Exit edit mode. (Values are applied live to the working copy; nothing
        // is persisted until Save, so leaving edit mode is safe.)
        editing_ = false;
        refresh_row(selected_);
        apply_selection_highlight();
        configure_nav();
        return;
    }
    if (view_ == View::Cc1101Edit) {
        selected_ = 0;
        view_ = View::ModuleList;
        fields_.clear();
        rebuild();
        return;
    }
    view_model().go_back(); // ModuleList -> root menu
}

void ConfigScreen::configure_nav() {
    if (editing_) {
        nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
        nav_bar().set_icon(2, "OK", select_cb, this);
        nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
        configure_common_nav("Done", back_cb, this);
    } else {
        nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
        nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
        nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
        configure_common_nav(view::NAV_BACK, back_cb, this);
    }
}

void ConfigScreen::up_cb(lv_event_t* event) {
    auto* self = static_cast<ConfigScreen*>(lv_event_get_user_data(event));
    if (!self) return;
    if (self->editing_) self->adjust_value(1);
    else self->move_selection(-1);
}

void ConfigScreen::down_cb(lv_event_t* event) {
    auto* self = static_cast<ConfigScreen*>(lv_event_get_user_data(event));
    if (!self) return;
    if (self->editing_) self->adjust_value(-1);
    else self->move_selection(1);
}

void ConfigScreen::select_cb(lv_event_t* event) {
    auto* self = static_cast<ConfigScreen*>(lv_event_get_user_data(event));
    if (self) self->on_select();
}

void ConfigScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<ConfigScreen*>(lv_event_get_user_data(event));
    if (self) self->back_or_up();
}

} // namespace screen
