/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"

namespace screen {

class AboutScreen : public BaseScreen {
public:
    AboutScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);

private:
    void build_content(lv_obj_t* content) override;
    void scroll(int32_t delta);

    static void scroll_up_cb(lv_event_t* event);
    static void scroll_down_cb(lv_event_t* event);
    static void back_cb(lv_event_t* event);

    lv_obj_t* scroll_{nullptr};
};

} // namespace screen
