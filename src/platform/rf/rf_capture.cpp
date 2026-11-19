/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rf_capture.h"

#include <cstdlib>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

// Frame closes after this much silence on GDO0 (matches Bruce's 30 ms RMT idle;
// must exceed the largest inter-frame gap so repeats stay in one capture).
constexpr int kIdleMs = 30;
// Discard captures with fewer edges than this — pure noise bursts. Bruce uses 16
// on the RMT path; the bare-GPIO path is noisier, but the CC1101's own AGC plus
// the glitch filter below keep this usable.
constexpr size_t kMinTransitions = 16;
// Drop transitions shorter than this (µs). Real OOK base pulses are ~250 µs, so
// this removes sub-bit spikes without eating legitimate symbols. Tunable on HW.
constexpr int kGlitchUs = 60;
// Hard cap on a single frame (signed durations). ~Bruce's 8192 symbols worth.
constexpr size_t kMaxDurations = 8192;

int env_int_or(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return (v && v[0] != '\0') ? std::atoi(v) : fallback;
}

// Merge a duration into `out`, coalescing with the back entry when they share a
// sign (port of Bruce's rf_push_duration_merge).
void push_merge(std::vector<int>& out, int d) {
    if (!out.empty() && ((out.back() > 0) == (d > 0))) {
        out.back() += d;
    } else {
        out.push_back(d);
    }
}

} // namespace

void rf_filter_glitches(std::vector<int>& durations) {
    if (durations.empty()) return;
    // Env override (RF_RX_GLITCH_US) for HW tuning without a rebuild; 0 disables.
    const int threshold = env_int_or("RF_RX_GLITCH_US", kGlitchUs);
    if (threshold <= 0) return;
    std::vector<int> filtered;
    filtered.reserve(durations.size());
    bool removed = false;
    for (int d : durations) {
        if (d > -threshold && d < threshold) { // |d| < threshold
            removed = true;
            continue;
        }
        push_merge(filtered, d);
    }
    if (removed) durations.swap(filtered);
}

#if defined(__linux__)

bool RfRxCapture::begin(int gdo0_gpio, const char* chip_path) {
    end();
    _gdo0 = gdo0_gpio;
    _buffer_full = false;
    _edges.store(0);

    const int chip_fd = open(chip_path, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0) return false;

    gpio_v2_line_request req{};
    req.offsets[0] = static_cast<__u32>(gdo0_gpio);
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                       GPIO_V2_LINE_FLAG_EDGE_FALLING;
    std::strncpy(req.consumer, "bruce-rf-rx", sizeof(req.consumer) - 1);

    const int rc = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip_fd);
    if (rc < 0 || req.fd < 0) return false;

    _req_fd = req.fd;
    // Make the event fd non-blocking so the reader's drain loop returns on EAGAIN
    // instead of blocking in read() waiting for the next edge. Otherwise, if the
    // signal stops, the thread is stuck in read(), never re-checks _running, and
    // end()'s join() hangs — which made --rf-record run long past its window.
    if (const int fl = fcntl(_req_fd, F_GETFL); fl != -1) {
        fcntl(_req_fd, F_SETFL, fl | O_NONBLOCK);
    }
    _running.store(true);
    _thread = std::thread(&RfRxCapture::reader_loop, this);
    return true;
}

void RfRxCapture::reader_loop() {
    // Best-effort real-time priority: fewer missed edges under load. Failing is
    // fine (no CAP_SYS_NICE) — we just run at normal priority then.
    sched_param sp{};
    sp.sched_priority = 20;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    const size_t min_transitions =
        static_cast<size_t>(env_int_or("RF_RX_MIN_TRANSITIONS", kMinTransitions));

    std::vector<int> frame;
    frame.reserve(1024);
    bool have_prev = false;
    uint64_t last_ns = 0;

    auto finalize = [&]() {
        // Glitch-filter FIRST, then apply the noise floor to the FILTERED pulse
        // count. A noise burst has many sub-threshold spikes that collapse to a
        // handful of durations once filtered, so checking the count afterwards is
        // what actually separates a real code (tens of pulses) from AGC chatter
        // (1-2 survivors). Checking the raw count let that chatter through.
        rf_filter_glitches(frame);
        if (frame.size() >= min_transitions) {
            {
                std::lock_guard<std::mutex> lk(_mtx);
                _ready = frame; // last frame wins if poll() hasn't drained it yet
                _buffer_full = frame.size() >= kMaxDurations;
            }
        }
        frame.clear();
        have_prev = false;
    };

    pollfd pfd{};
    pfd.fd = _req_fd;
    pfd.events = POLLIN;

    while (_running.load()) {
        const int pr = ::poll(&pfd, 1, kIdleMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) { // idle timeout -> frame boundary
            if (have_prev && !frame.empty()) finalize();
            continue;
        }
        if (!(pfd.revents & POLLIN)) continue;

        // Drain all pending edge events.
        gpio_v2_line_event ev{};
        while (read(_req_fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
            _edges.fetch_add(1, std::memory_order_relaxed);
            if (have_prev) {
                const uint64_t delta_ns = ev.timestamp_ns - last_ns;
                int dur_us = static_cast<int>(delta_ns / 1000ULL);
                if (dur_us <= 0) dur_us = 1;
                // The level held during this interval is the one BEFORE this
                // edge: a RISING edge means it was LOW (negative), FALLING means
                // it was HIGH (positive).
                const bool held_high = (ev.id == GPIO_V2_LINE_EVENT_FALLING_EDGE);
                frame.push_back(held_high ? dur_us : -dur_us);
                if (frame.size() >= kMaxDurations) {
                    finalize();
                    // start a fresh frame seeded from this edge
                    last_ns = ev.timestamp_ns;
                    have_prev = true;
                    continue;
                }
            }
            last_ns = ev.timestamp_ns;
            have_prev = true;
        }
    }
}

bool RfRxCapture::poll(std::vector<int>& durations) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (_ready.empty()) return false;
    durations.swap(_ready);
    _ready.clear();
    return true;
}

void RfRxCapture::end() {
    _running.store(false);
    if (_thread.joinable()) _thread.join();
    if (_req_fd >= 0) {
        close(_req_fd);
        _req_fd = -1;
    }
    std::lock_guard<std::mutex> lk(_mtx);
    _ready.clear();
}

#else // !__linux__

bool RfRxCapture::begin(int, const char*) { return false; }
void RfRxCapture::reader_loop() {}
bool RfRxCapture::poll(std::vector<int>&) { return false; }
void RfRxCapture::end() {}

#endif // __linux__

} // namespace platform
