/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rf_transmit.h"

#include <cstdlib>

#if defined(APP_HAVE_LIBGPIOD)
#include <ctime>
#include <gpiod.h>
#endif

namespace platform {
namespace rf {

#if defined(APP_HAVE_LIBGPIOD)

namespace {
constexpr const char* kConsumer = "bruce-rf-tx";

// Advance an absolute CLOCK_MONOTONIC deadline by `us` microseconds and sleep to
// it. Absolute deadlines stop per-pulse rounding error from accumulating over a
// long frame.
void sleep_until_plus(timespec& deadline, long us) {
    deadline.tv_nsec += us * 1000L;
    while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
}
} // namespace

GpioBitBangTx::~GpioBitBangTx() { close(); }

bool GpioBitBangTx::open(const std::string& chip, unsigned int line_offset) {
    close();
    gpiod_chip* c = gpiod_chip_open_by_name(chip.c_str());
    if (!c) return false;
    gpiod_line* l = gpiod_chip_get_line(c, line_offset);
    if (!l || gpiod_line_request_output(l, kConsumer, 0) < 0) {
        gpiod_chip_close(c);
        return false;
    }
    chip_ = c;
    line_ = l;
    return true;
}

void GpioBitBangTx::close() {
    if (line_) {
        gpiod_line_release(static_cast<gpiod_line*>(line_));
        line_ = nullptr;
    }
    if (chip_) {
        gpiod_chip_close(static_cast<gpiod_chip*>(chip_));
        chip_ = nullptr;
    }
}

bool GpioBitBangTx::is_open() const { return chip_ != nullptr && line_ != nullptr; }

bool GpioBitBangTx::transmit(const std::vector<int>& durations) {
    if (!is_open() || durations.empty()) return false;
    gpiod_line* l = static_cast<gpiod_line*>(line_);

    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    for (int d : durations) {
        if (d == 0) continue;
        const int level = (d > 0) ? 1 : 0;
        const long us = (d > 0) ? d : -d;
        if (gpiod_line_set_value(l, level) < 0) {
            gpiod_line_set_value(l, 0);
            return false;
        }
        sleep_until_plus(deadline, us);
    }

    gpiod_line_set_value(l, 0); // leave the line (and carrier) idle LOW
    return true;
}

#else // !APP_HAVE_LIBGPIOD -- desktop/emulator stub

GpioBitBangTx::~GpioBitBangTx() {}
bool GpioBitBangTx::open(const std::string&, unsigned int) { return false; }
void GpioBitBangTx::close() {}
bool GpioBitBangTx::is_open() const { return false; }
bool GpioBitBangTx::transmit(const std::vector<int>&) { return false; }

#endif

} // namespace rf
} // namespace platform
