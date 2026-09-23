#pragma once

// Forwarding header — see <fuse/ssfx/ssgi.hpp> (shared `fuse_ssfx` library).
#include <fuse/renderer/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi.hpp>

namespace fuse::renderer {

using ssfx::SsgiParams;
using ssfx::clampSsgiParams;
using ssfx::computeSsgiCpu;
using ssfx::ssgiPixelGather;

} // namespace fuse::renderer
