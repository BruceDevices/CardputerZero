#pragma once

#include "lvgl.h"

#include <cstddef>

namespace platform {

constexpr size_t kNavKeyCount = 5;

void init_key_input(lv_display_t* display);
void attach_key_router(lv_indev_t* indev);
void register_nav_button(size_t index, lv_obj_t* button);
void unregister_nav_button(size_t index, lv_obj_t* button);

// While a text-entry dialog is open (sudo password), letter/symbol/arrow keys
// must type into the focused textarea instead of firing nav actions; only ESC
// (cancel) and Enter (submit) keep their nav meaning. On device this also
// enables the full evdev character map (letters, digits, shift, backspace);
// desktop SDL keyboards already deliver real characters.
void set_text_input_mode(bool enabled);
bool text_input_mode();

} // namespace platform
