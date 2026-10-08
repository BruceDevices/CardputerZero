/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the Nmap guided-flow state machine. Not part of the
 * CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model src/model/tool_catalog.cpp \
 *       tests/nmap_selfcheck.cpp -o /tmp/nmap_check && /tmp/nmap_check
 *
 * Verifies the discover -> pick host -> Fast/Full drill-down: row layout in each
 * state, navigation rows (empty argv), and the state transitions that
 * on_navigation_action() applies - the part that silently breaks the flow if the
 * builder and the handler disagree on which row maps to which host.
 */

#include "tool_catalog.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>

static const model::ToolCategory& nmap_category() {
    model::refresh_dynamic_categories();
    for (const auto& c : model::tool_catalog()) {
        if (std::string(c.label) == "Nmap") return c;
    }
    std::fprintf(stderr, "no Nmap category\n");
    std::abort();
}

int main() {
    const std::string dir = "/tmp/bruce-nmap";
    mkdir(dir.c_str(), 0755);
    std::remove((dir + "/selected").c_str());

    // --- state: no hosts discovered yet ---
    std::remove((dir + "/hosts").c_str());
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 2);               // Scan + "(no hosts yet)"
        assert(!c.actions[0].argv.empty());          // Scan is runnable
        assert(c.actions[0].argv[0] == "sh");        // discovery via sh -c
    }

    // --- state: hosts discovered, none selected ---
    { std::ofstream o(dir + "/hosts"); o << "192.168.1.1\n192.168.1.50\n10.0.0.9\n"; }
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 1 + 3);           // Scan + 3 host rows
        assert(!c.actions[0].argv.empty());          // Scan runnable
        for (int i = 1; i <= 3; ++i) {
            assert(c.actions[i].argv.empty());       // host rows are navigation
        }
    }

    // --- pick the 2nd host (row index 2) -> selected becomes 192.168.1.50 ---
    model::on_navigation_action(/*category*/ -999, 2); // wrong category: no-op
    // find the real category index for the handler
    int nmap_idx = -1;
    {
        const auto& cat = model::tool_catalog();
        for (size_t i = 0; i < cat.size(); ++i) {
            if (std::string(cat[i].label) == "Nmap") nmap_idx = (int)i;
        }
    }
    assert(nmap_idx >= 0);
    model::on_navigation_action(nmap_idx, 2);        // row 2 == hosts[1]

    // --- state: host selected -> Back + Fast + Full ---
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 3);
        assert(c.actions[0].argv.empty());           // "Back to host list" (nav)
        assert(c.actions[1].argv.size() == 4);
        assert(c.actions[1].argv[0] == "nmap" && c.actions[1].argv[1] == "-F");
        assert(c.actions[1].argv[3] == "192.168.1.50");
        assert(c.actions[2].argv[1] == "-p-");
        assert(c.actions[2].argv[3] == "192.168.1.50");
    }

    // --- "Back to host list" (row 0) clears selection ---
    model::on_navigation_action(nmap_idx, 0);
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 1 + 3);           // back to host list
        assert(c.actions[2].argv.empty());
    }

    std::remove((dir + "/selected").c_str());
    std::remove((dir + "/hosts").c_str());
    std::puts("nmap self-check OK");
    return 0;
}
