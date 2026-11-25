/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Physical-ESC state across input sources (each evdev keypad, the desktop SDL
 * keyboard), free of LVGL/SDL/evdev so it can be unit-tested
 * (tests/esc_key_tracker_selfcheck.cpp). Single-threaded: only the LVGL thread
 * calls it (linux_input.cpp hands SDL transitions over via an atomic).
 *
 * The listener sees edges of "ESC held on any source". Besides plain key
 * events, a source can be corrected from hardware truth: after an evdev
 * SYN_DROPPED (lost events) the next SYN_REPORT re-reads the key state
 * (EVIOCGKEY); a read error / removed device / lost window focus clears the
 * source. So a lost release can never latch the hold.
 */

#pragma once

#include <map>

namespace platform {

class EscKeyTracker {
public:
    using Listener = void (*)(bool down, void* user_data);

    void set_listener(Listener listener, void* user_data) {
        listener_ = listener;
        user_data_ = user_data;
    }

    void clear_listener(Listener listener, void* user_data) {
        if (listener_ == listener && user_data_ == user_data) {
            listener_ = nullptr;
            user_data_ = nullptr;
        }
    }

    // evdev EV_KEY KEY_ESC from `source` (value 0 up, 1 down, 2 autorepeat).
    // Ignored while the source is resyncing after SYN_DROPPED, as the evdev
    // protocol requires; the following SYN_REPORT supplies the truth.
    void key(int source, int value) {
        if (value == 2) {
            return;
        }
        auto& s = sources_[source];
        if (s.resync) {
            return;
        }
        s.down = value != 0;
        update();
    }

    // evdev SYN_DROPPED: events were lost, ESC state may be stale.
    void syn_dropped(int source) { sources_[source].resync = true; }

    bool resync_pending(int source) const {
        const auto it = sources_.find(source);
        return it != sources_.end() && it->second.resync;
    }

    // evdev SYN_REPORT after SYN_DROPPED: `hardware_down` is the EVIOCGKEY
    // bit, or -1 if the query failed (treated as released).
    void resynced(int source, int hardware_down) {
        auto& s = sources_[source];
        s.resync = false;
        s.down = hardware_down > 0;
        update();
    }

    // Direct state from a trusted source (SDL down/up, an EVIOCGKEY check).
    void set(int source, bool down) {
        auto& s = sources_[source];
        s.resync = false;
        s.down = down;
        update();
    }

    // Source gone or unreliable (device removed, read error, focus lost).
    void remove(int source) {
        sources_.erase(source);
        update();
    }

    bool down() const { return down_; }

private:
    struct Source {
        bool down{false};
        bool resync{false};
    };

    void update() {
        bool any = false;
        for (const auto& entry : sources_) {
            any = any || entry.second.down;
        }
        if (any == down_) {
            return;
        }
        down_ = any;
        if (listener_) {
            listener_(down_, user_data_);
        }
    }

    std::map<int, Source> sources_;
    bool down_{false};
    Listener listener_{nullptr};
    void* user_data_{nullptr};
};

} // namespace platform
