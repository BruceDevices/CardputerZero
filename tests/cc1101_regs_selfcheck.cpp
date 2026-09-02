/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the CC1101 register math. Not part of the CMake
 * build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/platform/rf tests/cc1101_regs_selfcheck.cpp \
 *       src/platform/rf/cc1101_regs.cpp -o /tmp/cc1101_regs_check \
 *       && /tmp/cc1101_regs_check
 *
 * The radio arithmetic is pure and deterministic, so it can be pinned on the
 * host even though the SPI driver that applies it cannot. A wrong FREQ word puts
 * the carrier on the wrong frequency (silent RX/TX failure), so this checks the
 * round-trips and a couple of datasheet anchors.
 */

#include "cc1101_regs.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace platform::rf;

int main() {
    // FREQ round-trip: programmed carrier is within one LSB step (~397 Hz) of
    // the request across the whole Sub-GHz span.
    const double step = kCc1101XoscHz / 65536.0; // ~396.7 Hz per FREQ LSB
    for (double mhz : {300.0, 315.0, 390.0, 433.92, 434.0, 868.35, 915.0, 928.0}) {
        double hz = mhz * 1e6;
        FreqRegs r = cc1101_calc_freq(hz);
        double back = cc1101_freq_from_regs(r);
        assert(std::fabs(back - hz) <= step);
    }

    // Datasheet anchor: 433.92 MHz on a 26 MHz xtal -> 0x10 0xB0 0x71.
    {
        FreqRegs r = cc1101_calc_freq(433.92e6);
        assert(r.freq2 == 0x10 && r.freq1 == 0xB0 && r.freq0 == 0x71);
    }

    // Data rate round-trip within 1% for the presets we care about.
    for (double kbps : {1.0, 2.4, 4.8, 9.996, 10.0, 99.97}) {
        double baud = kbps * 1000.0;
        DataRateRegs r = cc1101_calc_datarate(baud);
        double back = cc1101_datarate_from_regs(r);
        assert(std::fabs(back - baud) / baud < 0.01);
        assert(r.drate_e <= 15);
    }

    // RX bandwidth: result is a valid CC1101 BW that covers the request, and the
    // canonical 270.833 kHz / 650 kHz presets resolve to the expected values.
    for (double bw : {58e3, 100e3, 200e3, 270.833e3, 325e3, 650e3}) {
        uint8_t high = cc1101_calc_rxbw_mdmcfg4_high(bw);
        double got = cc1101_rxbw_from_mdmcfg4_high(high);
        assert(got + 1.0 >= bw); // covers the request (allow rounding slack)
        assert((high & 0x0F) == 0); // only the high nibble is used
    }
    // Canonical presets: 270.833 kHz is E=1,M=2 -> 0x60; 650 kHz is E=0,M=1 -> 0x10.
    assert(cc1101_calc_rxbw_mdmcfg4_high(270.833e3) == 0x60);
    assert(cc1101_calc_rxbw_mdmcfg4_high(650e3) == 0x10);

    // Deviation round-trip is close for a typical 2-FSK deviation.
    {
        uint8_t reg = cc1101_calc_deviation(47607.0);
        double got = cc1101_deviation_from_reg(reg);
        assert(std::fabs(got - 47607.0) / 47607.0 < 0.05);
    }

    std::puts("cc1101 regs self-check OK");
    return 0;
}
