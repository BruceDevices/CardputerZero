/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Header-independent UART line setup (Linux). Uses the kernel's termios2
 * TCGETS2/TCSETS2 ioctls with BOTHER, so the speed is passed as a plain integer
 * and never goes through glibc's B* constants. Those constants changed value in
 * glibc 2.42 (B115200 == 115200U). A binary compiled against >=2.42 headers but
 * linked against an older libc passed bauds that cfsetispeed() rejected, so the
 * port silently stayed at its previous speed.
 *
 * Lives in its own translation unit: <asm/termbits.h> and <termios.h> both
 * define `struct termios` and can't be included together.
 */

#pragma once

#include <string>

namespace platform {

// Put `fd` (an open tty) into raw 8N1 mode at `baud` with CLOCAL|CREAD,
// VMIN=0 and VTIME=`vtime_ds` (tenths of a second), then flush pending input.
// Reads the settings back and fails unless the input and output speeds are
// within 2% of `baud`. On failure returns false and, if `err` is non-null,
// describes the step that failed (with strerror). No-op false on non-Linux.
bool uart_set_raw(int fd, int baud, int vtime_ds, std::string* err);

// Read the tty's current output speed in baud (-1 on error / non-Linux).
int uart_get_speed(int fd);

} // namespace platform
