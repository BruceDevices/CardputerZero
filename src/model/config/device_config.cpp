/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "device_config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace model {
namespace config {
namespace {

std::string config_dir() {
    if (const char* env = std::getenv("BRUCE_CONFIG_DIR"); env && env[0] != '\0') return env;
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/.bruce";
    }
    return "/root/.bruce";
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// Emit "prefix" lines for one SPI module.
void spi_to_kv(std::ostream& os, const std::string& p, const SpiModule& m) {
    os << p << ".spidev=" << m.spidev << "\n";
    os << p << ".aux0.chip=" << m.aux0.chip << "\n" << p << ".aux0.line=" << m.aux0.offset << "\n";
    os << p << ".aux1.chip=" << m.aux1.chip << "\n" << p << ".aux1.line=" << m.aux1.offset << "\n";
    os << p << ".aux2.chip=" << m.aux2.chip << "\n" << p << ".aux2.line=" << m.aux2.offset << "\n";
}

} // namespace

std::string DeviceConfig::file_path() { return config_dir() + "/pins.conf"; }

void DeviceConfig::reset_defaults() { *this = DeviceConfig{}; }

std::string DeviceConfig::serialize() const {
    std::ostringstream os;
    os << "# Bruce CardputerZero device pin/bus configuration\n";
    os << "# Edited via the Config menu; GDO/aux lines are libgpiod chip+offset.\n";

    spi_to_kv(os, "cc1101", cc1101);
    os << "cc1101.freq_mhz=" << cc1101_freq_mhz << "\n";
    os << "cc1101.scan_range=" << cc1101_scan_range << "\n";

    spi_to_kv(os, "lora", lora);
    spi_to_kv(os, "nrf24", nrf24);
    spi_to_kv(os, "pn532_spi", pn532_spi);
    spi_to_kv(os, "st25r", st25r);

    os << "pn532_i2c.device=" << pn532_i2c.device << "\n";
    os << "pn532_i2c.address=" << pn532_i2c.address << "\n";
    os << "gps.device=" << gps.device << "\n";
    os << "gps.baud=" << gps.baud << "\n";
    os << "ir.tx.chip=" << ir.tx.chip << "\n" << "ir.tx.line=" << ir.tx.offset << "\n";
    os << "ir.rx.chip=" << ir.rx.chip << "\n" << "ir.rx.line=" << ir.rx.offset << "\n";
    return os.str();
}

void DeviceConfig::parse(const std::string& text) {
    // Resolve a "module.field[.sub]" key against the known modules. Keeping this
    // as an explicit dispatch (rather than reflection) mirrors BruceConfigPins'
    // fromJson and stays obvious as modules are added.
    auto apply_spi = [](SpiModule& m, const std::string& field, const std::string& val) -> bool {
        if (field == "spidev") { m.spidev = val; return true; }
        if (field == "aux0.chip") { m.aux0.chip = val; return true; }
        if (field == "aux0.line") { m.aux0.offset = std::atoi(val.c_str()); return true; }
        if (field == "aux1.chip") { m.aux1.chip = val; return true; }
        if (field == "aux1.line") { m.aux1.offset = std::atoi(val.c_str()); return true; }
        if (field == "aux2.chip") { m.aux2.chip = val; return true; }
        if (field == "aux2.line") { m.aux2.offset = std::atoi(val.c_str()); return true; }
        return false;
    };

    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));

        const size_t dot = key.find('.');
        if (dot == std::string::npos) continue;
        const std::string mod = key.substr(0, dot);
        const std::string field = key.substr(dot + 1);

        if (mod == "cc1101") {
            if (field == "freq_mhz") cc1101_freq_mhz = std::atof(val.c_str());
            else if (field == "scan_range") cc1101_scan_range = std::atoi(val.c_str());
            else apply_spi(cc1101, field, val);
        } else if (mod == "lora") {
            apply_spi(lora, field, val);
        } else if (mod == "nrf24") {
            apply_spi(nrf24, field, val);
        } else if (mod == "pn532_spi") {
            apply_spi(pn532_spi, field, val);
        } else if (mod == "st25r") {
            apply_spi(st25r, field, val);
        } else if (mod == "pn532_i2c") {
            if (field == "device") pn532_i2c.device = val;
            else if (field == "address") pn532_i2c.address = std::atoi(val.c_str());
        } else if (mod == "gps") {
            if (field == "device") gps.device = val;
            else if (field == "baud") gps.baud = std::atoi(val.c_str());
        } else if (mod == "ir") {
            if (field == "tx.chip") ir.tx.chip = val;
            else if (field == "tx.line") ir.tx.offset = std::atoi(val.c_str());
            else if (field == "rx.chip") ir.rx.chip = val;
            else if (field == "rx.line") ir.rx.offset = std::atoi(val.c_str());
        }
    }
}

bool DeviceConfig::load() {
    std::ifstream file(file_path(), std::ios::binary);
    if (!file) return false;
    std::ostringstream buf;
    buf << file.rdbuf();
    parse(buf.str());
    return true;
}

bool DeviceConfig::save() const {
#if defined(__linux__)
    // Best-effort mkdir of the config directory (mode 0700); ignore EEXIST.
    ::mkdir(config_dir().c_str(), 0700);
#endif
    std::ofstream file(file_path(), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << serialize();
    return static_cast<bool>(file);
}

DeviceConfig& device_config() {
    static DeviceConfig instance = [] {
        DeviceConfig c;
        c.load(); // missing file keeps defaults
        return c;
    }();
    return instance;
}

} // namespace config
} // namespace model
