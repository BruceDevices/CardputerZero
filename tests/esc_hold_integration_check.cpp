/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Integration check for the "Hold ESC 3s to return home" banner: the real
 * src/platform/linux_input.cpp + src/view/widgets/esc_hold_hint.cpp on a
 * headless LVGL display, driven like the app (lv_tick_inc + lv_timer_handler).
 * Not part of the CMake build; it links the LVGL library of a desktop build.
 *
 * Built twice (Linux). LVGL is always the desktop build, so LV_CONF_PATH points
 * at lv_conf_desktop.h directly; USE_DESKTOP picks Bruce's input path.
 *
 *   L=<FETCHCONTENT_BASE_DIR of a desktop build>   # holds lvgl-src/ and lvgl-build/
 *   FLAGS="-std=c++17 -DLV_CONF_PATH=\"$PWD/src/config/lv_conf_desktop.h\" \
 *     -DLV_CONF_SUPPRESS_DEFINE_CHECK -DLV_KCONFIG_IGNORE -DLV_LVGL_H_INCLUDE_SIMPLE \
 *     -Isrc/platform -Isrc/view/widgets -isystem $L/lvgl-src -isystem src/config \
 *     -isystem $L/lvgl-build -isystem /usr/include/SDL2"
 *   SRCS="tests/esc_hold_integration_check.cpp src/platform/linux_input.cpp \
 *     src/view/widgets/esc_hold_hint.cpp $L/lvgl-build/lib/liblvgl.a \
 *     $L/lvgl-build/lib/liblvgl_thorvg.a"
 *   LIBS="-lSDL2 -lfreetype -lpng -ljpeg -lz -ldl -lpthread -lm"
 *   # device path: evdev through a FIFO, ioctl()/read() interposed below
 *   g++ $FLAGS -DUSE_DESKTOP=0 \
 *       -DAPP_KEY_INPUT_DEVICE=\"/tmp/bruce_esc_hold_check.fifo\" \
 *       $SRCS $LIBS -o /tmp/esc_hold_int_dev && /tmp/esc_hold_int_dev
 *   # desktop path: SDL events, incl. pushes from a worker thread
 *   g++ $FLAGS -DUSE_DESKTOP=1 $SRCS $LIBS -o /tmp/esc_hold_int_sdl && /tmp/esc_hold_int_sdl
 *
 * Covers: physical ESC vs Backspace/grave, autorepeat, release before/after
 * 500 ms, lost releases (SYN_DROPPED resync, silent loss caught by the in-hold
 * hardware check, read error, SDL focus loss), SDL release/repress bursts
 * between two LVGL reads restarting the hold, short ESC still Back, banner
 * kept across a screen load, no timer left once ESC is up, banner deleted with
 * its controller, and listener calls only on the LVGL thread.
 */

#include "esc_hold_hint.h"
#include "linux_input.h"
#include "lvgl.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>

#if USE_DESKTOP
#include <SDL.h>
#else
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#endif

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

void run_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        lv_tick_inc(1);
        lv_timer_handler();
    }
}

size_t timer_count() {
    size_t n = 0;
    for (auto* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
        ++n;
    }
    return n;
}

lv_obj_t* g_box = nullptr; // the banner (last layer_top child after creation)

bool banner_visible() {
    return g_box && lv_obj_is_valid(g_box) && !lv_obj_has_flag(g_box, LV_OBJ_FLAG_HIDDEN);
}

// Raw listener used before the widget exists: records calls and their thread.
std::thread::id g_lvgl_thread;
int g_raw_calls = 0;
bool g_raw_off_thread = false;
void raw_listener(bool, void*) {
    ++g_raw_calls;
    g_raw_off_thread = g_raw_off_thread || std::this_thread::get_id() != g_lvgl_thread;
}

uint8_t g_draw_buf[320 * 20 * 4];
void flush_cb(lv_display_t* display, const lv_area_t*, uint8_t*) {
    lv_display_flush_ready(display);
}

} // namespace

