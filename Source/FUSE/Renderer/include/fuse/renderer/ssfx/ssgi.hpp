#pragma once

// Forwarding header — see <fuse/ssfx/ssgi.hpp> (shared `fuse_ssfx` library).
#include <fuse/renderer/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi.hpp>

namespace fuse::renderer {

using ::fuse::ssfx::SsgiParams;
using ::fuse::ssfx::clampSsgiParams;
using ::fuse::ssfx::computeSsgiCpu;
using ::fuse::ssfx::ssgiPixelGather;

} // namespace fuse::renderer
