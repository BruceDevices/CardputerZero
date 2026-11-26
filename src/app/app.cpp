/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include <csignal>
#include <unistd.h>

#include "asset_manager.h"
#include "boot_anim.h"
#include "bruce_module.h"
#include "gps_config.h"
#include "logger.h"
#include "linux_input.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "esc_hold_hint.h"
#include "theme.h"
#include "screenshot_service.h"
#include "toast.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#if USE_DESKTOP
#include <SDL.h>
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

namespace app {
namespace {

// SIGTERM/SIGINT end the main loop like a quit request, so the screens are torn
// down and main() can drain pending storage work (GPX writes). Any second
// SIGINT/SIGTERM - same or different signal - forces the exit at once, exactly
// like --gps-tracker.
volatile std::sig_atomic_t g_terminate = 0;

void on_terminate(int sig) {
    if (g_terminate) {
        _exit(128 + sig);
    }
    g_terminate = 1;
}

#if USE_DESKTOP
// Closing the SDL window: LVGL's SDL driver answers SDL_QUIT with lv_deinit() +
// exit(0) (and SDL_WINDOWEVENT_CLOSE with lv_display_delete), tearing FreeType
// down while AssetManager still holds its fonts ("drop_all_cb: entry ... is
// still referenced") and skipping our own shutdown. Drop both events before
// the driver polls them and end the main loop like SIGTERM instead.
int sdl_quit_filter(void*, SDL_Event* event) {
    const bool close = event->type == SDL_QUIT ||
                       (event->type == SDL_WINDOWEVENT &&
                        event->window.event == SDL_WINDOWEVENT_CLOSE);
    if (close) {
        g_terminate = 1;
        return 0; // drop it
    }
    return 1;
}
#endif

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

// Re-apply the LVGL default theme when dark mode toggles so component
// internals (list rows, textarea) follow the switch, not just the
// bind_theme'd containers.
void dark_mode_theme_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* display = static_cast<lv_display_t*>(lv_observer_get_user_data(observer));
    if (display) {
        view::apply_lvgl_theme(display, lv_subject_get_int(subject) != 0);
    }
}

// Pairs the display with the view model so the accent/theme observer can
// re-apply the LVGL theme using the current dark-mode state.
struct ThemeContext {
    lv_display_t* display;
    viewmodel::BaseViewModel* view_model;
};

// PrintScreen captures the active screen to a PNG, with toast feedback on
// lv_layer_top() so it survives screen switches. Ported from Template's
// ShortcutController without the help popup and exit-hold logic.
class ScreenshotController {
public:
    explicit ScreenshotController(viewmodel::BaseViewModel& view_model)
        : toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model)) {
        toast_->build();
        platform::set_global_key_listener(global_key_listener, this);
    }

    ~ScreenshotController() {
        platform::clear_global_key_listener(global_key_listener, this);
    }

private:
    bool handle_key(uint32_t key) {
        if (key != platform::kKeyPrintScreen) {
            return false;
        }

        const auto result = platform::screenshot::capture_active_screen();
        if (result.success) {
            LOG_INFO("screenshot saved: {}", result.path);
            toast_->show_highlighted("Saved to ",
                                     result.path,
                                     "",
                                     view::widgets::ToastTone::Success,
                                     5200,
                                     true);
        }
        else {
            LOG_WARN("screenshot failed: {}", result.error);
            toast_->show("Screenshot failed: " + result.error,
                         view::widgets::ToastTone::Error,
                         3200);
        }
        return true;
    }

    static bool global_key_listener(uint32_t key, void* user_data) {
        auto* controller = static_cast<ScreenshotController*>(user_data);
        return controller && controller->handle_key(key);
    }

    std::unique_ptr<view::widgets::Toast> toast_;
};

