/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "tool_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace model {
namespace {

// The "Use CC1101 CAP" action re-execs this very binary with --cc1101-detect.
// The app installs outside PATH (/usr/share/APPLaunch/bin), so resolve the
// running executable instead of relying on execvp's PATH search; fall back to
// the bare name if /proc is unavailable.
std::string self_exe_path() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return "bruce_cardputerzero";
    }
    buf[n] = '\0';
    return std::string(buf);
}

// Where user scripts live: $BRUCE_SCRIPTS_DIR, else ~/.bruce/scripts.
std::string scripts_dir() {
    if (const char* env = std::getenv("BRUCE_SCRIPTS_DIR"); env && env[0] != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/.bruce/scripts";
    }
    return "/root/.bruce/scripts";
}

// ToolAction stores label/description as const char*, so dynamically discovered
// script names need storage that outlives catalog init. A function-local deque
// (stable addresses across push_back, unlike vector) owns them for the process
// lifetime. ponytail: scan runs once at first catalog access, so new scripts
// need an app restart - fine for v1; make the catalog rebuildable if that itches.
std::vector<ToolAction> scan_scripts() {
    static std::deque<std::string> pool;
    const std::string dir = scripts_dir();

    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* entry = readdir(d)) {
            if (entry->d_name[0] == '.') {
                continue; // skip ., .., and dotfiles
            }
            const std::string path = dir + "/" + entry->d_name;
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            if ((st.st_mode & S_IXUSR) == 0) {
                continue; // executable bit required - run via the script's shebang
            }
            names.emplace_back(entry->d_name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());

    std::vector<ToolAction> actions;
    for (const auto& name : names) {
        pool.push_back(name);
        const char* label = pool.back().c_str();
        pool.push_back(dir + "/" + name);
        const std::string& full = pool.back();
        actions.push_back(ToolAction{label, full.c_str(), {full}});
    }

    if (actions.empty()) {
        // The menu can't show an empty runnable category, so guide the user.
        pool.push_back("No scripts yet");
        const char* label = pool.back().c_str();
        pool.push_back("Add an executable script (shebang + chmod +x) to " + dir);
        const char* desc = pool.back().c_str();
        actions.push_back(ToolAction{
            label, desc,
            {"sh", "-c",
             "echo 'No executable scripts found in " + dir + "'; "
             "echo 'Add a script with a shebang, chmod +x, then restart Bruce.'"}});
    }

    return actions;
}

} // namespace

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
constexpr const char* kI2c       = ""; // circuitry
constexpr const char* kSpi       = ""; // memory
constexpr const char* kUart      = ""; // terminal-window
constexpr const char* kOneWire   = ""; // git-commit
constexpr const char* kGpio      = ""; // cpu
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
            "M5 CAP CC1101 over SPI, or an SDR dongle (e.g. RTL-SDR)",
            icon::kRf,
            false,
            {
                {"Use CC1101 CAP (detect)",
                 "Select the M5 CAP CC1101: probe it over spidev and confirm the "
                 "chip responds before using RF tools.",
                 {self_exe_path(), "--cc1101-detect"}},
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
            "I2C Bus",
            "SDA=G2 SCL=G3 - scan and read I2C devices (i2c-tools)",
            icon::kI2c,
            false,
            {
                {"List I2C buses", "i2cdetect -l", {"i2cdetect", "-l"}},
                {"Scan bus 1", "i2cdetect -y 1", {"i2cdetect", "-y", "1"}},
                {"Dump 0x50 (EEPROM)", "i2cdump -y 1 0x50", {"i2cdump", "-y", "1", "0x50"}},
                {"Read 0x50 reg 0x00", "i2cget -y 1 0x50 0x00", {"i2cget", "-y", "1", "0x50", "0x00"}},
            },
        },
        {
            "SPI Bus",
            "spidev0.0 - probe/read SPI flash (flashrom)",
            icon::kSpi,
            false,
            {
                {"List SPI devices", "ls /dev/spidev*",
                 {"sh", "-c", "ls -l /dev/spidev* 2>/dev/null || echo 'no /dev/spidev* (enable SPI overlay)'"}},
                {"Probe SPI flash", "flashrom -p linux_spi:dev=/dev/spidev0.0",
                 {"flashrom", "-p", "linux_spi:dev=/dev/spidev0.0,spispeed=2000"}},
                {"Read flash -> /tmp/spi.bin", "flashrom ... -r /tmp/spi.bin",
                 {"flashrom", "-p", "linux_spi:dev=/dev/spidev0.0,spispeed=2000", "-r", "/tmp/spi.bin"}},
            },
        },
        {
            "UART / Serial",
            "TX=G14 RX=G15 - monitor a serial line (read only)",
            icon::kUart,
            false,
            {
                {"List serial ports", "ls /dev/ttyS* ...",
                 {"sh", "-c", "ls -l /dev/ttyS* /dev/ttyAMA* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo 'no serial ports found'"}},
                {"Monitor ttyS0 @115200", "stty 115200; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 115200 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @9600", "stty 9600; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 9600 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
            },
        },
        {
            "1-Wire",
            "needs w1-gpio overlay - list/read 1-Wire slaves",
            icon::kOneWire,
            false,
            {
                {"List 1-Wire devices", "ls /sys/bus/w1/devices/",
                 {"sh", "-c", "ls /sys/bus/w1/devices/ 2>/dev/null || echo 'w1 not loaded (needs w1-gpio overlay)'"}},
                {"Read all slaves", "cat .../w1_slave",
                 {"sh", "-c", "for d in /sys/bus/w1/devices/*/w1_slave; do [ -e \"$d\" ] && { echo \"== $d ==\"; cat \"$d\"; }; done"}},
            },
        },
        {
            "GPIO / DIO",
            "G7/G9/G10/G11/G22/G23/G26 - libgpiod (verify offsets: G# != line#)",
            icon::kGpio,
            false,
            {
                {"Detect gpiochips", "gpiodetect", {"gpiodetect"}},
                {"Info gpiochip0", "gpioinfo gpiochip0", {"gpioinfo", "gpiochip0"}},
                {"Read line 7", "gpioget gpiochip0 7", {"gpioget", "gpiochip0", "7"}},
                {"Set line 7 = 1 (5s)", "gpioset --mode=time --sec=5 ... 7=1",
                 {"gpioset", "--mode=time", "--sec=5", "gpiochip0", "7=1"}},
                {"Monitor line 7", "gpiomon gpiochip0 7", {"gpiomon", "gpiochip0", "7"}},
            },
        },
        {
            "Scripts",
            "Run your own executable scripts from ~/.bruce/scripts (GPIO, RF, net...)",
            icon::kUart,
            false,
            scan_scripts(),
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
