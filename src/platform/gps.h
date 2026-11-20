/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * GPS presence detection for the CardputerZero CAP LORA (GPS on the HAT UART:
 * CM0 TX=G14, RX=G15 -> /dev/serial0). Detection is by NMEA: a running receiver
 * streams `$G...` sentences even without a position fix, so weak/no signal is
 * fine - we only confirm the module is wired and talking, and learn its baud.
 */

#pragma once

#include <string>

namespace platform {

// Scan a UART for NMEA output. Tries each baud in `bauds` (reading up to
// `ms_per_baud` ms per baud), stopping at the first that yields a valid-looking
// `$G..*HH` sentence. On success returns true, sets `baud_found` and copies one
// sample sentence into `sample`. `bauds`/`n_bauds` default (nullptr/0) uses the
// built-in list {115200, 9600, 38400, 57600} (115200 is the CAP LORA's rate).
bool probe_gps(const char* uart, const int* bauds, int n_bauds, int ms_per_baud,
               int& baud_found, std::string& sample);

// Entry point for the `--gps-detect [uart]` subcommand: scans the baud list,
// prints each attempt and a sample sentence, returns 0 when NMEA was seen.
int gps_detect_main(int argc, char** argv);

} // namespace platform
