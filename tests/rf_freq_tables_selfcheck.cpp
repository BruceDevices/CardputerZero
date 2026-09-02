/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the ported Sub-GHz frequency tables. Not part of
 * the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model/rf tests/rf_freq_tables_selfcheck.cpp \
 *       src/model/rf/freq_tables.cpp -o /tmp/rf_freq_check && /tmp/rf_freq_check
 *
 * Guards the one thing that silently breaks a band-limited scan: kRangeLimits
 * indexes into kFrequencyList by position, so if the list and the limits drift
 * out of sync (a frequency added mid-band, a miscounted boundary), a scan walks
 * the wrong band. This pins the count, the band boundaries, and the ordering.
 */

#include "freq_tables.h"

#include <cassert>
#include <cstdio>

int main() {
    using namespace model::rf;

    // The list has exactly 57 entries (indices 0-56); "All ranges" must span it.
    assert(kFrequencyCount == 57);
    assert(kRangeLimits[3][0] == 0);
    assert(kRangeLimits[3][1] == static_cast<int>(kFrequencyCount) - 1);

    // Every range limit is a valid, ordered index into the list.
    for (int r = 0; r < 4; ++r) {
        assert(kRangeLimits[r][0] >= 0);
        assert(kRangeLimits[r][1] < static_cast<int>(kFrequencyCount));
        assert(kRangeLimits[r][0] <= kRangeLimits[r][1]);
    }

    // The three real bands tile the list contiguously with no gap or overlap.
    assert(kRangeLimits[0][0] == 0);
    assert(kRangeLimits[0][1] + 1 == kRangeLimits[1][0]);
    assert(kRangeLimits[1][1] + 1 == kRangeLimits[2][0]);
    assert(kRangeLimits[2][1] == static_cast<int>(kFrequencyCount) - 1);

    // Each band's frequencies fall inside its advertised MHz window (loosely -
    // the labels are the promise the UI shows the user).
    for (std::size_t i = kRangeLimits[0][0]; i <= static_cast<std::size_t>(kRangeLimits[0][1]); ++i) {
        assert(kFrequencyList[i] >= 300.0f && kFrequencyList[i] <= 351.0f);
    }
    for (std::size_t i = kRangeLimits[1][0]; i <= static_cast<std::size_t>(kRangeLimits[1][1]); ++i) {
        assert(kFrequencyList[i] >= 387.0f && kFrequencyList[i] <= 468.0f);
    }
    for (std::size_t i = kRangeLimits[2][0]; i <= static_cast<std::size_t>(kRangeLimits[2][1]); ++i) {
        assert(kFrequencyList[i] >= 779.0f && kFrequencyList[i] <= 928.0f);
    }

    std::puts("rf freq tables self-check OK");
    return 0;
}
