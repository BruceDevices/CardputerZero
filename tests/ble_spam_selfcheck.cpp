/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the BLE-spam advertisement payload builders. Not
 * part of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/platform tests/ble_spam_selfcheck.cpp \
 *       src/platform/ble_spam.cpp -o /tmp/ble_spam_check && /tmp/ble_spam_check
 *
 * The bug-prone part is the fixed AD header bytes and the significant length of
 * each packet - these must match the Flipper/firmware layouts exactly or iOS/
 * Android/Windows silently drop the advertisement. Random fields are ignored.
 */

#include "ble_spam.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using platform::BleSpamTarget;

int main() {
    uint8_t p[31];

    // --- parse ---
    BleSpamTarget t;
    assert(platform::ble_spam_parse_target("apple", t) && t == BleSpamTarget::Apple);
    assert(platform::ble_spam_parse_target("all", t) && t == BleSpamTarget::All);
    assert(!platform::ble_spam_parse_target("nope", t));
    assert(!platform::ble_spam_parse_target(nullptr, t));

    // Builders are deterministic given rng_state; seed is irrelevant for the
    // fixed header bytes we assert. Use a fixed seed for reproducibility.
    uint32_t s;

    // --- Apple ProximityPair: 31 bytes, Apple manuf + Continuity ProximityPair ---
    s = 1;
    assert(platform::ble_spam_build_payload(BleSpamTarget::Apple, p, s) == 31);
    assert(p[0] == 0x1E && p[1] == 0xFF && p[2] == 0x4C && p[3] == 0x00);
    assert(p[4] == 0x07 && p[5] == 0x19 && p[6] == 0x07);

    // --- Apple NearbyAction: Flags + 10-byte manuf element = 14 bytes ---
    s = 2;
    assert(platform::ble_spam_build_payload(BleSpamTarget::AppleAction, p, s) == 14);
    assert(p[0] == 0x02 && p[1] == 0x01 && p[2] == 0x06); // Flags
    assert(p[3] == 0x0A && p[4] == 0xFF && p[5] == 0x4C && p[6] == 0x00);
    assert(p[7] == 0x0F && p[8] == 0x05);

    // --- Google Fast Pair: Flags + UUID list + service data 0xFE2C + TX = 17 ---
    s = 3;
    assert(platform::ble_spam_build_payload(BleSpamTarget::Google, p, s) == 17);
    assert(p[0] == 0x02 && p[1] == 0x01 && p[2] == 0x06);
    assert(p[3] == 0x03 && p[4] == 0x03 && p[5] == 0x2C && p[6] == 0xFE);
    assert(p[7] == 0x06 && p[8] == 0x16 && p[9] == 0x2C && p[10] == 0xFE);
    assert(p[14] == 0x02 && p[15] == 0x0A);

    // --- Samsung: either Watch (18, flags + 0x0E/0xFF/0x75) or Buds (31, 0x1B/0xFF/0x75) ---
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        s = seed;
        size_t n = platform::ble_spam_build_payload(BleSpamTarget::Samsung, p, s);
        bool watch = (n == 18 && p[0] == 0x02 && p[3] == 0x0E && p[4] == 0xFF && p[5] == 0x75);
        bool buds  = (n == 31 && p[0] == 0x1B && p[1] == 0xFF && p[2] == 0x75);
        assert(watch || buds);
    }

    // --- Microsoft Swift Pair: Flags + manuf (6+name) ; name 1..21 => len 11..31 ---
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        s = seed;
        size_t n = platform::ble_spam_build_payload(BleSpamTarget::Microsoft, p, s);
        assert(n >= 11 && n <= 31);
        assert(p[0] == 0x02 && p[1] == 0x01 && p[2] == 0x06);
        assert(p[3] == static_cast<uint8_t>(n - 3 - 1)); // AD length = element bytes - its own length byte
        assert(p[4] == 0xFF && p[5] == 0x06 && p[6] == 0x00);
        assert(p[7] == 0x03 && p[8] == 0x00 && p[9] == 0x80);
    }

    // --- All: never exceeds the 31-byte legacy payload ---
    s = 123;
    for (int i = 0; i < 1000; ++i) {
        assert(platform::ble_spam_build_payload(BleSpamTarget::All, p, s) <= 31);
    }

    std::puts("ble_spam self-check OK");
    return 0;
}
