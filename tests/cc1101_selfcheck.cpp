/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the CC1101 SPI header framing. Not part of the
 * CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/platform tests/cc1101_selfcheck.cpp -o /tmp/cc1101_check \
 *       && /tmp/cc1101_check
 *
 * The one thing that silently bricks detection: reading a CC1101 status
 * register (0x30-0x3D) without the burst bit fires a command strobe instead,
 * so VERSION must be read with addr | READ(0x80) | BURST(0x40) = addr | 0xC0.
 */

#include "cc1101.h"

#include <cassert>
#include <cstdio>

int main() {
    // VERSION (0x31) and PARTNUM (0x30) reads must set both read + burst bits.
    assert(platform::cc1101_status_read_header(0x31) == 0xF1);
    assert(platform::cc1101_status_read_header(0x30) == 0xF0);
    // Burst bit must never be dropped for any status register.
    for (uint8_t addr = 0x30; addr <= 0x3D; ++addr) {
        assert((platform::cc1101_status_read_header(addr) & 0xC0) == 0xC0);
    }

    // ST25R3916 space-A register read: top two mode bits must be 0b01 (addr|0x40),
    // never the CC1101-style burst bits. IC_IDENTITY (0x3F) -> 0x7F.
    assert(platform::st25r_reg_read_header(0x3F) == 0x7F);
    assert(platform::st25r_reg_read_header(0x00) == 0x40);
    for (uint8_t addr = 0x00; addr <= 0x3F; ++addr) {
        assert((platform::st25r_reg_read_header(addr) & 0xC0) == 0x40);
    }

    std::puts("cc1101 + st25r self-check OK");
    return 0;
}
