/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the tool catalog data. Not part of the CMake
 * build (the model lib globs src/model only). Run manually:
 *
 *   g++ -std=c++17 -Isrc/model src/model/tool_catalog.cpp \
 *       tests/tool_catalog_selfcheck.cpp -o /tmp/catalog_check && /tmp/catalog_check
 */

#include "tool_catalog.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
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

    // GPS rows: gpspipe guard, LoRa+GNSS Cap port/baud, and the in-app installer
    // carrying gpsd + gpsd-clients.
    {
        const model::ToolCategory* gps = nullptr;
        const model::ToolAction* install = nullptr;
        for (const auto& category : catalog) {
            if (std::string(category.label) == "GPS") gps = &category;
            for (const auto& action : category.actions) {
                if (std::string(action.label) == "Install optional tools (apt)") install = &action;
            }
        }
        assert(gps && install);
        assert(install->needs_root && install->argv.size() == 3);
        assert(install->argv[2].find(" gpsd gpsd-clients ") != std::string::npos);

        const model::ToolAction* fix = nullptr;
        const model::ToolAction* raw = nullptr;
        for (const auto& action : gps->actions) {
            const std::string label(action.label);
            if (label == "GPS fix (gpspipe)") fix = &action;
            if (label.rfind("Raw NMEA", 0) == 0) raw = &action;
        }
        assert(fix && raw);
        assert(fix->argv.size() == 3 && fix->argv[0] == "sh");
        assert(fix->argv[2].rfind("command -v gpspipe ", 0) == 0); // guard runs first
        assert(fix->argv[2].find("sudo apt install gpsd gpsd-clients") != std::string::npos);
        assert(fix->argv[2].find("\"Install optional tools (apt)\"") != std::string::npos);
        assert(fix->argv[2].find("-s 115200 /dev/serial0") != std::string::npos);
        assert(raw->argv.size() == 3 && raw->argv[0] == "sh");
        assert(raw->argv[2].find("${GPS_UART:-/dev/serial0}") != std::string::npos);
        assert(raw->argv[2].find("115200") != std::string::npos);
        assert(raw->argv[2].find("ttyUSB0 9600") == std::string::npos);

        // Run the scripts for real where nothing GPS-related exists: an empty
        // PATH hides gpspipe (command/echo are shell builtins), and a bogus
        // GPS_UART exercises the missing-device branch.
        auto run_script = [](const std::string& env, const std::string& script, int& rc) {
            const std::string path = "/tmp/bruce_gps_selfcheck_" + std::to_string(::getpid()) + ".sh";
            FILE* f = std::fopen(path.c_str(), "w");
            assert(f);
            std::fputs(script.c_str(), f);
            std::fclose(f);
            FILE* p = ::popen((env + " /bin/sh " + path + " 2>&1").c_str(), "r");
            assert(p);
            std::string out;
            char buf[512];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
            const int st = ::pclose(p);
            rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            std::remove(path.c_str());
            return out;
        };
        int rc = 0;
        std::string out = run_script("PATH=/nonexistent", fix->argv[2], rc);
        assert(rc == 127);
        assert(out.find("gpspipe is not installed - run System / Clock > \"Install optional tools (apt)\" "
                        "or: sudo apt install gpsd gpsd-clients") != std::string::npos);
        assert(out.find("timeout:") == std::string::npos);

        out = run_script("GPS_UART=/nonexistent/gps0", raw->argv[2], rc);
        assert(rc == 1);
        assert(out.find("/nonexistent/gps0 not found") != std::string::npos);

        // Exit-status handling with mock tools first on PATH: a normal timeout
        // (coreutils 124, BusyBox 143) is success; any other failure prints the
        // diagnostic AND keeps the original status.
        const std::string mock = "/tmp/bruce_gps_mock_" + std::to_string(::getpid());
        assert(::mkdir(mock.c_str(), 0700) == 0);
        auto write_tool = [&](const char* name, const std::string& body) {
            const std::string p = mock + "/" + name;
            FILE* f = std::fopen(p.c_str(), "w");
            assert(f);
            std::fprintf(f, "#!/bin/sh\n%s\n", body.c_str());
            std::fclose(f);
            assert(::chmod(p.c_str(), 0755) == 0);
        };
        const std::string env = "PATH=" + mock + ":/usr/bin:/bin";
        write_tool("gpspipe", "exit 42");
        write_tool("stty", "exit 0");

        write_tool("timeout", "shift; \"$@\""); // pass-through: gpspipe's 42 propagates
        out = run_script(env, fix->argv[2], rc);
        assert(rc == 42);
        assert(out.find("gpspipe failed (status 42)") != std::string::npos);
        assert(out.find("sudo gpsd -n -s 115200 /dev/serial0") != std::string::npos);

        for (const char* normal : {"exit 124", "exit 143", "exit 0"}) {
            write_tool("timeout", normal);
            out = run_script(env, fix->argv[2], rc);
            assert(rc == 0);
            assert(out.find("gpspipe failed") == std::string::npos);
            out = run_script(env + " GPS_UART=/dev/null", raw->argv[2], rc);
            assert(rc == 0);
            assert(out.find("read failed") == std::string::npos);
        }

        write_tool("timeout", "exit 1"); // cat failed
        out = run_script(env + " GPS_UART=/dev/null", raw->argv[2], rc);
        assert(rc == 1);
        assert(out.find("read failed on /dev/null (status 1)") != std::string::npos);
        assert(out.find("(no $G") == std::string::npos); // stops, no trailing echo

        write_tool("stty", "exit 3");
        out = run_script(env + " GPS_UART=/dev/null", raw->argv[2], rc);
        assert(rc == 3);
        assert(out.find("cannot configure /dev/null (status 3)") != std::string::npos);
        assert(out.find("reading /dev/null") == std::string::npos);

        for (const char* t : {"gpspipe", "stty", "timeout"}) std::remove((mock + "/" + t).c_str());
        ::rmdir(mock.c_str());
    }

    std::printf("tool_catalog self-check passed: %zu categories\n", catalog.size());
    return 0;
}
