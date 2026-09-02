/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). OOK capture/decode state machine
 * DERIVED FROM rc-switch (LGPL-2.1-or-later); KeeLoq frame decoder framing
 * mirrors the Flipper Zero firmware (GPL-3.0-or-later).
 */

#pragma once

#include "rf_codes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace model {
namespace rf {

// Pure OOK/KeeLoq decoders. Input is a list of signed microsecond durations
// (sign = logic level: + HIGH, - LOW), exactly as the platform capture layer
// (src/platform/rf, GDO0 edges) produces. Nothing here touches hardware; this
// is where a captured signal becomes a decoded RfCodes.

// Try to decode an OOK frame from `durations` using the protocol registry. On
// success fills out.key, out.Bit, out.te, out.protocol (registry name) and
// out.preset, and returns true. Other fields are untouched.
bool rf_decode_ook(const std::vector<int>& durations, RfCodes& out);

// Try to decode a KeeLoq frame (dedicated PWM state machine: header + sync gap +
// 64 PWM bits). On success sets out.key (raw 64-bit), out.Bit=64, out.te,
// out.protocol="KeeLoq", out.preset and returns true. The caller still splits
// out.key into fix/encrypted and runs manufacturer identification.
bool rf_decode_keeloq(const std::vector<int>& durations, RfCodes& out);

// Build the RAW representation from `durations`:
//  - `dataOut`  : "+a -b +c ..." string of signed durations.
//  - `teOut`    : first positive duration (base pulse estimate).
//  - when a repeated pattern is detected, sets `hasCrc=true` and fills `crcOut`
//    (CRC-64 of the pulse-index sequence), `indexedOut` (distinct pulse lengths)
//    and `bitsOut` (number of indexed transitions).
// Returns the number of transitions written to `dataOut`.
int rf_build_raw(
    const std::vector<int>& durations, std::string& dataOut, bool& hasCrc, uint64_t& crcOut,
    std::vector<int>& indexedOut, int& bitsOut, int& teOut
);

} // namespace rf
} // namespace model
