// Internal glue between the Tier A and Tier B widget sources.
#pragma once

#include "fuse/ui/ui.hpp"

namespace fuse::ui::widgets::detail {

/// Opens the combo box's popup list (Tier B). False when the Tier B classes are not registered (Tier A cycles).
bool openComboPopup(UiContext& ui, NodeHandle combo);

} // namespace fuse::ui::widgets::detail
