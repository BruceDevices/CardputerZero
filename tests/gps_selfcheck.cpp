/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the header-independent UART setup used by the GPS
 * probe (termios2/BOTHER) and for probe_gps() end to end on a pseudo-terminal.
 * Not part of the CMake build. Run manually (Linux):
 *
 *   g++ -std=c++17 -pthread -Isrc/platform tests/gps_selfcheck.cpp src/platform/gps.cpp \
 *       src/platform/uart_speed.cpp -lutil -o /tmp/gps_check && /tmp/gps_check
 *
 * Regression for the glibc 2.42 B* change: the old code passed numeric bauds to
 * cfsetispeed() and the port silently stayed at 9600. Here every speed must be
 * read back exactly via TCGETS2 (numeric); the classic termios API is used only
 * for the raw-mode flags and VMIN/VTIME.
 */

#include "gps.h"
#include "uart_speed.h"

#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main() {
    using namespace platform;

    int master = -1;
    int slave = -1;
    char name[128] = {0};
    assert(openpty(&master, &slave, name, nullptr, nullptr) == 0);

    // Start from a known speed different from every target.
    {
        termios t{};
        assert(tcgetattr(slave, &t) == 0);
        cfsetispeed(&t, B9600);
        cfsetospeed(&t, B9600);
        assert(tcsetattr(slave, TCSANOW, &t) == 0);
    }

    for (const int baud : {115200, 38400, 57600, 9600, 115200}) {
        std::string err;
        const bool ok = uart_set_raw(slave, baud, 2, &err);
        if (!ok) std::fprintf(stderr, "uart_set_raw(%d): %s\n", baud, err.c_str());
        assert(ok && err.empty());
        assert(uart_get_speed(slave) == baud);

        // Classic API only for flags: after a termios2/BOTHER set, legacy
        // TCGETS on glibc <2.42 reports the speed as BOTHER, not B115200.
        termios t{};
        assert(tcgetattr(slave, &t) == 0);
        assert((t.c_lflag & (ICANON | ECHO | ISIG)) == 0); // raw
        assert((t.c_cflag & CSIZE) == CS8 && (t.c_cflag & (CLOCAL | CREAD)) == (CLOCAL | CREAD));
        assert(t.c_cc[VMIN] == 0 && t.c_cc[VTIME] == 2);
    }

    // Errors are reported, not swallowed.
    {
        std::string err;
        assert(!uart_set_raw(slave, 0, 2, &err) && !err.empty());
        err.clear();
        int fds[2];
        assert(pipe(fds) == 0); // not a tty -> TCGETS2 fails
        assert(!uart_set_raw(fds[0], 115200, 2, &err));
        assert(err.find("TCGETS2") != std::string::npos);
        close(fds[0]);
        close(fds[1]);
        assert(uart_get_speed(-1) == -1);
    }

    // probe_gps end to end: feed NMEA into the master side while the probe
    // reads the slave node; it must report the first baud it tried.
    {
        std::thread feeder([master] {
            const char* nmea = "garbage\r\n$GNRMC,,V,,,,,,,,,,N,V*37\r\n$GNGGA,,,,,,0,00,25.5,,,,,,*64\r\n";
            for (int i = 0; i < 20; ++i) {
                (void)!write(master, nmea, std::strlen(nmea));
                usleep(50 * 1000);
            }
        });
        const int bauds[] = {115200};
        int found = -1;
        std::string sample;
        const bool ok = probe_gps(name, bauds, 1, 800, found, sample);
        feeder.join();
        assert(ok && found == 115200);
        assert(sample == "$GNRMC,,V,,,,,,,,,,N,V*37");
    }

    close(slave);
    close(master);
    std::printf("gps self-check passed (pty %s)\n", name);
    return 0;
}
