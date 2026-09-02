/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "freq_tables.h"

namespace model {
namespace rf {

// 57 entries (indices 0-56). Keep in sync with kRangeLimits below: the band
// boundaries are index positions, not frequency comparisons.
const float kFrequencyList[] = {
    /* 300 - 348 MHz band (indices 0-23) */
    300.000f, 302.757f, 303.875f, 303.900f, 304.250f, 307.000f, 307.500f,
    307.800f, 309.000f, 310.000f, 312.000f, 312.100f, 312.200f, 313.000f,
    313.850f, 314.000f, 314.350f, 314.980f, 315.000f, 318.000f, 330.000f,
    345.000f, 348.000f, 350.000f,

    /* 387 - 464 MHz band (indices 24-47) */
    387.000f, 390.000f, 418.000f, 430.000f, 430.500f, 431.000f, 431.500f,
    433.075f, 433.220f, 433.420f, 433.657f, 433.889f, 433.920f, 434.075f,
    434.177f, 434.190f, 434.390f, 434.420f, 434.620f, 434.775f, 438.900f,
    440.175f, 464.000f, 467.750f,

    /* 779 - 928 MHz band (indices 48-56) */
    779.000f, 868.350f, 868.400f, 868.800f, 868.950f, 906.400f, 915.000f,
    925.000f, 928.000f,
};

const std::size_t kFrequencyCount = sizeof(kFrequencyList) / sizeof(kFrequencyList[0]);

const char* const kFrequencyRanges[4] = {
    "300-348 MHz",
    "387-464 MHz",
    "779-928 MHz",
    "All ranges",
};

const int kRangeLimits[4][2] = {
    {0, 23},  // 300-348 MHz
    {24, 47}, // 387-464 MHz
    {48, 56}, // 779-928 MHz
    {0, 56},  // All ranges
};

} // namespace rf
} // namespace model
