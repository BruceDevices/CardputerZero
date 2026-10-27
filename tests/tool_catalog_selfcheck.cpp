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
                    assert(with_pw.size() == 8);
                    assert(with_pw[0] == "sudo" && with_pw[1] == "-k" && with_pw[2] == "-S" &&
                           with_pw[3] == "-p" && with_pw[4] == "");
                    assert(with_pw[5] == launched[2] && with_pw[6] == launched[3] &&
                           with_pw[7] == launched[4]);
                } else {
                    assert(with_pw.size() == launched.size() + 3);
                    assert(with_pw[0] == "sudo" && with_pw[1] == "-k" && with_pw[2] == "-S" &&
                           with_pw[3] == "-p" && with_pw[4] == "");
                    assert(std::vector<std::string>(with_pw.begin() + 5, with_pw.end()) ==
                           std::vector<std::string>(launched.begin() + 2, launched.end()));
                }
                assert(model::action_needs_root(action) == (launched[0] == "sudo"));
            }
        }
    }

    std::printf("tool_catalog self-check passed: %zu categories\n", catalog.size());
    return 0;
}
