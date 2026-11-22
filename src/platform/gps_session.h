/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * In-app GPS serial reader: the Linux stand-in for Bruce firmware's
 * `HardwareSerial GPSserial` + TinyGPS++ pair. No gpsd, no gpspipe.
 *
 * Ownership: one Session owns the port from open() to close(), exclusively
 * (TIOCEXCL on a tty plus flock(), so a second tracker / another reader gets
 * "busy" instead of silently splitting the byte stream). GPS features open it
 * on entry and close it on stop/back, so nothing holds the UART (and no thread
 * runs) while no GPS feature is active.
 *
 * Threading: a reader thread blocks in poll() on the port and a wake pipe -
 * zero CPU while no bytes arrive - and feeds every byte into the Parser under a
 * mutex. The UI thread polls with take_available() / with_gps() from an
 * lv_timer and never blocks on the serial port.
 */

#pragma once

#include "gps_parser.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace platform::gps {

enum class OpenError {
    None,
    NotFound,   // ENOENT/ENODEV/ENXIO: no such node (Cap/USB GPS missing)
    Permission, // EACCES/EPERM: user not in the dialout group
    Busy,       // EBUSY / locked: another process holds the port (gpsd, a 2nd tracker)
    Setup,      // the baud could not be applied
    Other,
};

struct OpenResult {
    OpenError error = OpenError::None;
    std::string message; // one-line, user-facing
};

class Session {
public:
    Session() = default;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Opens `port` read-only and sets `baud` (termios2/BOTHER, see
    // uart_speed.h) when it is a tty. A non-tty (FIFO, file) is read as is,
    // which lets tests and the desktop build feed canned NMEA via GPS_UART.
    bool open(const std::string& port, int baud, OpenResult& result);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Bytes received since the previous call: Bruce's `GPSserial.available()`
    // check (the bytes themselves are already parsed).
    size_t take_available();

    // True once the port really hung up / hit EOF (USB GPS unplugged, FIFO
    // writer gone). An empty tty queue is not a hang-up.
    bool hung_up() const { return hung_up_.load(); }

    // Run `fn(Parser&)` with the parser locked.
    template <class Fn>
    auto with_gps(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(parser_);
    }

    // The front ends' one synchronized read: `fn(bytes, hung_up, parser)` with
    // the bytes received since the last snapshot, the hang-up flag and the
    // parser taken under the same lock the reader feeds with. The reader sets
    // the flag under that lock only after the final bytes were parsed, so a
    // snapshot that sees the hang-up also sees (or already took) every byte
    // before it - a last fix right before EOF is never dropped.
    template <class Fn>
    void snapshot(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t bytes = available_;
        available_ = 0;
        fn(bytes, hung_up_.load(), parser_);
    }

private:
    void reader_main();
    void mark_hung_up(); // under mutex_, after any bytes it read were parsed

    int fd_ = -1;
    bool is_tty_ = false;
    int wake_[2] = {-1, -1};
    std::thread reader_;
    std::mutex mutex_;
    Parser parser_;
    size_t available_ = 0;
    std::atomic<bool> hung_up_{false};
};

// Bruce's global `gpsConnected`: true while a GPS feature runs and has received
// data. Drives the title-bar GPS pin (Bruce: drawGpsSmall in drawStatusBar).
bool connected();
void set_connected(bool value);

} // namespace platform::gps
