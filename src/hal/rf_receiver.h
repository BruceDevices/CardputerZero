/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "cc1101.h"
#include "rf_capture.h"

namespace hal {

class RfReceiver {
public:
    ~RfReceiver() { stop(); }

    // Returns an error message on failure, nullptr on success.
    const char* start(float mhz, bool capture_pulses);
    void stop(std::deque<platform::RfRxFrame>* pending = nullptr);
    bool active() const { return radio_.fd >= 0; }
    bool poll(platform::RfRxFrame& frame) { return capture_.poll(frame); }
    uint64_t edges() const { return capture_.edges(); }
    bool tune(float mhz);
    int rssi_dbm() const;

private:
    platform::Cc1101Radio radio_;
    platform::RfRxCapture capture_;
};

} // namespace hal
