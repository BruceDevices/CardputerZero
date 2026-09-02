/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the device pin/bus config (de)serialization. Not
 * part of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model/config tests/device_config_selfcheck.cpp \
 *       src/model/config/device_config.cpp -o /tmp/devcfg_check && /tmp/devcfg_check
 *
 * A wrong key parse silently loses a user's pin edit, so this pins the
 * serialize -> parse round-trip (CC1101 + a couple of future modules) and the
 * "unknown key ignored / missing key keeps default" behavior.
 */

#include "device_config.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace model::config;

// Float equality with tolerance: the host (MinGW/x87) keeps literals at extended
// precision, so exact == on a stored double can spuriously fail; the values
// themselves round-trip fine on the ARM64 target.
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    DeviceConfig a;
    a.cc1101.spidev = "/dev/spidev1.0";
    a.cc1101.aux0 = {"gpiochip2", 17}; // GDO0
    a.cc1101.aux1 = {"gpiochip0", 27}; // GDO2
    a.cc1101_freq_mhz = 315.0;
    a.cc1101_scan_range = 0;
    a.gps.device = "/dev/ttyAMA0";
    a.gps.baud = 115200;
    a.ir.tx = {"gpiochip0", 18};

    // Round-trip through text.
    DeviceConfig b;
    b.parse(a.serialize());
    assert(b.cc1101.spidev == "/dev/spidev1.0");
    assert(b.cc1101.aux0.chip == "gpiochip2" && b.cc1101.aux0.offset == 17);
    assert(b.cc1101.aux1.chip == "gpiochip0" && b.cc1101.aux1.offset == 27);
    assert(b.cc1101.aux0.is_set() && !b.cc1101.aux2.is_set());
    assert(near(b.cc1101_freq_mhz, 315.0));
    assert(b.cc1101_scan_range == 0);
    assert(b.gps.device == "/dev/ttyAMA0" && b.gps.baud == 115200);
    assert(b.ir.tx.chip == "gpiochip0" && b.ir.tx.offset == 18);

    // Defaults preload the M5 CAP reference wiring (docs/cardputerzero-gpio.md).
    DeviceConfig d;
    assert(d.cc1101.spidev == "/dev/spidev0.0");
    assert(d.cc1101.aux0.chip == "gpiochip0" && d.cc1101.aux0.offset == 15); // GDO0
    assert(d.cc1101.aux1.offset == 13);      // RF_SW0 (antenna switch)
    assert(d.cc1101.aux2.offset == -1);      // GDO2 not broken out on this CAP
    assert(near(d.cc1101_freq_mhz, 433.92));
    assert(d.lora.aux0.offset == 4 && d.lora.aux1.offset == 3 && d.lora.aux2.offset == 6);
    assert(d.gps.device == "/dev/ttyS0" && d.gps.baud == 9600);

    // Unknown keys ignored; missing keys keep current values.
    DeviceConfig e;
    e.cc1101.aux0 = {"gpiochip3", 5};
    e.parse("bogus.key=123\ncc1101.freq_mhz=868.35\n");
    assert(near(e.cc1101_freq_mhz, 868.35));
    assert(e.cc1101.aux0.chip == "gpiochip3" && e.cc1101.aux0.offset == 5); // untouched

    // reset_defaults restores the compiled-in state (M5 reference defaults).
    e.reset_defaults();
    assert(e.cc1101.aux0.offset == 15 && near(e.cc1101_freq_mhz, 433.92));

    std::puts("device config self-check OK");
    return 0;
}
