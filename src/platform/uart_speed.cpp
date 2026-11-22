/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "uart_speed.h"

#if defined(__linux__)
// Deliberately NOT <termios.h>: see uart_speed.h.
#include <asm/termbits.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#endif

namespace platform {

#if defined(__linux__)

namespace {

bool fail(std::string* err, const char* what) {
    if (err) {
        *err = std::string(what) + ": " + std::strerror(errno);
    }
    return false;
}

bool speed_close(unsigned got, int want) {
    const long diff = std::labs(static_cast<long>(got) - static_cast<long>(want));
    return diff * 50 <= static_cast<long>(want); // within 2%
}

} // namespace

bool uart_set_raw(int fd, int baud, int vtime_ds, std::string* err) {
    if (baud <= 0) {
        errno = EINVAL;
        return fail(err, "invalid baud");
    }
    struct termios2 tio {};
    if (ioctl(fd, TCGETS2, &tio) != 0) {
        return fail(err, "TCGETS2");
    }
    // cfmakeraw() equivalent, then 8N1 + receiver on, modem lines ignored.
    tio.c_iflag &= ~static_cast<tcflag_t>(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    tio.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB | CBAUD | (CBAUD << IBSHIFT));
    tio.c_cflag |= CS8 | CLOCAL | CREAD | BOTHER | (BOTHER << IBSHIFT);
    tio.c_ispeed = static_cast<speed_t>(baud);
    tio.c_ospeed = static_cast<speed_t>(baud);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = static_cast<cc_t>(vtime_ds < 0 ? 0 : (vtime_ds > 255 ? 255 : vtime_ds));
    if (ioctl(fd, TCSETS2, &tio) != 0) {
        return fail(err, "TCSETS2");
    }

    struct termios2 back {};
    if (ioctl(fd, TCGETS2, &back) != 0) {
        return fail(err, "TCGETS2 (read-back)");
    }
    if (!speed_close(back.c_ospeed, baud) || !speed_close(back.c_ispeed, baud)) {
        if (err) {
            *err = "speed not applied: asked " + std::to_string(baud) + ", tty reports in=" +
                   std::to_string(back.c_ispeed) + " out=" + std::to_string(back.c_ospeed);
        }
        return false;
    }
    // Drop bytes received at the previous (wrong) speed.
    if (ioctl(fd, TCFLSH, TCIFLUSH) != 0) {
        return fail(err, "TCFLSH");
    }
    return true;
}

int uart_get_speed(int fd) {
    struct termios2 tio {};
    if (ioctl(fd, TCGETS2, &tio) != 0) {
        return -1;
    }
    return static_cast<int>(tio.c_ospeed);
}

#else

bool uart_set_raw(int, int, int, std::string* err) {
    if (err) {
        *err = "UART setup is only available on Linux builds";
    }
    return false;
}

int uart_get_speed(int) { return -1; }

#endif

} // namespace platform
