/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the physical-ESC state tracker
 * (src/platform/esc_key_tracker.h): per-source state, autorepeat, the evdev
 * SYN_DROPPED resync, and recovery from lost releases. Not part of the CMake
 * build. Run manually (Linux):
 *
 *   g++ -std=c++17 -Isrc/platform tests/esc_key_tracker_selfcheck.cpp \
 *       -o /tmp/esc_key_tracker_check && /tmp/esc_key_tracker_check
 */

#include "esc_key_tracker.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::vector<int> g_edges; // 1 = down, 0 = up

void listener(bool down, void*) { g_edges.push_back(down ? 1 : 0); }

using platform::EscKeyTracker;

void plain_press_release() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.key(3, 1);
    check(t.down() && g_edges == std::vector<int>{1}, "plain: down edge");
    t.key(3, 2);
    t.key(3, 2);
    check(g_edges.size() == 1, "plain: autorepeat is not an edge");
    t.key(3, 1);
    check(g_edges.size() == 1, "plain: duplicate down is not an edge");
    t.key(3, 0);
    check(!t.down() && g_edges == (std::vector<int>{1, 0}), "plain: up edge");
    t.key(3, 0);
    check(g_edges.size() == 2, "plain: duplicate up is not an edge");
}

void two_sources_aggregate() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.key(3, 1);
    t.key(7, 1);
    check(g_edges == std::vector<int>{1}, "multi: second source down, no new edge");
    t.key(3, 0);
    check(t.down() && g_edges.size() == 1, "multi: still held on the other source");
    t.key(7, 0);
    check(!t.down() && g_edges == (std::vector<int>{1, 0}), "multi: up once both are up");
}

void syn_dropped_recovers_lost_release() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.key(3, 1);
    t.syn_dropped(3);
    check(t.resync_pending(3), "dropped: resync pending");
    // Events inside the dropped window are not trusted (evdev protocol): an
    // opposite (up) event must not release the stored hold before the resync.
    t.key(3, 0);
    check(t.down() && g_edges == std::vector<int>{1}, "dropped: key-up ignored while resyncing");
    t.key(3, 1);
    check(t.down() && g_edges == std::vector<int>{1}, "dropped: key-down ignored while resyncing");
    t.resynced(3, 0); // EVIOCGKEY: ESC is actually up - its release was lost
    check(!t.resync_pending(3), "dropped: resync done");
    check(!t.down() && g_edges == (std::vector<int>{1, 0}), "dropped: lost release recovered");
}

void syn_dropped_keeps_real_hold() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.key(3, 1);
    t.syn_dropped(3);
    t.resynced(3, 1);
    check(t.down() && g_edges == std::vector<int>{1}, "dropped: real hold kept, no extra edge");
}

void syn_dropped_finds_lost_press() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.syn_dropped(3);
    t.key(3, 1); // stored up: a down inside the window is ignored too
    check(!t.down() && g_edges.empty(), "dropped: key-down ignored while resyncing (stored up)");
    t.resynced(3, 1);
    check(t.down() && g_edges == std::vector<int>{1}, "dropped: lost press recovered");
}

void failed_query_is_release() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.key(3, 1);
    t.syn_dropped(3);
    t.resynced(3, -1);
    check(!t.down() && g_edges == (std::vector<int>{1, 0}), "dropped: failed EVIOCGKEY = released");
}

void removal_clears_source() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.key(3, 1);
    t.remove(3);
    check(!t.down() && g_edges == (std::vector<int>{1, 0}), "remove: device gone releases");
    t.remove(3);
    t.remove(99);
    check(g_edges.size() == 2, "remove: unknown/repeated removal is harmless");
}

void set_reconciles() {
    EscKeyTracker t;
    g_edges.clear();
    t.set_listener(listener, nullptr);
    t.set(-1, true);
    t.set(-1, true);
    check(g_edges == std::vector<int>{1}, "set: down once");
    t.set(-1, false);
    check(g_edges == (std::vector<int>{1, 0}), "set: hardware check releases a stale hold");
    t.syn_dropped(5);
    t.set(5, false);
    check(!t.resync_pending(5), "set: a direct read also ends a pending resync");
}

void listener_replacement() {
    EscKeyTracker t;
    g_edges.clear();
    int other = 0;
    t.set_listener(listener, &other);
    t.clear_listener(listener, nullptr); // wrong user data: kept
    t.key(3, 1);
    check(g_edges.size() == 1, "listener: clear with other user data is a no-op");
    t.clear_listener(listener, &other);
    t.key(3, 0);
    check(g_edges.size() == 1, "listener: cleared listener sees nothing");
    t.key(3, 1);
    t.set_listener(listener, nullptr);
    check(g_edges.size() == 1, "listener: new listener is not told about an old hold");
    t.key(3, 0);
    check(g_edges.size() == 2 && g_edges.back() == 0, "listener: new listener gets the next edge");
}

} // namespace

int main() {
    plain_press_release();
    two_sources_aggregate();
    syn_dropped_recovers_lost_release();
    syn_dropped_keeps_real_hold();
    syn_dropped_finds_lost_press();
    failed_query_is_release();
    removal_clears_source();
    set_reconciles();
    listener_replacement();

    std::printf("esc_key_tracker_selfcheck: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
