/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Parser for `bluetoothctl --timeout 5 scan le` output, shared by the BLE Scan
 * screen (#11) and BLE wardriving. Pure (no LVGL, no process), unit-tested in
 * tests/wardrive_selfcheck.cpp against a captured bluetoothctl 5.85 log.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace platform::ble {

struct Device {
    std::string name; // "" when the device has no name (bluetoothctl shows the MAC)
    std::string mac;  // "AA:BB:CC:DD:EE:FF", as bluetoothctl prints it
    int rssi = 0;     // last usable RSSI seen, 0 when none (BlueZ 0 / 127 are not usable)
    // Bluetooth SIG company id of the manufacturer data (bluetoothctl's
    // "ManufacturerData.Key" / "ManufacturerData Key"), 0 when none. This is
    // what Bruce reads from the first two bytes of NimBLE's manufacturer data.
    uint16_t mfgr_id = 0;
};

struct ScanResult {
    std::vector<Device> devices; // in order of first appearance
    bool truncated = false;      // more than max_devices were seen
    std::string error;           // bluetoothctl / sudo / shell error lines, "\n"-separated
    // "Discovery started" / "Controller ... Discovering: yes" was seen.
    bool discovery_started = false;
};

struct ParseOptions {
    size_t max_devices = 100; // firmware MAX_DISPLAY_DEVICES (BLE Scan screen)
    // Wardriving: only devices seen during the scan window count. A device
    // counts when a [NEW]/[CHG] line for it follows "Discovery started" /
    // "Controller ... Discovering: yes"; devices bluetoothctl only listed
    // from BlueZ's cache - and every device when discovery never started -
    // are dropped. The BLE Scan screen keeps the default (all devices).
    bool only_seen_in_scan = false;
};

// ANSI CSI/OSC escapes and BlueZ's readline markers removed.
std::string strip_escapes(const std::string& in);

// Devices beyond max_devices are not tracked at all (the BLE Scan screen's
// bound); with only_seen_in_scan the cached entries are tracked up to a fixed
// cap (4 x max_devices, at least 1000) so they cannot crowd out seen ones.
ScanResult parse_scan(const std::string& output, const ParseOptions& options = {});

// RSSI from `bluetoothctl info <mac>`: "RSSI: 0xffffffb4 (-76)" (BlueZ >=
// 5.65) or "RSSI: -76" (older). False when the device reports none.
bool parse_info_rssi(const std::string& output, int& rssi);

// RSSIs from one interactive `bluetoothctl` fed "info <mac>" lines on stdin
// (BlueZ 5.82): each "Device <MAC> (random|public)" block's "RSSI:" line,
// and any "[CHG] Device <MAC> RSSI:" event printed in between (also after a
// prompt). Keyed by the MAC as printed; devices without an RSSI (or with
// BlueZ's 0 / 127 "unknown") are absent.
std::map<std::string, int> parse_info_batch(const std::string& output);

} // namespace platform::ble
