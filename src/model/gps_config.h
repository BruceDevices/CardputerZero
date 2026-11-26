/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * GPS settings - the CardputerZero stand-in for Bruce firmware's
 * `bruceConfigPins.gpsBaudrate` + `gps_bus` (brucePins.conf). Pure data and
 * file I/O, no LVGL / hardware, so the menu (model), the GPS screens (view)
 * and the CLI front ends share one source of truth.
 *
 * Storage: ~/.bruce/gps.conf, key=value (the ~/.bruce/ui.conf precedent; there
 * is no JSON library), with Bruce's key name kept:
 *
 *     gpsBaudrate=115200
 *     gpsPort=/dev/serial0        # or "Auto"
 *
 * Precedence for the port/baud a GPS feature actually uses (resolve()):
 *     $GPS_UART / $GPS_BAUD  >  ~/.bruce/gps.conf  >  board default
 * The environment variables stay a developer/test override; the menu's
 * Config screens edit only the file.
 */

#pragma once

#include <string>
#include <vector>

namespace model::gps_config {

// Bruce's Baudrate menu, in order (settings.cpp setGpsBaudrateMenu).
const std::vector<int>& baud_rates();

// The gpsPort value that makes the GPS features probe the candidate ports for
// NMEA instead of opening one fixed node (CardputerZero extension).
constexpr const char* kAutoPort = "Auto";
bool is_auto(const std::string& port);

struct GpsConfig {
    int baud = 9600;
    std::string port = "/dev/serial0";

    bool operator==(const GpsConfig& o) const { return baud == o.baud && port == o.port; }
    bool operator!=(const GpsConfig& o) const { return !(*this == o); }
};

// BruceConfigPins::validateGpsBaudrateValue(): anything but the five menu
// values becomes 9600.
int validate_baud(int baud);

// A port value is kept when it is "Auto" or an absolute device path without
// whitespace/control characters; anything else falls back to the default.
bool valid_port(const std::string& port);

// Board defaults (Bruce: boards/<board>/interface.cpp): the LoRa+GNSS Cap's
// ATGM336H talks 115200 on the HAT UART; any other board gets Bruce's 9600.
GpsConfig board_default(bool cap_lora);

// The process-wide board default, set once by the app after CAP detection
// (defaults to board_default(false) until then).
void set_board_default(const GpsConfig& config);
const GpsConfig& current_board_default();

// key=value text -> config. Unknown keys, comments (#) and blank lines are
// ignored; a missing or invalid value keeps `defaults`' value, except that a
// present-but-unknown baud becomes 9600 as in Bruce.
GpsConfig parse(const std::string& text, const GpsConfig& defaults);
std::string serialize(const GpsConfig& config);

// $BRUCE_GPS_CONF (tests), else $HOME/.bruce/gps.conf.
std::string config_path();

// The saved config, or `defaults` when the file is missing/unreadable.
GpsConfig load(const GpsConfig& defaults);
GpsConfig load(); // with current_board_default()

// Writes the file (creating ~/.bruce 0755 if needed) via a temp file + rename,
// mode 0644, owned by whoever runs the app. Returns false with `err` set on
// failure.
bool save(const GpsConfig& config, std::string* err = nullptr);

// What a GPS feature should open: the saved config with $GPS_UART / $GPS_BAUD
// applied on top (a positive GPS_BAUD is taken as is - it is a test hook).
struct Effective {
    std::string port;
    int baud = 9600;
    bool port_from_env = false;
    bool baud_from_env = false;
};
Effective resolve(const GpsConfig& config);

} // namespace model::gps_config
