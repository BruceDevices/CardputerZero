/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform {
namespace rf {

// Captures the CC1101 OOK bitstream by timestamping edges on the GDO0 line with
// libgpiod (kernel edge events). Replaces the ESP32 RMT RX peripheral: the chip
// demodulates, GDO0 carries the sliced data, and we turn edge timestamps into
// the signed-microsecond duration list the decoder consumes (+ HIGH, - LOW).
//
// Threading follows ProcessRunner: a worker thread only touches a mutex-guarded
// buffer; the LVGL thread calls poll() to drain it. The worker never touches
// LVGL. Sub-glitch edges shorter than min_pulse_us are merged out (the CC1101
// OOK slicer emits hash when no real signal is present).
//
// Requires libgpiod (APP_HAVE_LIBGPIOD). Without it every method is a safe
// no-op returning false, so desktop/emulator builds still link.
class GpioEdgeCapture {
public:
    GpioEdgeCapture() = default;
    ~GpioEdgeCapture();

    GpioEdgeCapture(const GpioEdgeCapture&) = delete;
    GpioEdgeCapture& operator=(const GpioEdgeCapture&) = delete;

    // Open GDO0: `chip` e.g. "gpiochip0", `line_offset` the GDO0 line number
    // (verify against the CAP wiring - the label G# is not the kernel offset).
    bool open(const std::string& chip, unsigned int line_offset);
    void close();
    bool is_open() const;

    // Start/stop the edge-capture worker. min_pulse_us drops shorter glitches.
    bool start(unsigned int min_pulse_us = 100);
    void stop();
    bool running() const { return running_.load(); }

    // Move captured durations (signed µs) to `out`. Returns true if any were
    // produced since the last call. Call from the LVGL thread.
    bool poll(std::vector<int>& out);

private:
    void worker_main(unsigned int min_pulse_us);

    std::thread worker_;
    std::mutex mutex_;
    std::vector<int> pending_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};

    // Opaque libgpiod handles (void* so the header stays lib-agnostic).
    void* chip_ = nullptr;
    void* line_ = nullptr;
    bool requested_ = false; // whether the line currently holds an edge-event request
};

} // namespace rf
} // namespace platform
