/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). The OOK protocol timing model is
 * DERIVED FROM rc-switch (LGPL-2.1-or-later), (C) 2011 Suat Ozgur and
 * contributors; the Flipper-name mapping mirrors the Flipper Zero firmware
 * (GPL-3.0-or-later).
 */

#pragma once

#include "rf_codes.h" // model::rf::HighLow

#include <cstdint>

namespace model {
namespace rf {

// Single source of truth for static OOK protocol definitions and radio presets.
// Pure data - no hardware, no LVGL. Consumers (encoder / decoder / send / scan)
// read from here; no protocol parameters are redefined elsewhere.

// Radio preset: how to configure the transceiver (modulation, bandwidth,
// deviation, data rate). A field left at 0 means "keep the module default".
struct RfPreset {
    const char* name;    // canonical preset name written/read in `.sub`
    uint8_t modulation;  // CC1101: 0=2-FSK, 1=GFSK, 2=ASK/OOK, 4=MSK
    float deviation;     // kHz (0 = keep default)
    float rxBW;          // kHz (0 = keep default)
    float dataRate;      // kbps (0 = keep default)
    uint8_t legacyProto; // default legacy protocol no. for OOK presets
};

// Static OOK protocol definition. Timings follow the classic factor model:
// every pulse is a multiple of `te` µs, expressed as {high, low} counts.
struct RfProtocolDef {
    const char* name; // identity written to `Protocol:` in the `.sub` file
    uint16_t te;      // base pulse length in µs
    HighLow sync;     // sync / pilot factor ({0,0} = none)
    HighLow zero;     // bit 0 encoding
    HighLow one;      // bit 1 encoding
    uint8_t bits;     // typical payload length in bits (0 = variable)
    bool inverted;    // inverted signal level
    uint8_t flags;    // bitmask, see RfProtocolFlags
};

enum RfProtocolFlags : uint8_t {
    RF_PF_HAS_SYNC = 0x01,  // protocol uses a sync/pilot pulse
    RF_PF_FIXED_LEN = 0x02, // payload length is fixed (== bits)
};

} // namespace rf
} // namespace model
