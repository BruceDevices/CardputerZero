/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "config/device_config.h"

#include <string>
#include <vector>

namespace screen {

// Interactive settings screen (the "Config" menu, gear icon). Two levels:
//   * a module list (CC1101 active today; LoRa/nRF24/IR/GPS/PN532/ST25R shown
//     as upcoming), and
//   * a per-module field editor (only CC1101 for now).
//
// Edits a working copy of model::config::device_config(); "Save" persists it to
// disk. The 5-key nav bar drives a browse/edit interaction: browse moves the
// selection, Select enters edit on a value (or runs an action), and in edit mode
// Up/Down change the value while Select confirms (Back cancels the field).
class ConfigScreen : public BaseScreen {
public:
    ConfigScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);

private:
    enum class View { ModuleList, Cc1101Edit };

    enum class FieldKind { EnumStr, IntRange, Freq, Action };
    enum class ActionId { None, Save, Reset };

    struct Field {
        std::string label;
        FieldKind kind;
        std::string* str_target = nullptr;   // EnumStr
        int* int_target = nullptr;           // IntRange
        double* freq_target = nullptr;       // Freq
        const std::vector<std::string>* candidates = nullptr; // EnumStr
        int int_min = 0;
        int int_max = 0;
        ActionId action = ActionId::None;
    };

    void build_content(lv_obj_t* content) override;
    void build_module_list(lv_obj_t* content);
    void build_cc1101_editor(lv_obj_t* content);

    void rebuild(); // re-enter build for the current view (nav change)
    void enter_cc1101();
    void back_or_up();

    void move_selection(int delta);
    void on_select();
    void adjust_value(int delta); // edit-mode Up/Down
    void apply_selection_highlight();
    void refresh_row(size_t index);
    std::string field_value_text(const Field& f) const;
    void configure_nav();

    static void up_cb(lv_event_t* event);
    static void down_cb(lv_event_t* event);
    static void select_cb(lv_event_t* event);
    static void back_cb(lv_event_t* event);

    View view_ = View::ModuleList;
    bool editing_ = false; // in edit mode on the selected field
    size_t selected_ = 0;
    lv_obj_t* content_container_ = nullptr; // the BaseScreen content, for in-place rebuilds
    std::vector<lv_obj_t*> rows_;
    std::vector<Field> fields_;

    // Working copy; committed to device_config() only on Save.
    model::config::DeviceConfig working_;

    // Candidate value lists (owned so Field pointers stay valid).
    std::vector<std::string> spidev_candidates_;
    std::vector<std::string> chip_candidates_;
};

} // namespace screen
