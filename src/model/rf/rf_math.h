/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). Small pure numeric helpers shared by
 * the RF decode/encode paths.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace model {
namespace rf {

// Map a captured duration to an index in `indexed_durations` (±50µs tolerance).
// Returns -1 when there is room (< 4 entries) for the caller to append a new
// distinct pulse length; otherwise the closest existing index.
int find_pulse_index(const std::vector<int>& indexed_durations, int duration);

// CRC-64-ECMA over a pulse-index sequence (used to detect a repeated RAW frame).
uint64_t crc64_ecma(const std::vector<int>& data);

// Reverse the low `bits` bits of `num` (general bit helper, used by KeeLoq).
uint64_t reverse_bits(uint64_t num, uint8_t bits);

} // namespace rf
} // namespace model