// Re-apply the LVGL theme when the theme epoch bumps (e.g. accent color change).
void theme_epoch_observer(lv_observer_t* observer, lv_subject_t* subject) {
    LV_UNUSED(subject);
    auto* ctx = static_cast<ThemeContext*>(lv_observer_get_user_data(observer));
    if (ctx && ctx->display && ctx->view_model) {
        view::apply_lvgl_theme(ctx->display, ctx->view_model->is_dark_mode());
    }
}

lv_display_t* init_display() {
#if USE_DESKTOP
    auto* display = lv_sdl_window_create(view::kScreenWidth, view::kScreenHeight);
    if (!display) {
        return nullptr;
    }

    lv_sdl_window_set_title(display, "Bruce CardputerZero");
    lv_sdl_window_set_resizeable(display, false);
    SDL_SetEventFilter(sdl_quit_filter, nullptr);
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    auto* keyboard = lv_sdl_keyboard_create();
    platform::attach_key_router(keyboard);
    platform::init_key_input(display); // registers the SDL PrintScreen watch
    return display;
#elif APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("bruce");

    lv_init();

    // LVGL's lv_delay_ms() busy-waits unless a delay callback is registered.
    // The fbdev/DRM drivers don't set one (SDL later overrides this with
    // SDL_Delay), so provide a real sleep for the device build.
    lv_delay_set_cb([](uint32_t ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds{ms});
    });

    // Detect the connected CAP and fill the BruceModule pin registry (Subjects
    // need lv_init() done above). Prints the module summary to the terminal.
    hal::detect_cap();
    // GPS board default (Bruce: per-board gpsBaudrate): the LoRa+GNSS Cap's
    // receiver runs at 115200, anything else gets Bruce's 9600. Used until
    // the user saves GPS > Config (~/.bruce/gps.conf).
    model::gps_config::set_board_default(
        model::gps_config::board_default(hal::modules().kind == hal::CapKind::CapLora));

    auto* display = init_display();
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    viewmodel::BaseViewModel view_model;
    view::load_accent(); // restore the user's chosen accent color before theming
    view::apply_lvgl_theme(display, view_model.is_dark_mode());

    ScreenManager screen_manager(view_model, assets);
    screen_manager.start();

    // Global PrintScreen shortcut: owns the toast on lv_layer_top() for the
    // app's lifetime and unregisters its listener when the loop exits.
    ScreenshotController screenshot_shortcuts(view_model);

    // "Hold ESC 3s to return home" banner after 0.5 s of physical ESC, over
    // every screen. Display only: APPLaunch itself ends the app at 3 s.
    view::widgets::EscHoldHint esc_hold_hint(lv_layer_top());

    // Bruce boot animation: the shark lunges toward the screen over the menu,
    // then fades out. Non-blocking; driven by the LVGL timer loop below.
    view::play_boot_animation(assets, view::palette(view_model.is_dark_mode()).primary);

    bool running = true;
    auto* quit_observer = lv_subject_add_observer(view_model.quit_requested_subject(),
                                                  quit_requested_observer,
                                                  &running);
    auto* theme_observer = lv_subject_add_observer(view_model.dark_mode_subject(),
                                                   dark_mode_theme_observer,
                                                   display);
    ThemeContext theme_context{display, &view_model};
    auto* accent_observer = lv_subject_add_observer(view_model.theme_epoch_subject(),
                                                    theme_epoch_observer,
                                                    &theme_context);

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    struct sigaction sa{};
    sa.sa_handler = on_terminate;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    while (running && !g_terminate) {
        // Sleep until the next LVGL timer is due, clamped to 1..5 ms so input
        // latency stays as before while the idle loop no longer spins a core.
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms == LV_NO_TIMER_READY) {
            idle_ms = 5;
        }
        lv_delay_ms(std::clamp<uint32_t>(idle_ms, 1, 5));
    }

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }
    if (theme_observer) {
        lv_observer_remove(theme_observer);
    }
    if (accent_observer) {
        lv_observer_remove(accent_observer);
    }

    return 0;
}

} // namespace app
