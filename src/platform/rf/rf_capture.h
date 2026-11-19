/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * RfRxCapture — the "RMT of Linux" (M7).
 *
 * The ESP32 timestamps GDO0 edges with the RMT peripheral (or an ISR, on the
 * M5_RF_MODULE path). There is no RMT here: we capture GDO0 transitions through
 * the GPIO character device (uapi v2, GPIO_V2_LINE_EVENT, kernel ns timestamps)
 * on a dedicated thread (SCHED_FIFO, best-effort), without root (the `gpio`
 * group is enough).
 *
 * The interface mirrors Bruce's RfRxSession::poll(durations) seam: the UI only
 * calls poll() non-blocking and gets back a list of SIGNED durations in µs
 * (HIGH > 0, LOW < 0). Same frame-close semantics (30 ms idle), noise floor
 * (minimum transition count) and glitch filter.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace platform {

class RfRxCapture {
public:
    RfRxCapture() = default;
    ~RfRxCapture() { end(); }
    RfRxCapture(const RfRxCapture&) = delete;
    RfRxCapture& operator=(const RfRxCapture&) = delete;

    // Claim GDO0 as an input with events on both edges and start the reader
    // thread. `chip_path` defaults to /dev/gpiochip0 (offset == BCM number).
    // Returns false if the line cannot be claimed (e.g. G7 held by the SPI
    // driver as CE1).
    bool begin(int gdo0_gpio, const char* chip_path = "/dev/gpiochip0");

    // Non-blocking. If a frame is ready, moves the (already filtered) signed
    // durations into `durations` and returns true; otherwise returns false.
    bool poll(std::vector<int>& durations);

    // Stop the thread and release the line.
    void end();

    bool active() const { return _running.load(); }
    // true if the last frame filled the buffer (signal longer than capacity).
    bool bufferFull() const { return _buffer_full; }
    // Total edge events seen since begin() — diagnostic (is GDO0 toggling at all?).
    uint64_t edges() const { return _edges.load(); }

private:
    void reader_loop(); // thread body

    int _req_fd = -1;         // gpio v2 line-request fd
    int _gdo0 = -1;
    std::thread _thread;
    std::atomic_bool _running{false};
    std::atomic<uint64_t> _edges{0};

    std::mutex _mtx;
    std::vector<int> _ready; // completed frame awaiting poll()
    bool _buffer_full = false;
};

// Apply the glitch filter + same-level duration merge (port of Bruce's
// rf_filter_m5_rx_glitches/rf_push_duration_merge). Exposed for unit testing.
void rf_filter_glitches(std::vector<int>& durations);

} // namespace platform