#if !USE_DESKTOP
// ---- fake evdev: a FIFO carries input_event structs; ioctl()/read() on it are
// answered here (EVIOCGBIT -> has nav keys, EVIOCGKEY -> g_hw_esc).
namespace {
int g_fifo_fd = -1;     // the app's (reader) fd, learned from EVIOCGBIT
int g_writer = -1;
int g_hw_esc = 0;       // kernel key state reported by EVIOCGKEY
int g_read_errno = 0;   // non-zero: read() on the device fails with it
int g_ioctl_errno = 0;  // non-zero: EVIOCGKEY fails with it

int g_back_clicks = 0; // nav slot 0 (Back) activations
void back_clicked_cb(lv_event_t*) { ++g_back_clicks; }

void send_ev(uint16_t type, uint16_t code, int32_t value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(g_writer, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev))) {
        std::perror("fifo write");
        std::exit(2);
    }
}

void key_ev(uint16_t code, int32_t value) {
    if (code == KEY_ESC && value != 2) {
        g_hw_esc = value;
    }
    send_ev(EV_KEY, code, value);
    send_ev(EV_SYN, SYN_REPORT, 0);
}
} // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x20 + EV_KEY) { // EVIOCGBIT(EV_KEY)
        g_fifo_fd = fd;
        auto* bits = static_cast<unsigned long*>(arg);
        for (size_t i = 0; i < _IOC_SIZE(request) / sizeof(unsigned long); ++i) {
            bits[i] = 0;
        }
        bits[KEY_F / (sizeof(unsigned long) * 8)] |= 1UL << (KEY_F % (sizeof(unsigned long) * 8));
        return 0;
    }
    if (fd == g_fifo_fd && _IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x18) { // EVIOCGKEY
        if (g_ioctl_errno) {
            errno = g_ioctl_errno;
            return -1;
        }
        auto* bits = static_cast<unsigned long*>(arg);
        for (size_t i = 0; i < _IOC_SIZE(request) / sizeof(unsigned long); ++i) {
            bits[i] = 0;
        }
        if (g_hw_esc) {
            bits[KEY_ESC / (sizeof(unsigned long) * 8)] |= 1UL << (KEY_ESC % (sizeof(unsigned long) * 8));
        }
        return 0;
    }
    using ioctl_fn = int (*)(int, unsigned long, void*);
    static auto real = reinterpret_cast<ioctl_fn>(dlsym(RTLD_NEXT, "ioctl"));
    return real(fd, request, arg);
}

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    if (fd == g_fifo_fd && g_read_errno) {
        errno = g_read_errno;
        return -1;
    }
    using read_fn = ssize_t (*)(int, void*, size_t);
    static auto real = reinterpret_cast<read_fn>(dlsym(RTLD_NEXT, "read"));
    return real(fd, buf, count);
}

namespace {

void device_checks(lv_display_t* display) {
    const char* path = APP_KEY_INPUT_DEVICE;
    unlink(path);
    if (mkfifo(path, 0600) != 0) {
        std::perror("mkfifo");
        std::exit(2);
    }
    platform::init_key_input(display);
    g_writer = open(path, O_WRONLY | O_NONBLOCK);
    check(g_fifo_fd >= 0 && g_writer >= 0, "dev: fake evdev keypad attached");

    // Nav slot 0 (Back) so short-press routing can be observed.
    auto* back = lv_button_create(lv_screen_active());
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_CLICKED, nullptr);
    platform::register_nav_button(0, back);

    // Raw listener: transitions reach it on this (the LVGL) thread.
    g_lvgl_thread = std::this_thread::get_id();
    platform::set_esc_key_listener(raw_listener, nullptr);
    key_ev(KEY_ESC, 1);
    run_ms(40);
    key_ev(KEY_ESC, 0);
    run_ms(200);
    check(g_raw_calls == 2 && !g_raw_off_thread, "dev: listener down+up on the LVGL thread");
    platform::clear_esc_key_listener(raw_listener, nullptr);

