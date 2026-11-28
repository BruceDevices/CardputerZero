/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Strict parsing of a numeric uid/gid such as $SUDO_UID, shared by the menu
 * (model::app home under sudo) and the wardriving child (platform). Header
 * only, so the model library stays free of platform code.
 */

#pragma once

#include <cstdint>

namespace model {

// Digits only - no sign, space or suffix - and below (uid_t)-1.
inline bool parse_id(const char* text, uint32_t& out) {
    if (!text || !*text) {
        return false;
    }
    uint64_t v = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint64_t>(*p - '0');
        if (v >= 0xffffffffull) {
            return false;
        }
    }
    out = static_cast<uint32_t>(v);
    return true;
}

} // namespace model
