/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later).
 */

#pragma once

#include "rf_protocol.h"

#include <string>

namespace model {
namespace rf {

// Radio preset registry. Single place where the `.sub` "Preset:" names map to
// concrete transceiver parameters. Resolve a preset by its canonical/alias
// name; returns nullptr if unknown (caller then tries the numeric legacy path).
const RfPreset* rf_find_preset(const std::string& name);

} // namespace rf
} // namespace model
