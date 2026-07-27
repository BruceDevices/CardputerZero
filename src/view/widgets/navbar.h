/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_widget.h"
#include "base_viewmodel.h"
#include "icon_button.h"

#include <array>
#include <memory>

namespace app {
class AssetManager;
}

namespace view::widgets {

// A row of 5 generic buttons mapped 1:1 to the hardware/keyboard nav keys.
// NavBar itself has no notion of "pages" - each screen configures its own 5
// slots via set_icon()/set_enabled() right after building its content, which
// keeps this widget reusable across every screen instead of growing a
// per-page switch statement.
class NavBar : public BaseWidgets {
public:
    NavBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~NavBar() override;

    void build() override;

    // index is 0..4, left to right, matching hardware keys 4..8.
    void set_icon(size_t index, const char* text, lv_event_cb_t click_cb, void* user_data);
    void set_text(size_t index, const char* text);
    void set_enabled(size_t index, bool enabled);

private:
    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    std::array<std::unique_ptr<IconButton>, 5> icon_buttons_;
};

} // namespace view::widgets