    auto* top = lv_layer_top();
    const uint32_t children_before = lv_obj_get_child_count(top);
    {
        view::widgets::EscHoldHint hint(top);
        g_box = lv_obj_get_child(top, -1);
        const size_t idle_timers = timer_count();
        check(!banner_visible(), "dev: banner hidden at start");

        // Short tap: Back on press, no banner.
        g_back_clicks = 0;
        key_ev(KEY_ESC, 1);
        run_ms(20);
        check(g_back_clicks == 1, "dev: short ESC is Back on press");
        run_ms(180);
        key_ev(KEY_ESC, 0);
        run_ms(800);
        check(!banner_visible(), "dev: short tap shows nothing");
        check(timer_count() == idle_timers, "dev: no timer left after a tap");

        // Hold: hidden before 500 ms, shown after, autorepeat ignored.
        key_ev(KEY_ESC, 1);
        run_ms(470);
        check(!banner_visible(), "dev: hidden before 500 ms");
        key_ev(KEY_ESC, 2);
        run_ms(70);
        check(banner_visible(), "dev: shown after 500 ms despite autorepeat");
        for (int i = 0; i < 20; ++i) {
            key_ev(KEY_ESC, 2);
            run_ms(50);
        }
        check(banner_visible(), "dev: still shown at ~1.5 s (no self-quit, no flicker)");
        lv_obj_t* other = lv_obj_create(nullptr);
        lv_screen_load(other);
        run_ms(20);
        check(banner_visible(), "dev: banner kept across a screen load");
        key_ev(KEY_ESC, 0);
        run_ms(40);
        check(!banner_visible(), "dev: release hides");
        check(timer_count() == idle_timers, "dev: no timer left once ESC is up");

        // Backspace and grave map to Back but are not the physical ESC.
        g_back_clicks = 0;
        key_ev(KEY_BACKSPACE, 1);
        run_ms(300);
        check(timer_count() == idle_timers, "dev: Backspace hold starts no hold timer");
        run_ms(600);
        check(!banner_visible(), "dev: Backspace hold shows nothing");
        key_ev(KEY_BACKSPACE, 0);
        run_ms(200);
        key_ev(KEY_GRAVE, 1);
        run_ms(300);
        check(timer_count() == idle_timers, "dev: grave hold starts no hold timer");
        run_ms(600);
        check(!banner_visible(), "dev: grave hold shows nothing");
        key_ev(KEY_GRAVE, 0);
        run_ms(200);
        check(g_back_clicks == 2, "dev: Backspace/grave still route as Back");
        check(timer_count() == idle_timers, "dev: Backspace/grave start no timer");

        // Lost release, kernel reports SYN_DROPPED (queue overflow).
        key_ev(KEY_ESC, 1);
        run_ms(600);
        check(banner_visible(), "dev: shown before SYN_DROPPED");
        g_hw_esc = 0; // released, but the EV_KEY 0 was dropped
        send_ev(EV_SYN, SYN_DROPPED, 0);
        send_ev(EV_SYN, SYN_REPORT, 0);
        run_ms(40);
        check(!banner_visible(), "dev: SYN_DROPPED resync hides a stale banner");
        check(timer_count() == idle_timers, "dev: no timer left after resync");

        // Silent loss (no SYN_DROPPED): the in-hold hardware check catches it.
        key_ev(KEY_ESC, 1);
        run_ms(600);
        check(banner_visible(), "dev: shown before silent loss");
        g_hw_esc = 0;
        run_ms(300);
        check(!banner_visible(), "dev: in-hold EVIOCGKEY check hides a stale banner");
        check(timer_count() == idle_timers, "dev: no timer left after the check");
        send_ev(EV_KEY, KEY_ESC, 0); // the late release is harmless
        send_ev(EV_SYN, SYN_REPORT, 0);
        run_ms(40);

        // Silent loss before 500 ms: never shown.
        key_ev(KEY_ESC, 1);
        run_ms(100);
        g_hw_esc = 0;
        run_ms(800);
        check(!banner_visible(), "dev: release lost before 500 ms never shows");
        send_ev(EV_KEY, KEY_ESC, 0);
        send_ev(EV_SYN, SYN_REPORT, 0);
        run_ms(40);

        // Read error (device removed): the source is dropped.
        key_ev(KEY_ESC, 1);
        run_ms(600);
        check(banner_visible(), "dev: shown before read error");
        g_read_errno = ENODEV;
        g_ioctl_errno = ENODEV;
        run_ms(40);
        check(!banner_visible(), "dev: read error hides the banner");
        g_read_errno = 0;
        g_ioctl_errno = 0;
        g_hw_esc = 0;
        send_ev(EV_KEY, KEY_ESC, 0);
        send_ev(EV_SYN, SYN_REPORT, 0);
        run_ms(40);
        check(timer_count() == idle_timers, "dev: no timer left after read error");

        // A fresh hold still works after all that.
        key_ev(KEY_ESC, 1);
        run_ms(600);
        check(banner_visible(), "dev: next hold shows again");
    }
    // Controller destroyed mid-hold: banner and timer go with it.
    check(lv_obj_get_child_count(top) == children_before, "dev: banner deleted with its controller");
    key_ev(KEY_ESC, 0);
    run_ms(40);

