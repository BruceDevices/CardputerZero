/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the Scripts category discovery. Not part of the
 * CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model src/model/tool_catalog.cpp \
 *       tests/scripts_selfcheck.cpp -o /tmp/scripts_check && /tmp/scripts_check
 */

#include "tool_catalog.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static const model::ToolCategory& scripts_category() {
    for (const auto& c : model::tool_catalog()) {
        if (std::string(c.label) == "Scripts") {
            return c;
        }
    }
    std::fprintf(stderr, "no Scripts category\n");
    std::abort();
}

int main() {
    // Point the catalog at a temp dir BEFORE the first tool_catalog() call
    // (the scan runs once, at first access).
    const std::string dir = std::string("/tmp/bruce_scripts_check_") + std::to_string(getpid());
    mkdir(dir.c_str(), 0755);
    setenv("BRUCE_SCRIPTS_DIR", dir.c_str(), 1);

    // b.sh executable, a.sh executable, z.txt not executable, .hidden skipped.
    for (const char* n : {"b.sh", "a.sh"}) {
        std::ofstream(dir + "/" + n) << "#!/bin/sh\necho hi\n";
        chmod((dir + "/" + n).c_str(), 0755);
    }
    std::ofstream(dir + "/z.txt") << "not exec\n"; // 0644, must be skipped
    std::ofstream(dir + "/.hidden") << "#!/bin/sh\n";
    chmod((dir + "/.hidden").c_str(), 0755); // executable but dotfile -> skipped

    const auto& scripts = scripts_category();
    assert(!scripts.info_only);
    assert(scripts.actions.size() == 2); // a.sh, b.sh only
    assert(std::string(scripts.actions[0].label) == "a.sh"); // sorted
    assert(std::string(scripts.actions[1].label) == "b.sh");
    // argv[0] is the full path execvp launches, honoring the script's shebang.
    assert(scripts.actions[0].argv.size() == 1);
    assert(scripts.actions[0].argv[0] == dir + "/a.sh");

    std::puts("scripts self-check OK");
    return 0;
}
