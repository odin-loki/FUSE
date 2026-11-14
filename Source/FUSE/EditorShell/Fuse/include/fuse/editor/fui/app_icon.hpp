// FUSE application icon for the editor shell (Appendix A "Icons"; replaced the former Qt shell's app_icon).
// The Source/FUSE/Branding rasters are embedded at build time; the Windows executable icon comes from
// Branding/fuse_editor.rc (fuse.ico), linked into fuse_editor_fuse on WIN32.
#pragma once

#include "fuse/app/app.hpp"

#include <array>
#include <vector>

namespace fuse::editor::fui {

/// Same sizes as the former Qt shell's kAppIconSizes.
inline constexpr std::array<int, 4> kAppIconSizes{16, 32, 48, 256};

/// Decoded RGBA rasters, one per kAppIconSizes entry (empty on a decode failure).
std::vector<fuse::ui::Image> applicationIcon();

} // namespace fuse::editor::fui
