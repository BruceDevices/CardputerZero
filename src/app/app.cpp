/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "asset_manager.h"
#include "boot_anim.h"
#include "logger.h"
#include "linux_input.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "theme.h"
#include "screenshot_service.h"
#include "toast.h"

#include <memory>

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
    while (running) {
        lv_timer_handler();
        lv_delay_ms(5);
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
