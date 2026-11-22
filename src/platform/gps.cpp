/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gps.h"
#include "uart_speed.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef APP_GPS_UART
#define APP_GPS_UART "/dev/serial0"
#endif

namespace platform {
namespace {

constexpr int kDefaultBauds[] = {115200, 9600, 38400, 57600};

#if defined(__linux__)

long now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Find the first plausible NMEA sentence in `buf`: "$G" .. "*" (checksum). On a
// hit, copies the sentence (without CR/LF) into `out` and returns true.
bool find_nmea(const std::string& buf, std::string& out) {
    size_t pos = 0;
    while ((pos = buf.find("$G", pos)) != std::string::npos) {
        const size_t star = buf.find('*', pos);
        const size_t nl = buf.find_first_of("\r\n", pos);
        // A complete sentence has a '*' checksum marker before the line ends.
        if (star != std::string::npos && (nl == std::string::npos || star < nl) &&
            star - pos >= 6 && star + 2 < buf.size()) {
            size_t end = star + 3; // include *HH
            out.assign(buf, pos, end - pos);
            return true;
        }
        pos += 2;
    }
    return false;
}

// Read a UART at one baud for up to `ms` ms; true if NMEA seen.
bool scan_one_baud(const char* uart, int baud, int ms, std::string& sample) {
    if (baud <= 0) {
        return false;
    }
    const int fd = open(uart, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        std::fprintf(stderr, "gps: cannot open %s: %s\n", uart, std::strerror(errno));
        return false;
    }
    // termios2/BOTHER: independent of the glibc B* constants (see uart_speed.h).
    std::string err;
    if (!uart_set_raw(fd, baud, /*vtime_ds=*/2, &err)) { // 200ms read timeout
        std::fprintf(stderr, "gps: cannot set %s to %d baud: %s\n", uart, baud, err.c_str());
        close(fd);
        return false;
    }

    std::string buf;
    const long deadline = now_ms() + ms;
    char chunk[256];
    while (now_ms() < deadline) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            buf.append(chunk, static_cast<size_t>(n));
            if (find_nmea(buf, sample)) {
                close(fd);
                return true;
            }
            if (buf.size() > 8192) {
                buf.erase(0, buf.size() - 2048); // keep the tail bounded
            }
        }
    }
    close(fd);
    return false;
}

#endif // __linux__

} // namespace

bool probe_gps(const char* uart, const int* bauds, int n_bauds, int ms_per_baud,
               int& baud_found, std::string& sample) {
    baud_found = -1;
    sample.clear();
    if (bauds == nullptr || n_bauds <= 0) {
        bauds = kDefaultBauds;
        n_bauds = static_cast<int>(sizeof(kDefaultBauds) / sizeof(kDefaultBauds[0]));
    }
#if defined(__linux__)
    for (int i = 0; i < n_bauds; ++i) {
        if (scan_one_baud(uart, bauds[i], ms_per_baud, sample)) {
            baud_found = bauds[i];
            return true;
        }
    }
#else
    (void)uart;
    (void)ms_per_baud;
#endif
    return false;
}

int gps_detect_main(int argc, char** argv) {
    std::string uart = APP_GPS_UART;
    if (argc >= 1 && argv[0] && argv[0][0] != '\0') {
        uart = argv[0];
    } else {
        const char* env = std::getenv("GPS_UART");
        if (env && env[0] != '\0') {
            uart = env;
        }
    }

    std::printf("GPS detection on %s (NMEA; no fix needed)\n", uart.c_str());

#if defined(__linux__)
    // Per-baud scan with a generous window so a weak receiver still emits a line.
    const int bauds[] = {115200, 9600, 38400, 57600};
    const int n = static_cast<int>(sizeof(bauds) / sizeof(bauds[0]));
    for (int i = 0; i < n; ++i) {
        std::string sample;
        int found = -1;
        const bool ok = probe_gps(uart.c_str(), &bauds[i], 1, 1500, found, sample);
        if (ok) {
            std::printf("  %6d baud: NMEA detected -> %s\n", bauds[i], sample.c_str());
            std::printf("Result: GPS present at %d baud.\n", bauds[i]);
            std::fflush(stdout);
            return 0;
        }
        std::printf("  %6d baud: no NMEA\n", bauds[i]);
        std::fflush(stdout);
    }
    std::printf("Result: no NMEA on any baud - check the CAP seating / antenna, "
                "the UART node, and GPS_UART.\n");
#else
    std::printf("error: UART is only available on Linux builds.\n");
#endif
    std::fflush(stdout);
    return 1;
}

} // namespace platform
