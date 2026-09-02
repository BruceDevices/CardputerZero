/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later).
 */

#include "rf_presets.h"

namespace model {
namespace rf {
namespace {

// Canonical preset table. Values reproduce exactly the parameters previously
// inlined in Bruce's sendRfCommand(): a field at 0 means "keep module default".
// Default OOK radio params (module defaults): mod=2, dev=1.58, BW=270.83,
// dataRate=10 - so OOK presets only override what differs.
const RfPreset rf_presets[] = {
    // name                mod  dev         rxBW    dataRate  legacyProto
    {"Ook270Async",         2,  0.0f,       270.f,  0.0f,     1},
    {"Ook650Async",         2,  0.0f,       650.f,  0.0f,     2},
    {"2FSKDev238Async",     0,  2.380371f,  238.f,  0.0f,     1},
    {"2FSKDev476Async",     0,  47.60742f,  476.f,  0.0f,     1},
    {"MSK99_97KbAsync",     4,  47.60742f,  0.0f,   99.97f,   1},
    {"GFSK9_99KbAsync",     1,  19.042969f, 0.0f,   9.996f,   1},
};

// Alias map: legacy Furi preset names found in existing `.sub` files ->
// canonical preset name above.
struct PresetAlias {
    const char* alias;
    const char* canonical;
};
const PresetAlias rf_preset_aliases[] = {
    {"FuriHalSubGhzPresetOok270Async", "Ook270Async"},
    {"FuriHalSubGhzPresetOok650Async", "Ook650Async"},
    {"FuriHalSubGhzPreset2FSKDev238Async", "2FSKDev238Async"},
    {"FuriHalSubGhzPreset2FSKDev476Async", "2FSKDev476Async"},
    {"FuriHalSubGhzPresetMSK99_97KbAsync", "MSK99_97KbAsync"},
    {"FuriHalSubGhzPresetGFSK9_99KbAsync", "GFSK9_99KbAsync"},
};

} // namespace

const RfPreset* rf_find_preset(const std::string& name) {
    // Resolve a legacy alias to its canonical name first.
    std::string wanted = name;
    for (const auto& a : rf_preset_aliases) {
        if (name == a.alias) {
            wanted = a.canonical;
            break;
        }
    }
    for (const auto& p : rf_presets) {
        if (wanted == p.name) return &p;
    }
    return nullptr;
}

} // namespace rf
} // namespace model
