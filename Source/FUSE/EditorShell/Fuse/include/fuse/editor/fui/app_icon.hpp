// FUSE application icon for the FUSE UI editor shell: twin of Editor/src/qt/app_icon.{hpp,cpp} (Appendix A "Icons").
// The Source/FUSE/Branding rasters are embedded at build time; the Windows executable icon comes from
// Branding/fuse_editor.rc (fuse.ico), linked into fuse_editor_fuse on WIN32.
#pragma once

#include "fuse/app/app.hpp"

#include <array>
#include <vector>

namespace fuse::editor::fui {

/// Same sizes as the Qt shell's kAppIconSizes.
inline constexpr std::array<int, 4> kAppIconSizes{16, 32, 48, 256};

/// Decoded RGBA rasters, one per kAppIconSizes entry (empty on a decode failure).
std::vector<fuse::ui::Image> applicationIcon();

} // namespace fuse::editor::fui
