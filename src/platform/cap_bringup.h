/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * M5 Cap (CC1101 + ST25R3916) bring-up for the CardputerZero HAT/EXT port.
 *
 * On this board the cap is fed through a power chain that userspace must
 * enable before either chip answers SPI (the reason "no functionality works"
 * when the same cap runs fine on the ESP32 firmware):
 *
 *   1. M5IOE1 IO expander (/dev/i2c-1 @ 0x4F), register map per M5's
 *      m5stack/M5IOE1 driver: PYG13 (P13) high closes the HAT-port 5V load
 *      switch (schematic U23); PYG1 (P1) low muxes HAT_P0/P1 to GPIO instead
 *      of USB4 (they carry the cap's POWER_EN / NFC_IRQ).
 *   2. BCM 26 (HAT_P0): the current code drives it high as a "POWER_EN". This
 *      is under review (milestone 5) - G26 is the per-module RST line in the
 *      CAP profiles, not a confirmed system power-enable. See the RST note below.
 *
 * SPI wiring for the CAP CC1101 (CC1101 + ST25R3916), CONFIRMED ON HARDWARE -
 * the per-CAP source of truth lives in the BruceModule profiles
 * (src/hal/bruce_module.*), not here:
 *   SPI0: G11=SCLK G10=MOSI G9=MISO, level-shifted (global, shared bus)
 *   CC1101_CS    = G7  = SPI0_CS1 -> hardware CS -> /dev/spidev0.1
 *   ST25R3916_CS = G22 (plain GPIO: spidev runs SPI_NO_CS, CS bit-banged)
 *   NFC_IRQ      = G23 (unused; the driver polls the IRQ registers)
 *   CC1101_GDO0  = G14, CC1101_RF_SW0 = G15 (RF TX/RX only; not probe-verified)
 * Note G8/SPI0_CS0 is the internal LCD - never asserted on purpose. On the
 * tested CAP CC1101 only the CC1101 (G7) answered; the ST25R (G22) was silent.
 *
 * RST is NOT a fixed system pin: it is per-module (varies by CAP vendor). The
 * known CAPs happen to wire it to G26, carried as each module's `rst` field in
 * the BruceModule profile - G26 is a general-purpose GPIO, not "POWER_EN".
 * (The energy/reset chain itself is milestone 5; this file's role here is the
 * wiring map, which must match the profiles above.)
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct CapBringupReport {
    bool rail_led = false; // ext_5v_out gpio-led closed the HAT 5V rail (primary)
    bool hat_5v = false;   // M5IOE1 PYG13_HAT_EN closed the HAT 5V load switch
    bool cap_en = false;   // deprecated: there is no system POWER_EN; see note below
    std::string text;      // human-readable log lines (newline-terminated)
};

// Best-effort power-up of the cap chain (see file comment). Missing pieces are
// reported with the exact enable command, never fatal - e.g. a user-wired cap
// on the raw header may be powered independently.
CapBringupReport cap_bringup();

// Drive one GPIO line high/low through the char device (v1 uapi ioctls - no
// libgpiod dependency). `chip_path` like "/dev/gpiochip0", `line` = offset.
// The line stays claimed for the process lifetime (the handle is kept open).
bool gpio_line_write(const char* chip_path, unsigned line, bool high);

// M5IOE1 register-bank math for pins P1..P14 (unit-checked): P1-P8 use the L
// mode/out registers, P9-P14 the H ones; returns the bit within that bank.
uint8_t cap_ioe_bit(uint8_t pin);
bool cap_ioe_high_bank(uint8_t pin);

// First candidate path that exists on this system ("" when none). Lets the
// CAP tools fall back across /dev/spidev0.1 / 0.0 automatically instead of
// failing on a node the running kernel never created.
std::string spidev_pick(const std::vector<std::string>& candidates);

// Claim an output line for repeated toggling (CS lines). On success the
// handle fd is stored in *out_handle when non-null (else kept for the process
// lifetime) and the line drives `initial_high`.
bool gpio_claim_output(const char* chip_path, unsigned line, bool initial_high,
                       int* out_handle = nullptr);

// Set a claimed line's level.
bool gpio_line_set(int handle_fd, bool high);

// Read a GPIO line's current level as an input (claims it as input, reads once,
// releases). Returns false on any error (and leaves `value` untouched). Used to
// poll a chip's BUSY line (e.g. SX1262) before an SPI transaction.
bool gpio_read_line(const char* chip_path, unsigned line, bool& value);

// Set a BCM pin's function select (FSEL, 0-7) directly via /dev/gpiomem. This is
// the escape hatch for restoring a pin's peripheral ALT function after it was
// bit-banged as a GPIO: requesting a line through the gpio chardev switches the
// pin to GPIO and the kernel does NOT revert it on release. Needs the `gpio`
// group (no root). Returns false if /dev/gpiomem can't be mapped.
bool gpio_set_function(unsigned line, unsigned fsel);

// Restore the mini-UART (ttyS0) mux on the HAT serial pins G14/G15 (ALT5 =
// TXD1/RXD1) so the GPS keeps working after those pins were used as a CC1101 CS
// (Pingequa). Call after any probe that bit-banged G14 or G15. No-op-safe.
bool gpio_restore_serial_mux();

} // namespace platform
