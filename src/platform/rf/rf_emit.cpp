/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rf_emit.h"

#include "cap_bringup.h" // gpio_claim_output / gpio_line_set / gpio_restore_serial_mux

#if defined(__linux__)
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace platform {

void rf_ook_encode_pt2262(uint64_t key, int bits, int te, int count, std::vector<int>& out) {
    if (bits <= 0 || te <= 0 || count <= 0) return;
    out.reserve(out.size() + static_cast<size_t>(count) * (bits * 2 + 2));
    for (int rep = 0; rep < count; ++rep) {
        for (int i = bits - 1; i >= 0; --i) {
            const bool one = (key >> i) & 0x1ULL;
            out.push_back(one ? 3 * te : 1 * te);   // HIGH
            out.push_back(one ? -1 * te : -3 * te); // LOW
        }
        // Sync / inter-frame gap (protocol 1: high 1*te, low 31*te).
        out.push_back(1 * te);
        out.push_back(-31 * te);
    }
}

#if defined(__linux__)

namespace {
// Busy-wait until `deadline` on CLOCK_MONOTONIC (sub-ms precision; no syscall).
inline void busy_until(const timespec& deadline) {
    timespec now;
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < deadline.tv_sec ||
             (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
}
inline void add_us(timespec& t, long us) {
    t.tv_nsec += us * 1000L;
    while (t.tv_nsec >= 1000000000L) {
        t.tv_nsec -= 1000000000L;
        t.tv_sec += 1;
    }
}
} // namespace

bool rf_tx_line_open(RfTxLine& line, int gpio, const char* chip) {
    line = RfTxLine{};
    if (!gpio_claim_output(chip, static_cast<unsigned>(gpio), /*initial_high=*/false, &line.fd)) {
        return false;
    }
    line.gpio = gpio;
    return true;
}

bool rf_tx_line_emit(const RfTxLine& line, const std::vector<int>& durations) {
    if (line.fd < 0 || durations.empty()) return false;

    // Best-effort real-time priority so preemption doesn't stretch a pulse.
    sched_param sp{};
    sp.sched_priority = 30;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    for (int d : durations) {
        gpio_line_set(line.fd, d > 0); // +high / -low
        add_us(t, d < 0 ? -d : d);
        busy_until(t);
    }
    gpio_line_set(line.fd, false); // leave carrier off
    return true;
}

void rf_tx_line_close(RfTxLine& line) {
    if (line.fd >= 0) {
        gpio_line_set(line.fd, false);
        close(line.fd);
    }
    // G14/G15 are the shared mini-UART pins (CAP CC1101 GDO0=G15); hand them back
    // to ALT5 if we just bit-banged one — same contract as the probes/radio.
    if (line.gpio == 14 || line.gpio == 15) gpio_restore_serial_mux();
    line = RfTxLine{};
}

bool rf_bitbang_durations(int gpio, const std::vector<int>& durations, const char* chip) {
    RfTxLine line;
    if (!rf_tx_line_open(line, gpio, chip)) return false;
    const bool ok = rf_tx_line_emit(line, durations);
    rf_tx_line_close(line);
    return ok;
}

#else // !__linux__

bool rf_tx_line_open(RfTxLine&, int, const char*) { return false; }
bool rf_tx_line_emit(const RfTxLine&, const std::vector<int>&) { return false; }
void rf_tx_line_close(RfTxLine&) {}
bool rf_bitbang_durations(int, const std::vector<int>&, const char*) { return false; }

#endif // __linux__

} // namespace platform
