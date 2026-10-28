#pragma once

// Forwarding header: the Qt-/Vulkan-free screen-space effect references live in the shared `fuse_ssfx`
// library (Source/FUSE/ScreenSpace) so that fuse_compute can use them without depending on fuse_rhi.
#include <fuse/ssfx/ssfx_view.hpp>

namespace fuse::renderer {

using ::fuse::ssfx::SsfxCamera;
using ::fuse::ssfx::SsfxGBufferView;
using ::fuse::ssfx::ssfxReconstructNormal;

} // namespace fuse::renderer
