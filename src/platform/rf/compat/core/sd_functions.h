/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Shim for the one Bruce core helper the RF protocols tree references.
 * getFsStorage() reports "no SD mounted" on the CM0 so the KeeLoq reader uses
 * the embedded keystore.
 */
#pragma once
#include <FS.h>

inline bool getFsStorage(FS *&fs) {
    fs = nullptr;
    return false;
}
