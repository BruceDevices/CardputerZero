/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>

namespace model {
namespace rf {

// Curated Sub-GHz frequencies (MHz) the CC1101 can tune, ported verbatim from
// Bruce's subghz_frequency_list. Ordered low->high and grouped in three bands;
// kRangeLimits indexes into this list so a scan can stay inside one band.
// Pure data - no hardware, no LVGL - so it ports 1:1 and is unit-checkable.
extern const float kFrequencyList[];
extern const std::size_t kFrequencyCount;

// Human labels for the four scan ranges (three bands + "All ranges").
extern const char* const kFrequencyRanges[4];

// [start, end] inclusive indices into kFrequencyList for each scan range.
// Index 3 ("All ranges") spans the whole list.
extern const int kRangeLimits[4][2];

} // namespace rf
} // namespace model