    close(g_writer);
    unlink(path);
}

} // namespace
#else
// ---- desktop: SDL events through the app's SDL event watch.
namespace {

void push_key(Uint32 type, SDL_Keycode sym, Uint8 repeat = 0) {
    SDL_Event e{};
    e.type = type;
    e.key.keysym.sym = sym;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

void indev_read_cb(lv_indev_t*, lv_indev_data_t* data) {
    data->state = LV_INDEV_STATE_RELEASED;
}

void desktop_checks(lv_display_t* display) {
    SDL_Init(SDL_INIT_EVENTS);
    platform::init_key_input(display); // installs the SDL event watch
    // Stand-in for LVGL's SDL keyboard indev: its read cycle drains ESC state.
    auto* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, indev_read_cb);
    platform::attach_key_router(indev);

    g_lvgl_thread = std::this_thread::get_id();
    platform::set_esc_key_listener(raw_listener, nullptr);
    std::thread worker([] { push_key(SDL_KEYDOWN, SDLK_ESCAPE); });
    worker.join();
    check(g_raw_calls == 0, "sdl: worker-thread push does not call the listener off-thread");
    run_ms(40);
    check(g_raw_calls == 1 && !g_raw_off_thread, "sdl: transition applied on the LVGL thread");
    std::thread worker_up([] { push_key(SDL_KEYUP, SDLK_ESCAPE); });
    worker_up.join();
    run_ms(40);
    check(g_raw_calls == 2 && !g_raw_off_thread, "sdl: release applied on the LVGL thread");
    platform::clear_esc_key_listener(raw_listener, nullptr);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    auto* top = lv_layer_top();
    const uint32_t children_before = lv_obj_get_child_count(top);
    {
        view::widgets::EscHoldHint hint(top);
        g_box = lv_obj_get_child(top, -1);
        const size_t idle_timers = timer_count();

        push_key(SDL_KEYDOWN, SDLK_ESCAPE);
        run_ms(470);
        check(!banner_visible(), "sdl: hidden before 500 ms");
        push_key(SDL_KEYDOWN, SDLK_ESCAPE, 1); // OS autorepeat
        run_ms(90);
        check(banner_visible(), "sdl: shown after 500 ms despite repeats");
        push_key(SDL_KEYUP, SDLK_ESCAPE);
        run_ms(40);
        check(!banner_visible(), "sdl: release hides");
        check(timer_count() == idle_timers, "sdl: no timer left once ESC is up");

        push_key(SDL_KEYDOWN, SDLK_ESCAPE);
        run_ms(200);
        push_key(SDL_KEYUP, SDLK_ESCAPE);
        run_ms(800);
        check(!banner_visible(), "sdl: short tap shows nothing");

        push_key(SDL_KEYDOWN, SDLK_BACKSPACE);
        run_ms(300);
        check(timer_count() == idle_timers, "sdl: Backspace hold starts no hold timer");
        run_ms(600);
        check(!banner_visible(), "sdl: Backspace hold shows nothing");
        push_key(SDL_KEYUP, SDLK_BACKSPACE);

        // Bursts between two LVGL reads: a release (or focus loss) followed by
        // a new press must end the old hold and restart the 500 ms clock.
        push_key(SDL_KEYDOWN, SDLK_ESCAPE);
        run_ms(400);
        push_key(SDL_KEYUP, SDLK_ESCAPE);
        push_key(SDL_KEYDOWN, SDLK_ESCAPE); // no loop iteration in between
        run_ms(180);
        check(!banner_visible(), "sdl: up+down burst restarts the 500 ms hold");
        run_ms(400);
        check(banner_visible(), "sdl: the new hold shows 500 ms after the repress");
        push_key(SDL_KEYUP, SDLK_ESCAPE);
        push_key(SDL_KEYDOWN, SDLK_ESCAPE); // burst while the banner is up
        run_ms(20);
        check(!banner_visible(), "sdl: up+down burst hides a visible banner");
        run_ms(560);
        check(banner_visible(), "sdl: ...and shows it again after a fresh 500 ms");
        std::thread burst([] {
            SDL_Event focus{};
            focus.type = SDL_WINDOWEVENT;
            focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&focus);
            push_key(SDL_KEYDOWN, SDLK_ESCAPE);
        });
        burst.join();
        run_ms(20);
        check(!banner_visible(), "sdl: focus-loss+down burst (worker thread) ends the old hold");
        run_ms(560);
        check(banner_visible(), "sdl: ...and the new hold shows after 500 ms");
        push_key(SDL_KEYUP, SDLK_ESCAPE);
        run_ms(40);
        check(timer_count() == idle_timers, "sdl: no timer left after the bursts");

        // Lost key-up: focus loss ends the hold.
        push_key(SDL_KEYDOWN, SDLK_ESCAPE);
        run_ms(600);
        check(banner_visible(), "sdl: shown before focus loss");
        SDL_Event focus{};
        focus.type = SDL_WINDOWEVENT;
        focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        SDL_PushEvent(&focus);
        run_ms(40);
        check(!banner_visible(), "sdl: focus loss hides the banner");
        check(timer_count() == idle_timers, "sdl: no timer left after focus loss");

        // Hold started from a worker-thread push: shown, on the LVGL thread.
        std::thread worker_hold([] { push_key(SDL_KEYDOWN, SDLK_ESCAPE); });
        worker_hold.join();
        run_ms(600);
        check(banner_visible(), "sdl: worker-thread hold shows the banner");
    }
    check(lv_obj_get_child_count(top) == children_before, "sdl: banner deleted with its controller");
    push_key(SDL_KEYUP, SDLK_ESCAPE);
    run_ms(40);
    SDL_Quit();
}

} // namespace
#endif

int main() {
    lv_init();
    auto* display = lv_display_create(320, 170);
    lv_display_set_buffers(display, g_draw_buf, nullptr, sizeof(g_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);
    run_ms(20);

#if USE_DESKTOP
    desktop_checks(display);
    const char* mode = "sdl";
#else
    device_checks(display);
    const char* mode = "evdev";
#endif

    std::printf("esc_hold_integration_check (%s): %d/%d checks passed\n", mode,
                g_checks - g_failures, g_checks);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
