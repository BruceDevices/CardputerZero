/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "lvgl.h"

namespace app {
class AssetManager;
}

namespace view {

// Play the Bruce boot animation: a full-screen overlay (on the top layer) with
// the Bruce shark zooming toward the viewer and the "BRUCE" wordmark in `accent`,
// then fades out and deletes itself to reveal whatever screen is underneath.
// Non-blocking - driven by the running LVGL timer loop.
void play_boot_animation(app::AssetManager& assets, lv_color_t accent);

} // namespace view
