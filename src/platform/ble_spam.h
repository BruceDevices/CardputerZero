/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace platform {

// BLE-spam advertisement families, ported from Bruce firmware's ble_spam module
// (payload byte layouts match the Flipper Zero ble_spam reference it uses).
enum class BleSpamTarget {
    Apple,        // Continuity ProximityPair - AirPods/Beats "pairing" popup on iOS
    AppleAction,  // Continuity NearbyAction  - "Setup new device" style iOS modals
    Google,       // Google Fast Pair         - "device nearby" popup on Android
    Samsung,      // Samsung EasySetup        - Galaxy Watch/Buds popup on Samsung
    Microsoft,    // Swift Pair               - "new Bluetooth device" toast on Windows
    All,          // pick a random family each packet
};

// Parse a CLI target name ("apple", "apple-action", "google", "samsung",
// "microsoft", "all"). Returns false on an unknown name.
bool ble_spam_parse_target(const char* name, BleSpamTarget& out);

// Build one legacy-adv payload for `target` into out[0..30]; returns the
// significant length (<= 31). `rng_state` is advanced in place (xorshift32) and
// supplies the random device/battery/name fields. For All, a concrete family is
// chosen at random each call. The returned bytes are the full AD payload ready
// for HCI LE Set Advertising Data (including any Flags AD element).
std::size_t ble_spam_build_payload(BleSpamTarget target, uint8_t out[31], uint32_t& rng_state);

// Entry point for the app's `--ble-spam <target> [hciN]` subcommand. Takes over
// the HCI controller (HCI_CHANNEL_USER) and broadcasts rotating advertisements
// with a fresh random MAC per packet until SIGTERM. Returns 0 on clean exit,
// non-zero if the controller could not be opened.
int ble_spam_main(int argc, char** argv);

} // namespace platform
