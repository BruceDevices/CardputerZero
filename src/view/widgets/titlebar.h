/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_widget.h"
#include "base_viewmodel.h"

namespace view::widgets {

class TitleBar : public BaseWidgets {
public:
    TitleBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model);

    void build() override;

private:
    viewmodel::BaseViewModel& view_model_;
};

} // namespace view::widgets
