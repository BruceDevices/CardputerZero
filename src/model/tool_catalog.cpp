/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "tool_catalog.h"

namespace model {

// Phosphor-Fill glyph codepoints (official @phosphor-icons "Fill" map, verified
// against the bundled font). Kept here as data so the model owns nothing from
// the view layer; the menu carousel just renders category.icon.
namespace icon {
constexpr const char* kWifi      = ""; // wifi-high
constexpr const char* kBluetooth = ""; // bluetooth
constexpr const char* kRf        = ""; // broadcast
constexpr const char* kNetwork   = ""; // network
constexpr const char* kStorage   = ""; // hard-drives
constexpr const char* kShield    = ""; // shield-warning
constexpr const char* kInfo      = ""; // info
} // namespace icon

const std::vector<ToolCategory>& tool_catalog() {
    static const std::vector<ToolCategory> catalog = {
        {
            "Wi-Fi",
            "Scan, inspect and toggle the Wi-Fi radio",
            icon::kWifi,
            false,
            {
                {"Scan nearby networks", "iw dev wlan0 scan", {"iw", "dev", "wlan0", "scan"}},
                {"Show Wi-Fi status", "ip addr show wlan0", {"ip", "addr", "show", "wlan0"}},
                {"Enable Wi-Fi radio", "rfkill unblock wifi", {"rfkill", "unblock", "wifi"}},
                {"Disable Wi-Fi radio", "rfkill block wifi", {"rfkill", "block", "wifi"}},
            },
        },
        {
            "Bluetooth LE",
            "Discover and manage nearby BLE devices",
            icon::kBluetooth,
            false,
            {
                {"Scan for BLE devices (15s)", "timeout 15 hcitool lescan",
                 {"timeout", "15", "hcitool", "lescan"}},
                {"Show adapter info", "bluetoothctl show", {"bluetoothctl", "show"}},
                {"Enable Bluetooth radio", "rfkill unblock bluetooth", {"rfkill", "unblock", "bluetooth"}},
                {"Disable Bluetooth radio", "rfkill block bluetooth", {"rfkill", "block", "bluetooth"}},
            },
        },
        {
            "Sub-GHz / RF",
            "Requires an attached SDR dongle (e.g. RTL-SDR)",
            icon::kRf,
            false,
            {
                {"Capture 433MHz (30s)", "timeout 30 rtl_433", {"timeout", "30", "rtl_433"}},
                {"List SDR devices", "rtl_test -t", {"rtl_test", "-t"}},
            },
        },
        {
            "Network Tools",
            "curl / ping / netcat helpers",
            icon::kNetwork,
            false,
            {
                {"My public IP (curl)", "curl -s http://ifconfig.me",
                 {"curl", "-s", "http://ifconfig.me"}},
                {"Fetch example.com headers (curl)", "curl -sI http://example.com",
                 {"curl", "-sI", "http://example.com"}},
                {"Ping default gateway", "ping default gateway x4",
                 {"sh", "-c", "ping -c 4 \"$(ip route | awk '/default/ {print $3; exit}')\""}},
                {"Port scan default gateway (nc)", "nc -zv <gateway> 1-1024",
                 {"sh", "-c", "nc -zv \"$(ip route | awk '/default/ {print $3; exit}')\" 1-1024"}},
                {"Start netcat listener :4444", "nc -lvp 4444", {"nc", "-lvp", "4444"}},
            },
        },
        {
            "SMB Client",
            "Anonymous SMB share discovery",
            icon::kStorage,
            false,
            {
                {"List shares on default gateway", "smbclient -L //<gateway> -N",
                 {"sh", "-c", "smbclient -L \"//$(ip route | awk '/default/ {print $3; exit}')\" -N"}},
                {"smbclient version", "smbclient --version", {"smbclient", "--version"}},
            },
        },
        {
            "Responder",
            "LLMNR/NBT-NS/mDNS poisoner - authorized use only",
            icon::kShield,
            false,
            {
                {"Start Responder on wlan0", "responder -I wlan0", {"responder", "-I", "wlan0"}},
                {"Start Responder on eth0", "responder -I eth0", {"responder", "-I", "eth0"}},
            },
        },
        {
            "About",
            "Version and usage notice",
            icon::kInfo,
            true,
            {},
        },
    };
    return catalog;
}

} // namespace model
