/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the tool catalog data. Not part of the CMake
 * build (the model lib globs src/model only). Run manually:
 *
 *   g++ -std=c++17 -Isrc/model src/model/tool_catalog.cpp src/model/gps_config.cpp \
 *       tests/tool_catalog_selfcheck.cpp -o /tmp/catalog_check && /tmp/catalog_check
 */

#include "tool_catalog.h"
#include "gps_config.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main() {
    const auto& catalog = model::tool_catalog();
    assert(!catalog.empty());

    int info_only_count = 0;
    for (const auto& category : catalog) {
        assert(category.label != nullptr && category.label[0] != '\0');
        assert(category.description != nullptr);

        if (category.info_only) {
            // Info categories (About) open a static screen, so they must carry
            // no actions - MenuScreen would otherwise list an empty category.
            ++info_only_count;
            assert(category.actions.empty());
            continue;
        }

        // A runnable category must have at least one action, and every action
        // must have a non-empty label. An empty argv is a navigation row
        // (Theme accents, Nmap host picks, Evil Portal clone picks); anything
        // else must carry a launchable argv[0].
        assert(!category.actions.empty());
        for (const auto& action : category.actions) {
            assert(action.label != nullptr && action.label[0] != '\0');
            if (action.argv.empty()) {
                continue; // navigation row
            }
            assert(!action.argv[0].empty());
        }
    }

    // Exactly one info-only category (About) is expected today.
    assert(info_only_count == 1);

    // Terminal category: exactly two view rows, both opening TerminalScreen,
    // neither a launcher nor a root-flagged catalog row (root is chosen per
    // typed command via the leading "sudo " prefix inside TerminalScreen).
    const model::ToolCategory* terminal = nullptr;
    for (const auto& category : catalog) {
        if (std::string(category.label) == "Terminal") {
            terminal = &category;
        }
    }
    assert(terminal && terminal->actions.size() == 2);
    for (const auto& action : terminal->actions) {
        assert(action.terminal_view);
        assert(!action.needs_root);
        assert(!action.argv.empty()); // harmless fallback print, never launched by MenuScreen
    }

    // BLE Scan opens the dedicated firmware-style list/detail screen, and its
    // fallback argv is the shell scan script (the screen builds its own 5s
    // bluetoothctl command, but the row must still be a runnable root row).
    for (const auto& category : catalog) {
        if (std::string(category.label) != "Bluetooth LE") continue;
        assert(!category.actions.empty());
        assert(category.actions[0].ble_scan_view);
        assert(category.actions[0].needs_root);
        assert(category.actions[0].argv.size() == 3);
        assert(category.actions[0].argv[0] == "sh");
        assert(category.actions[0].argv[1] == "-c");
    }

    // Terminal command routing (what TerminalScreen spawns). User commands -
    // and root commands when the app itself is root - run as plain `sh -c`.
    // Root commands for a non-root app ride the sudoers-authorized binary's
    // `--root-sh` passthrough; the flag set follows terminal_auth_mode().
    {
        const std::vector<std::string> user = model::terminal_command_argv("df -h", false);
        assert(user.size() == 3 && user[0] == "sh" && user[1] == "-c" && user[2] == "df -h");

        if (::geteuid() == 0) {
            const std::vector<std::string> root = model::terminal_command_argv("id -u", true);
            assert(root.size() == 3 && root[0] == "sh" && root[1] == "-c" && root[2] == "id -u");
            assert(model::terminal_auth_mode() == model::TerminalAuth::NotNeeded);
        } else {
            char exe[4096];
            const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            assert(n > 0);
            exe[n] = '\0';
            const std::string self(exe);
            const std::vector<std::string> root = model::terminal_command_argv("id -u", true);
            assert(root[0] == "sudo" && root.back() == "id -u");
            const bool passwordless = model::terminal_auth_mode() == model::TerminalAuth::Passwordless;
            if (passwordless) {
                assert(root.size() == 5 && root[1] == "-n" && root[2] == self &&
                       root[3] == "--root-sh" && root[4] == "id -u");
            } else {
                assert(model::terminal_auth_mode() == model::TerminalAuth::Password);
                assert(root.size() == 7 && root[1] == "-S" &&
                       root[2] == "-p" && root[3] == "" &&
                       root[4] == self && root[5] == "--root-sh" && root[6] == "id -u");
            }
        }
    }

    // Launch routing (what ToolRunScreen actually spawns). As root the wrap is
    // skipped, so only check the escalation shapes for a non-root app - the
    // APPLaunch normal case:
    //   native re-execs            -> sudo -n <self> <args...>
    //   other needs_root rows      -> sudo -n <self> --root-sh <script>
    //   everything else            -> unchanged
    // The sudoers rule authorizes exactly this binary, so `sudo sh` must never
    // appear; the Evil Portal clone flow depends on this routing.
    if (::geteuid() != 0) {
        char exe[4096];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        assert(n > 0);
        exe[n] = '\0';
        const std::string self(exe); // self_exe_path() resolves per-process
        for (const auto& category : catalog) {
            for (const auto& action : category.actions) {
                if (action.argv.empty()) continue; // navigation row
                const std::vector<std::string> launched = model::launch_argv(action);
                assert(!launched.empty());
                if (action.argv[0] == self) {
                    assert(launched.size() == action.argv.size() + 2);
                    assert(launched[0] == "sudo" && launched[1] == "-n");
                } else if (action.needs_root) {
                    assert(launched.size() == 5);
                    assert(launched[0] == "sudo" && launched[1] == "-n");
                    assert(launched[2] == self && launched[3] == "--root-sh");
                    if (action.argv[0] == "sh" && action.argv.size() >= 3) {
                        assert(launched[4] == action.argv[2]); // script passes verbatim
                    } else {
                        assert(launched[4].rfind("exec '", 0) == 0); // quoted argv
                    }
                } else {
                    assert(launched == action.argv);
                }

                // Password-prompt path (sudo -S): same gating and command tail
                // as launch_argv, only the sudo flag set differs.
                const std::vector<std::string> with_pw = model::sudo_argv_with_password(action);
                if (launched[0] != "sudo") {
                    assert(with_pw == launched); // unflagged row: no escalation either way
                } else if (launched[3] == "--root-sh") {
                    assert(with_pw.size() == 7);
                    assert(with_pw[0] == "sudo" && with_pw[1] == "-S" &&
                           with_pw[2] == "-p" && with_pw[3] == "");
                    assert(with_pw[4] == launched[2] && with_pw[5] == launched[3] &&
                           with_pw[6] == launched[4]);
                } else {
                    assert(with_pw.size() == launched.size() + 2);
                    assert(with_pw[0] == "sudo" && with_pw[1] == "-S" &&
                           with_pw[2] == "-p" && with_pw[3] == "");
                    assert(std::vector<std::string>(with_pw.begin() + 4, with_pw.end()) ==
                           std::vector<std::string>(launched.begin() + 2, launched.end()));
                }
                assert(model::action_needs_root(action) == (launched[0] == "sudo"));
            }
        }
    }

    // GPS: Bruce's GpsMenu tree, no gpsd anywhere.
    {
        namespace gc = model::gps_config;
        const std::string conf = "/tmp/bruce_gps_catalog_" + std::to_string(::getpid()) + ".conf";
        std::remove(conf.c_str());
        ::setenv("BRUCE_GPS_CONF", conf.c_str(), 1);
        ::unsetenv("GPS_UART");
        ::unsetenv("GPS_BAUD");

        int gps_idx = -1;
        const model::ToolAction* install = nullptr;
        for (size_t c = 0; c < catalog.size(); ++c) {
            if (std::string(catalog[c].label) == "GPS") gps_idx = static_cast<int>(c);
            for (const auto& action : catalog[c].actions) {
                if (std::string(action.label) == "Install optional tools (apt)") install = &action;
                for (const auto& word : action.argv) {
                    assert(word.find("gpspipe") == std::string::npos);
                    assert(word.find("gpsd") == std::string::npos);
                }
                assert(std::string(action.description).find("gpsd") == std::string::npos);
            }
        }
        assert(gps_idx >= 0 && install);
        assert(install->needs_root && install->argv.size() == 3);
        assert(install->argv[2].find("gpsd") == std::string::npos);
        const auto& gps = catalog[static_cast<size_t>(gps_idx)];
        assert(std::string(gps.description) == "Wardriving, GPS Tracker and GPS config");

        auto labels = [&]() {
            model::refresh_dynamic_categories(gps_idx);
            std::vector<std::string> out;
            for (const auto& a : model::tool_catalog()[static_cast<size_t>(gps_idx)].actions) out.push_back(a.label);
            return out;
        };
        auto row = [&](size_t i) -> const model::ToolAction& {
            return model::tool_catalog()[static_cast<size_t>(gps_idx)].actions[i];
        };
        auto read_conf = [&]() {
            std::string text;
            if (FILE* f = std::fopen(conf.c_str(), "r")) {
                char buf[256];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
                std::fclose(f);
            }
            return text;
        };
        using V = std::vector<std::string>;

        // Root (GpsMenu::optionsMenu): header follows the configured baud.
        assert(labels() == (V{"Wardriving", "GPS Tracker", "Config", "Main Menu"}));
        assert(model::category_header(gps_idx) == "GPS (9600 bps)"); // Bruce default
        gc::set_board_default(gc::board_default(/*cap_lora=*/true));
        assert(model::category_header(gps_idx) == "GPS (115200 bps)");
        assert(model::category_header(0) == catalog[0].label);
        assert(row(1).gps_app == model::GpsApp::Tracker);
        assert(!row(1).needs_root && !model::action_needs_root(row(1))); // user-owned GPX files
        assert(row(0).argv.empty() && row(2).argv.empty() && row(3).argv.empty());
        assert(!model::on_category_back(gps_idx));       // root: Back leaves the category
        assert(model::on_navigation_action(gps_idx, 3)); // "Main Menu" leaves too
        assert(labels()[0] == "Wardriving");

        // Wardriving submenu -> Bruce's Wardriving on GpsAppScreen: the root
        // child `--wardrive <mode> --ui` with the configured port/baud, the
        // app user's home (never the root child's own $HOME) and the Wi-Fi
        // interface in argv.
        assert(!model::on_navigation_action(gps_idx, 0));
        assert(model::category_header(gps_idx) == "Wardriving");
        assert(labels() == (V{"Scan WiFi Networks", "Scan BLE Devices", "Scan Both", "Back"}));
        const char* modes[] = {"wifi", "ble", "both"};
        const std::string home_env = std::getenv("HOME") ? std::getenv("HOME") : "";
        for (size_t i = 0; i < 3; ++i) {
            const auto& a = row(i);
            assert(a.argv.size() == 12 && a.argv[1] == "--wardrive" && a.argv[2] == modes[i]);
            assert(a.argv[3] == "--ui");
            assert(a.argv[4] == "--port" && a.argv[5] == "/dev/serial0");
            assert(a.argv[6] == "--baud" && a.argv[7] == "115200");
            assert(a.argv[8] == "--home" && !a.argv[9].empty());
            assert(home_env.empty() || a.argv[9] == home_env);
            assert(a.argv[10] == "--iface" && a.argv[11] == model::wifi_interface());
            assert(a.needs_root && model::action_needs_root(a) == (::geteuid() != 0));
            assert(a.gps_app == model::GpsApp::Wardriving);
        }
        {
            // HOME unset: the passwd entry of the app user, not "".
            const std::string saved = home_env;
            ::unsetenv("HOME");
            labels();
            assert(!row(0).argv[9].empty());
            if (!saved.empty()) ::setenv("HOME", saved.c_str(), 1);
            labels();
        }
        ::setenv("GPS_UART", "/dev/ttyUSB7", 1); // env override wins over the config
        labels();
        assert(row(0).argv[5] == "/dev/ttyUSB7");
        ::unsetenv("GPS_UART");
        assert(model::on_category_back(gps_idx)); // Back key -> GPS menu
        assert(labels()[0] == "Wardriving");
        model::on_navigation_action(gps_idx, 0);
        labels();
        model::on_navigation_action(gps_idx, 3); // "Back" row
        assert(labels()[3] == "Main Menu");

        // Config > Baudrate: Bruce's five values, the current one marked; a
        // pick is saved at once.
        model::on_navigation_action(gps_idx, 2);
        assert(model::category_header(gps_idx) == "GPS Config");
        assert(labels() == (V{"Baudrate", "GPS Port", "Back"}));
        model::on_navigation_action(gps_idx, 0);
        assert(model::category_header(gps_idx) == "Baudrate");
        assert(labels() == (V{"9600 bps", "19200 bps", "38400 bps", "57600 bps", "115200 bps  *"}));
        assert(model::preferred_selection(gps_idx) == 4);
        assert(model::on_category_back(gps_idx)); // Back -> Config
        assert(labels()[0] == "Baudrate");
        model::on_navigation_action(gps_idx, 0);
        labels();
        assert(!model::on_navigation_action(gps_idx, 0)); // 9600
        assert(read_conf().find("gpsBaudrate=9600\n") != std::string::npos);
        assert(model::category_header(gps_idx) == "GPS (9600 bps)"); // back on the GPS menu
        assert(labels()[0] == "Wardriving");
        model::on_navigation_action(gps_idx, 0);
        assert(row((labels(), 0)).argv[7] == "9600");
        model::on_category_back(gps_idx);

        // Config > GPS Port: "Port = ...", "Save Config" disabled until changed.
        model::refresh_gps_category({"/dev/ttyACM0", "/dev/serial/by-id/usb-u-blox_GNSS-if00"});
        model::on_navigation_action(gps_idx, 2);
        labels();
        model::on_navigation_action(gps_idx, 1);
        assert(model::category_header(gps_idx) == "GPS Port");
        assert(labels() == (V{"Port = /dev/serial0", "Save Config", "Main Menu"}));
        assert(row(1).dimmed && !row(0).dimmed);
        assert(!model::on_navigation_action(gps_idx, 1)); // disabled: nothing happens
        assert(labels()[1] == "Save Config");
        model::on_navigation_action(gps_idx, 0); // the picker
        assert(model::category_header(gps_idx) == "Select GPS Port");
        assert(labels() == (V{"Auto", "/dev/ttyACM0", "/dev/serial/by-id/usb-u-blox_GNSS-if00",
                              "/dev/serial0 (not present)"}));
        assert(model::preferred_selection(gps_idx) == 3);
        model::on_navigation_action(gps_idx, 0); // Auto
        assert(labels() == (V{"Port = Auto", "Save Config", "Main Menu"}));
        assert(!row(1).dimmed);
        assert(model::on_category_back(gps_idx)); // Back discards -> Config
        model::on_navigation_action(gps_idx, 1);
        assert(labels()[0] == "Port = /dev/serial0"); // nothing was saved
        model::on_navigation_action(gps_idx, 0);
        labels();
        model::on_navigation_action(gps_idx, 0); // Auto again
        labels();
        assert(model::on_navigation_action(gps_idx, 2)); // "Main Menu": leave, discard
        assert(read_conf().find("gpsPort=/dev/serial0\n") != std::string::npos);
        assert(labels()[0] == "Wardriving");
        model::on_navigation_action(gps_idx, 2);
        labels();
        model::on_navigation_action(gps_idx, 1);
        labels();
        model::on_navigation_action(gps_idx, 0);
        labels();
        model::on_navigation_action(gps_idx, 1); // /dev/ttyACM0
        assert(labels()[0] == "Port = /dev/ttyACM0");
        assert(!model::on_navigation_action(gps_idx, 1)); // Save Config
        assert(read_conf() == "# Bruce CardputerZero GPS settings (GPS > Config)\n"
                              "gpsBaudrate=9600\ngpsPort=/dev/ttyACM0\n");
        assert(labels()[0] == "Wardriving"); // saved -> GPS menu
        model::on_navigation_action(gps_idx, 0);
        assert(row((labels(), 0)).argv[5] == "/dev/ttyACM0");
        model::on_category_back(gps_idx);
        labels();

        // The header shows the baud the features will really use: $GPS_BAUD
        // over the saved 9600 (the Baudrate list still marks the saved one).
        ::setenv("GPS_BAUD", "57600", 1);
        assert(model::category_header(gps_idx) == "GPS (57600 bps)");
        ::unsetenv("GPS_BAUD");
        assert(model::category_header(gps_idx) == "GPS (9600 bps)");
        assert(model::take_navigation_error(gps_idx).empty());

        // A save that fails (the config's parent is a regular file -> ENOTDIR,
        // also as root): an error for the popup, the edited port kept and
        // "Save Config" still enabled; fixing the path and saving again works.
        const std::string bad = conf + "/gps.conf";
        ::setenv("BRUCE_GPS_CONF", bad.c_str(), 1);
        model::on_navigation_action(gps_idx, 2); // Config
        labels();
        model::on_navigation_action(gps_idx, 1); // GPS Port
        assert(labels()[0] == "Port = /dev/serial0"); // unreadable config -> board default
        model::on_navigation_action(gps_idx, 0);
        labels();
        model::on_navigation_action(gps_idx, 2); // the by-id node
        assert(labels()[0] == "Port = /dev/serial/by-id/usb-u-blox_GNSS-if00");
        assert(!model::on_navigation_action(gps_idx, 1)); // Save Config -> fails
        const std::string err = model::take_navigation_error(gps_idx);
        assert(err == "Cannot save GPS config: Not a directory");
        assert(model::take_navigation_error(gps_idx).empty()); // taken once
        assert(model::category_header(gps_idx) == "GPS Port");  // not sent back to the GPS menu
        assert(labels()[0] == "Port = /dev/serial/by-id/usb-u-blox_GNSS-if00");
        assert(!row(1).dimmed); // retry possible
        ::setenv("BRUCE_GPS_CONF", conf.c_str(), 1);
        assert(!model::on_navigation_action(gps_idx, 1)); // retry -> saved
        assert(model::take_navigation_error(gps_idx).empty());
        assert(read_conf().find("gpsPort=/dev/serial/by-id/usb-u-blox_GNSS-if00\n") != std::string::npos);
        assert(labels()[0] == "Wardriving");
        // Baudrate: a failed save stays on the list with the error.
        ::setenv("BRUCE_GPS_CONF", bad.c_str(), 1);
        model::on_navigation_action(gps_idx, 2);
        labels();
        model::on_navigation_action(gps_idx, 0);
        labels();
        assert(!model::on_navigation_action(gps_idx, 1)); // 19200
        assert(model::take_navigation_error(gps_idx).rfind("Cannot save GPS config: ", 0) == 0);
        assert(model::category_header(gps_idx) == "Baudrate");
        ::setenv("BRUCE_GPS_CONF", conf.c_str(), 1);
        assert(read_conf().find("gpsBaudrate=9600\n") != std::string::npos); // unchanged
        model::on_category_back(gps_idx);
        model::on_category_back(gps_idx);
        labels();

        std::remove(conf.c_str());
        ::unsetenv("BRUCE_GPS_CONF");
    }

    std::printf("tool_catalog self-check passed: %zu categories\n", catalog.size());
    return 0;
}
