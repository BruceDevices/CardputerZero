/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). OOK send state machine DERIVED FROM
 * rc-switch (LGPL-2.1-or-later); KeeLoq OOK framing mirrors the Flipper Zero
 * firmware (GPL-3.0-or-later).
 */

#pragma once

#include "rf_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace model {
namespace rf {

// Pure, hardware-free pulse-train builders. They turn a logical payload into a
// list of signed microsecond timings (sign = logic level: + HIGH, - LOW). The
// platform layer (src/platform/rf) is what streams the returned durations out
// the CC1101 GDO0 line; nothing here touches hardware.

// Build the signed-µs duration list for `data` (`bits` long, MSB first) under
// protocol `def`, repeated `repeat` times. `te` overrides def->te when > 0.
// Returns false on bad input (null def, 0 bits, non-positive base pulse).
bool rf_encode_protocol(
    uint64_t data, unsigned int bits, int te, const RfProtocolDef* def, int repeat, std::vector<int>& out
);

// Build the KeeLoq OOK frame (11-pulse header, sync gap, 64 PWM bits MSB-first,
// trailing status bit + inter-frame gap) for `key`. Single frame - the caller
// repeats it. KeeLoq does not fit the factor-based OOK model, hence its own
// builder.
bool rf_keeloq_durations(uint64_t key, std::vector<int>& out);

// Build a RAW bit-string frame: each '1'/'0' is one half-pulse of `te` µs at the
// matching level (other chars skipped). Walks the string from end to start, as
// the legacy bit sender did. Returns false on empty input or te <= 0.
bool rf_encode_raw_bits(const std::string& bits, int te, std::vector<int>& out);

} // namespace rf
} // namespace model
