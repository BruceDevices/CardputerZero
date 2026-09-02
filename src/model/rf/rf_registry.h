/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). Timing table DERIVED FROM rc-switch
 * (LGPL-2.1-or-later); Flipper-name mapping mirrors the Flipper Zero firmware
 * (GPL-3.0-or-later).
 */

#pragma once

#include "rf_protocol.h"

#include <string>

namespace model {
namespace rf {

// Static OOK protocol registry. Lookup + iteration used by the decoder and the
// replay dispatch. Pure data - safe to unit-test without hardware.

// Find a protocol definition by name. Flipper Zero protocol names (as written
// in standard `.sub` files, e.g. "Nice FLO", "Holtek_HT12X", "Phoenix_V2") are
// accepted and resolved to the canonical registry entry. Returns nullptr if
// none.
const RfProtocolDef* rf_find_protocol(const std::string& name);

// Map a canonical registry name back to the Flipper protocol name (for writing
// Flipper-standard `.sub` files). Returns the name unchanged when it is already
// Flipper-compatible.
std::string rf_flipper_protocol_name(const std::string& canonical);

// Resolve a classic OOK protocol NUMBER (carried by the CLI/JSON TX contract)
// to a registry definition. Falls back to RcSwitch_1 for unknown numbers; never
// returns nullptr.
const RfProtocolDef* rf_protocol_for_number(int proto_no);

// Iteration helpers (e.g. for the generic decoder to try every protocol).
const RfProtocolDef* rf_protocol_at(int index);
int rf_protocol_count();

} // namespace rf
} // namespace model
