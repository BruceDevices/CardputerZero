/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later).
 */

#include "rf_math.h"

#include <cstdlib> // abs

namespace model {
namespace rf {
namespace {
constexpr uint64_t kCrc64EcmaPoly = 0x42F0E1EBA9EA3693ULL;
constexpr uint64_t kCrc64EcmaInit = 0xFFFFFFFFFFFFFFFFULL;
} // namespace

int find_pulse_index(const std::vector<int>& indexed_durations, int duration) {
    int abs_duration = std::abs(duration);
    int closest_index = -1;
    int closest_diff = 999999;

    for (size_t i = 0; i < indexed_durations.size(); i++) {
        int diff = std::abs(indexed_durations[i] - abs_duration);
        if (diff <= 50) { // ±50µs tolerance
            return static_cast<int>(i);
        }
        if (diff < closest_diff) {
            closest_diff = diff;
            closest_index = static_cast<int>(i);
        }
    }

    // Room for a new distinct pulse: signal the caller to append it.
    if (indexed_durations.size() < 4) return -1;
    return closest_index;
}

uint64_t crc64_ecma(const std::vector<int>& data) {
    uint64_t crc = kCrc64EcmaInit;
    for (int value : data) {
        crc ^= static_cast<uint64_t>(value) << 56; // use the value as the high byte
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000000000000000ULL) {
                crc = (crc << 1) ^ kCrc64EcmaPoly;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

uint64_t reverse_bits(uint64_t num, uint8_t bits) {
    uint64_t res = 0;
    for (uint8_t i = 0; i < bits; ++i) {
        res <<= 1;
        res |= (num >> i) & 1ULL;
    }
    return res;
}

} // namespace rf
} // namespace model
