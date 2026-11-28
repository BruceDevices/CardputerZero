/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Tiny Linux i2c-dev helper shared by the native RFID drivers (the ST25R3916
 * in I2C mode and the M5 RFID2 / MFRC522). Thin wrappers over /dev/i2c-N: open
 * the bus for a 7-bit address, do plain writes/reads, and a combined
 * write-then-read with a REPEATED START (I2C_RDWR) which register-pointer chips
 * like the ST25R3916 and MFRC522 expect for a register read.
 *
 * No hardware is required to compile these (guarded for __linux__); on other
 * platforms the calls are no-ops returning failure so the drivers build.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

// Open `dev` (e.g. "/dev/i2c-1") and bind the default slave to `addr7`
// (I2C_SLAVE). Returns an fd >= 0 on success, or -1 (errno set). Close with
// i2c_close().
int i2c_open(const char* dev, uint8_t addr7);

void i2c_close(int fd);

// Plain write of `n` bytes to the bound slave. Returns true on success.
bool i2c_write(int fd, const uint8_t* buf, size_t n);

// Plain read of `n` bytes from the bound slave. Returns true on success.
bool i2c_read(int fd, uint8_t* buf, size_t n);

// Combined transaction with a repeated START between the write and the read,
// addressed to `addr7` (so it works regardless of the bound default slave):
// write `wn` bytes, then read `rn` bytes. Returns true on success.
bool i2c_write_read(int fd, uint8_t addr7, const uint8_t* w, size_t wn, uint8_t* r, size_t rn);

// Probe whether a device ACKs at `addr7` on `dev` (0-length write). Returns
// true if something answered. Opens/closes its own fd.
bool i2c_present(const char* dev, uint8_t addr7);

// Enumerate candidate /dev/i2c-* bus paths that exist on this system, most
// likely first ("/dev/i2c-1" leads). Empty when the kernel exposes none.
std::vector<std::string> i2c_buses();

} // namespace platform
