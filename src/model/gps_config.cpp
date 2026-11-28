/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gps_config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace model::gps_config {
namespace {

GpsConfig& board_default_storage() {
    static GpsConfig config = board_default(false);
    return config;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_int(const std::string& s, int& out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0' || v < 0 || v > 100000000L) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

std::string home_dir() {
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return home;
    }
    return "/root";
}

} // namespace

const std::vector<int>& baud_rates() {
    static const std::vector<int> rates = {9600, 19200, 38400, 57600, 115200};
    return rates;
}

bool is_auto(const std::string& port) {
    return port == kAutoPort;
}

int validate_baud(int baud) {
    for (const int b : baud_rates()) {
        if (b == baud) {
            return baud;
        }
    }
    return 9600;
}

bool valid_port(const std::string& port) {
    if (is_auto(port)) {
        return true;
    }
    if (port.size() < 2 || port[0] != '/' || port.size() > 255) {
        return false;
    }
    for (const char c : port) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) {
            return false;
        }
    }
    return true;
}

GpsConfig board_default(bool cap_lora) {
    GpsConfig c;
    c.port = "/dev/serial0";
    c.baud = cap_lora ? 115200 : 9600;
    return c;
}

void set_board_default(const GpsConfig& config) {
    board_default_storage() = config;
}

const GpsConfig& current_board_default() {
    return board_default_storage();
}

GpsConfig parse(const std::string& text, const GpsConfig& defaults) {
    GpsConfig c = defaults;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        if (key == "gpsBaudrate") {
            int b = 0;
            // Present but unusable -> Bruce's validation fallback (9600).
            c.baud = parse_int(value, b) ? validate_baud(b) : 9600;
        } else if (key == "gpsPort") {
            if (valid_port(value)) {
                c.port = value;
            }
        }
    }
    return c;
}

std::string serialize(const GpsConfig& config) {
    return "# Bruce CardputerZero GPS settings (GPS > Config)\n"
           "gpsBaudrate=" + std::to_string(validate_baud(config.baud)) + "\n"
           "gpsPort=" + config.port + "\n";
}

std::string config_path() {
    if (const char* env = std::getenv("BRUCE_GPS_CONF"); env && env[0] != '\0') {
        return env;
    }
    return home_dir() + "/.bruce/gps.conf";
}

GpsConfig load(const GpsConfig& defaults) {
    std::ifstream in(config_path());
    if (!in) {
        return defaults;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), defaults);
}

GpsConfig load() {
    return load(current_board_default());
}

bool save(const GpsConfig& config, std::string* err) {
    if (!valid_port(config.port)) {
        if (err) {
            *err = "invalid GPS port: " + config.port;
        }
        return false;
    }
    const std::string path = config_path();
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = path.substr(0, slash);
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            if (err) {
                *err = "cannot create " + dir + ": " + std::strerror(errno);
            }
            return false;
        }
    }
    const std::string tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << serialize(config);
        out.flush();
        if (!out) {
            const int e = errno;
            std::remove(tmp.c_str());
            if (err) {
                *err = "cannot write " + tmp + ": " + std::strerror(e);
            }
            return false;
        }
    }
    ::chmod(tmp.c_str(), 0644);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        const int e = errno;
        std::remove(tmp.c_str());
        if (err) {
            *err = "cannot replace " + path + ": " + std::strerror(e);
        }
        return false;
    }
    return true;
}

Effective resolve(const GpsConfig& config) {
    Effective e;
    e.port = config.port;
    e.baud = validate_baud(config.baud);
    if (const char* env = std::getenv("GPS_UART"); env && env[0] != '\0') {
        e.port = env;
        e.port_from_env = true;
    }
    if (const char* env = std::getenv("GPS_BAUD"); env && std::atoi(env) > 0) {
        e.baud = std::atoi(env);
        e.baud_from_env = true;
    }
    return e;
}

} // namespace model::gps_config
