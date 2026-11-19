/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rf_recording.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(__cpp_lib_filesystem) || __has_include(<filesystem>)
#include <filesystem>
#endif

#if defined(__linux__)
#include <ctime>
#endif

namespace platform {

uint64_t rf_monotonic_ms() {
#if defined(__linux__)
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
#else
    return 0;
#endif
}

void RawRecording::add_frame(std::vector<int> durations, uint64_t arrival_ms) {
    int frame_dur_ms = 0;
    for (int d : durations) frame_dur_ms += (d < 0 ? -d : d);
    frame_dur_ms /= 1000;

    if (_have_prev) {
        long gap = static_cast<long>(arrival_ms) - static_cast<long>(_last_arrival_ms) -
                   _last_frame_dur_ms - 5;
        if (gap < 0) gap = 0;
        gaps_ms.push_back(static_cast<int>(gap));
    }

    frames.push_back(std::move(durations));
    _have_prev = true;
    _last_arrival_ms = arrival_ms;
    _last_frame_dur_ms = frame_dur_ms;
}

size_t RawRecording::total_durations() const {
    size_t n = 0;
    for (const auto& f : frames) n += f.size();
    return n;
}

std::string rf_save_sub(const RawRecording& rec, const std::string& dir) {
    if (rec.frames.empty()) return "";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // ok if it already exists

    std::string path;
    for (int i = 0;; ++i) {
        path = dir + "/raw_" + std::to_string(i) + ".sub";
        if (!std::filesystem::exists(path)) break;
        if (i > 100000) return ""; // guard against a pathological loop
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return "";

    // Header — byte-for-byte Bruce's save.cpp (Filetype/Version/Frequency/Preset/
    // Protocol). Frequency is written in Hz.
    std::fputs("Filetype: Bruce SubGhz File\n", f);
    std::fputs("Version 1\n", f);
    std::fprintf(f, "Frequency: %d\n", static_cast<int>(rec.frequency_hz));
    std::fputs("Preset: 0\n", f);
    std::fputs("Protocol: RAW\n", f);
    std::fputs("RAW_Data:", f);

    // Flipper RAW allows at most 512 signed durations per RAW_Data line; start a
    // new "RAW_Data:" line each time we hit that boundary.
    int count = 0;
    auto emit = [&](int v) {
        if (count > 0 && count % 512 == 0) std::fputs("\nRAW_Data:", f);
        std::fprintf(f, " %d", v);
        ++count;
    };

    for (size_t i = 0; i < rec.frames.size(); ++i) {
        for (int d : rec.frames[i]) emit(d);
        if (i + 1 < rec.frames.size()) {
            // Gap between bursts: a long LOW duration (negative µs), ms * -1000,
            // same as Bruce.
            emit(rec.gaps_ms[i] * -1000);
        }
    }
    std::fputc('\n', f);
    std::fclose(f);
    return path;
}

bool rf_load_sub(const std::string& path, RawRecording& rec, std::string& error) {
    rec = RawRecording{};
    error.clear();

    std::ifstream in(path);
    if (!in) {
        error = "could not open file";
        return false;
    }

    bool raw_protocol = false;
    bool have_frequency = false;
    std::vector<int> durations;
    std::string line;
    size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.rfind("Frequency:", 0) == 0) {
            std::istringstream values(line.substr(10));
            double hz = 0.0;
            if (!(values >> hz) || hz <= 0.0) {
                error = "invalid Frequency on line " + std::to_string(line_number);
                return false;
            }
            rec.frequency_hz = hz;
            have_frequency = true;
        } else if (line.rfind("Protocol:", 0) == 0) {
            std::istringstream value(line.substr(9));
            std::string protocol;
            value >> protocol;
            raw_protocol = (protocol == "RAW");
        } else if (line.rfind("RAW_Data:", 0) == 0) {
            std::istringstream values(line.substr(9));
            long long duration = 0;
            while (values >> duration) {
                if (duration == 0 || duration < std::numeric_limits<int>::min() ||
                    duration > std::numeric_limits<int>::max()) {
                    error = "invalid RAW duration on line " + std::to_string(line_number);
                    return false;
                }
                durations.push_back(static_cast<int>(duration));
            }
            if (!values.eof()) {
                error = "invalid RAW_Data on line " + std::to_string(line_number);
                return false;
            }
        }
    }

    if (!have_frequency) {
        error = "missing Frequency";
        return false;
    }
    if (!raw_protocol) {
        error = "only Protocol: RAW is supported";
        return false;
    }
    if (durations.empty()) {
        error = "missing RAW_Data durations";
        return false;
    }
    rec.frames.push_back(std::move(durations));
    return true;
}

} // namespace platform
