/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * GPS serial port discovery for Config > GPS Port and the "Auto" port:
 * which device nodes can carry a GPS (the HAT UART and USB GPS dongles), and a
 * short NMEA probe that finds the one that is actually streaming.
 *
 * No thread of its own and no state: the GPS screen runs auto_detect() on a
 * worker thread while it shows "Searching GPS...", the CLI front ends call it
 * directly. Each candidate is opened only for its probe window and closed
 * again, so nothing holds a port while no GPS feature runs.
 */

#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace platform::gps {

// Existing candidate nodes, in this order, without duplicates (a by-id link
// to a ttyACM node that is also listed is shown once, as the by-id name):
//   /dev/serial0, /dev/serial/by-id/* (sorted), /dev/ttyACM* , /dev/ttyUSB*
//   (natural order: ttyUSB2 before ttyUSB10).
// $BRUCE_GPS_PORT_CANDIDATES (':'-separated paths) replaces the list - a test
// hook for ptys / fake nodes; only entries that exist are returned.
std::vector<std::string> port_candidates();

// Same, scanning `dev_root` instead of /dev (tests build a fake /dev tree).
std::vector<std::string> port_candidates_in(const std::string& dev_root);

// The Auto probe order: `preferred` (the board default) first when it exists,
// then the remaining candidates.
std::vector<std::string> auto_order(const std::string& preferred, const std::vector<std::string>& candidates);

struct ProbeResult {
    enum class Kind {
        Found,        // `port` streams valid NMEA at `baud`
        NoCandidates, // no candidate node exists
        NoNmea,       // nodes opened, none sent NMEA at any baud
        Permission,   // no NMEA, and at least one node gave EACCES/EPERM
        Busy,         // no NMEA, and at least one node was held by another process
        Cancelled,
    };
    Kind kind = Kind::NoCandidates;
    std::string port;
    int baud = 0;
    // One-line, user-facing (the error popup text when kind != Found).
    std::string message;
    std::vector<std::string> tried;
};

// Probe `ports` for NMEA (a sentence passing its checksum): first every port
// at `preferred_baud`, then every port at each of the other `bauds`. Each try
// lasts at most `ms_per_try` ms; `cancel` (may be null) is checked every
// <=100 ms. A non-tty node (FIFO/file, tests) is read as is, once.
ProbeResult auto_detect(const std::vector<std::string>& ports, int preferred_baud, const std::vector<int>& bauds,
                        int ms_per_try, const std::atomic<bool>* cancel = nullptr);

// One probe of one node at one baud. Returns 0 on NMEA, else an errno-like
// code: ETIMEDOUT (opened, no NMEA), ENOENT, EACCES, EBUSY, ECANCELED, ...
int probe_port(const std::string& port, int baud, int ms, const std::atomic<bool>* cancel);

// Popup text for a port that cannot be opened for lack of permission (shared
// with Session::open, hence inline: no extra translation unit to link).
inline std::string permission_message(const std::string& port) {
    return "Cannot open " + port + ": permission denied - add your user to the dialout group";
}

} // namespace platform::gps
