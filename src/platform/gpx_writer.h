/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Storage worker for the GPS Tracker's GPX track. Every file operation (folder
 * open, exclusive create + header, points, footer/close) runs on this worker's
 * own thread, so slow or stuck storage never blocks the LVGL thread, the serial
 * reader, or the release of the UART on Back. Callers enqueue jobs and later
 * collect results with take_result() on their own thread.
 *
 * Safety:
 *  - The folder is opened O_DIRECTORY|O_NOFOLLOW and files are created relative
 *    to it with O_EXCL|O_NOFOLLOW (never following a planted symlink, never
 *    appending to another session's track: a "-1", "-2", ... suffix is used).
 *  - SIGXFSZ is blocked on the worker thread, so a write past RLIMIT_FSIZE fails
 *    with EFBIG and is reported instead of killing the process.
 *  - Writes are checked (short writes retried); a header that cannot be written
 *    leaves no stub file.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace platform::gps {

struct GpxPoint {
    double lat = 0, lng = 0, ele = 0, hdop = 0;
    uint32_t sats = 0;
};

// Default for the process-wide shutdown deadline (see shutdown_deadline()).
constexpr int kShutdownTimeoutMs = 3000;

class GpxWriter {
public:
    struct Result {
        enum class Kind { Point, Finalized };
        Kind kind = Kind::Point;
        bool ok = false;
        bool storage_error = false; // "Storage setup error" (folder unusable)
        std::string error;          // Bruce-style one-line message
        std::string filename;       // actual track name (may carry a suffix)
        GpxPoint point;
    };

    // `dir` is ~/BruceGPS. `delay_ms` (tests only) delays every file operation.
    explicit GpxWriter(std::string dir, int delay_ms = 0);
    // Never blocks: queued work (e.g. the footer) still completes on the
    // worker, which a process-wide registry owns and joins (drain_writers()).
    ~GpxWriter();

    GpxWriter(const GpxWriter&) = delete;
    GpxWriter& operator=(const GpxWriter&) = delete;

    // Append a point; the first one creates `name` (or a suffixed variant).
    void write_point(const std::string& name, const GpxPoint& p);
    // Write the footer and close the track (only one this writer created).
    void finalize();

    // Pop one finished job's result (caller's thread). False when none.
    bool take_result(Result& out);
    // No queued or running job and no uncollected result.
    bool idle() const;
    // Tests / CLI: wait up to `timeout_ms` for the worker to finish every
    // queued job (results may still be waiting for take_result()).
    bool wait_idle(int timeout_ms) const;

    struct Shared; // worker state; public so the process-wide registry can own it

private:
    friend size_t drain_writers();
    static void request_close(Shared& s, bool finalize);
    std::shared_ptr<Shared> shared_;
};

// ONE process-wide shutdown deadline: fixed by the first call (now + the
// configured timeout, kShutdownTimeoutMs by default); every later call returns
// the same time point, so shutdown never waits twice.
std::chrono::steady_clock::time_point shutdown_deadline();
void set_shutdown_timeout_ms(int ms);  // before the first shutdown_deadline()
void reset_shutdown_for_tests();       // tests only: forget the fixed deadline

// Orderly shutdown: close every track still open (writing its footer), let the
// workers finish their queues and join them, until shutdown_deadline().
// Workers still stuck on storage after it, and write errors nobody collected
// (e.g. the footer failing with ENOSPC after the screen went away), are logged
// once to stderr with the affected file and counted in the return value;
// callers then exit non-zero. Called by main() after the app loop / --gps-tracker, and from an
// atexit() hook (LVGL's SDL window close calls exit() directly), which exits
// with status 1 if writes are unfinished.
size_t drain_writers();

// Workers that have not finished yet (tests / diagnostics).
size_t live_writers();

} // namespace platform::gps
