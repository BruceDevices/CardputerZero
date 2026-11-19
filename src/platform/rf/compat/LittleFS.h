/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * LittleFS shim: a single global FS instance. On Linux there is no LittleFS
 * partition, so lookups of device paths (e.g. "/mfcodes") simply miss and the
 * KeeLoq keystore falls back to the embedded (AES-decrypted) blob.
 */
#pragma once
#include <FS.h>

extern FS LittleFS;
