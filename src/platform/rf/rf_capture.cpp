/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rf_capture.h"

#include <cstdlib>

#if defined(APP_HAVE_LIBGPIOD)
#include <ctime>
#include <gpiod.h>
#endif

namespace platform {
namespace rf {

#if defined(APP_HAVE_LIBGPIOD)

namespace {
constexpr const char* kConsumer = "bruce-rf-rx";

// Signed microseconds between two edge timestamps; sign carries the level held
// during that interval (level BEFORE the edge).
long diff_us(const timespec& a, const timespec& b) {
    long sec = static_cast<long>(b.tv_sec - a.tv_sec);
    long nsec = b.tv_nsec - a.tv_nsec;
    return sec * 1000000L + nsec / 1000L;
}
} // namespace

GpioEdgeCapture::~GpioEdgeCapture() {
    stop();
    close();
}

bool GpioEdgeCapture::open(const std::string& chip, unsigned int line_offset) {
    close();
    gpiod_chip* c = gpiod_chip_open_by_name(chip.c_str());
    if (!c) return false;
    gpiod_line* l = gpiod_chip_get_line(c, line_offset);
    if (!l) {
        gpiod_chip_close(c);
        return false;
    }
    chip_ = c;
    line_ = l;
    return true;
}

void GpioEdgeCapture::close() {
    stop(); // drops the event request if held
    line_ = nullptr; // owned by the chip; freed with gpiod_chip_close
    if (chip_) {
        gpiod_chip_close(static_cast<gpiod_chip*>(chip_));
        chip_ = nullptr;
    }
}

bool GpioEdgeCapture::is_open() const { return chip_ != nullptr && line_ != nullptr; }

bool GpioEdgeCapture::start(unsigned int min_pulse_us) {
    if (!is_open() || running_.load()) return false;
    gpiod_line* l = static_cast<gpiod_line*>(line_);
    if (gpiod_line_request_both_edges_events(l, kConsumer) < 0) return false;

    requested_ = true;
    stop_flag_.store(false);
    running_.store(true);
    worker_ = std::thread(&GpioEdgeCapture::worker_main, this, min_pulse_us);
    return true;
}

void GpioEdgeCapture::stop() {
    stop_flag_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
    if (requested_ && line_) {
        gpiod_line_release(static_cast<gpiod_line*>(line_)); // frees the request; start() re-requests
        requested_ = false;
    }
}

void GpioEdgeCapture::worker_main(unsigned int min_pulse_us) {
    gpiod_line* l = static_cast<gpiod_line*>(line_);
    timespec last{};
    bool have_last = false;
    // Level the line was driven to by the most recent edge. The interval between
    // that edge and the next one is held at exactly this level, so it is the sign
    // for that interval: a rising edge leaves the line HIGH (+), a falling edge
    // LOW (-).
    bool cur_level_high = false;

    while (!stop_flag_.load()) {
        timespec timeout{0, 200 * 1000 * 1000}; // 200 ms poll so stop() is responsive
        int r = gpiod_line_event_wait(l, &timeout);
        if (r <= 0) continue; // 0 = timeout, <0 = error (retry)

        gpiod_line_event ev;
        if (gpiod_line_event_read(l, &ev) < 0) continue;

        if (have_last) {
            long dur = diff_us(last, ev.ts);
            if (dur > 0 && static_cast<unsigned>(dur) >= min_pulse_us) {
                int signed_dur = cur_level_high ? static_cast<int>(dur) : -static_cast<int>(dur);
                std::lock_guard<std::mutex> lock(mutex_);
                // Merge same-level runs (a dropped glitch left the level unchanged).
                if (!pending_.empty() && ((pending_.back() > 0) == (signed_dur > 0))) {
                    pending_.back() += signed_dur;
                } else {
                    pending_.push_back(signed_dur);
                }
            }
        }
        last = ev.ts;
        cur_level_high = (ev.event_type == GPIOD_LINE_EVENT_RISING_EDGE);
        have_last = true;
    }
}

bool GpioEdgeCapture::poll(std::vector<int>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.empty()) return false;
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
    return true;
}

#else // !APP_HAVE_LIBGPIOD  -- desktop/emulator stub

GpioEdgeCapture::~GpioEdgeCapture() {}
bool GpioEdgeCapture::open(const std::string&, unsigned int) { return false; }
void GpioEdgeCapture::close() {}
bool GpioEdgeCapture::is_open() const { return false; }
bool GpioEdgeCapture::start(unsigned int) { return false; }
void GpioEdgeCapture::stop() { running_.store(false); }
void GpioEdgeCapture::worker_main(unsigned int) {}
bool GpioEdgeCapture::poll(std::vector<int>&) { return false; }

#endif

} // namespace rf
} // namespace platform
