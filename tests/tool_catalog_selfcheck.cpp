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

#include <cassert>
#include <cstdio>

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
        // must have a non-empty argv (argv[0] is what execvp launches).
        assert(!category.actions.empty());
        for (const auto& action : category.actions) {
            assert(action.label != nullptr && action.label[0] != '\0');
            assert(!action.argv.empty());
            assert(!action.argv[0].empty());
        }
    }

    // Exactly one info-only category (About) is expected today.
    assert(info_only_count == 1);

    std::printf("tool_catalog self-check passed: %zu categories\n", catalog.size());
    return 0;
}
