/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Shim for Bruce's <globals.h>. The RF protocols tree only needs
 * `bruceConfigPins` from it, which the compat rf_utils.h provides.
 */
#pragma once
#include <Arduino.h>
#include "rf_utils.h"
