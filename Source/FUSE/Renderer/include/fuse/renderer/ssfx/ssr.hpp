#pragma once

// Forwarding header — see <fuse/ssfx/ssr.hpp> (shared `fuse_ssfx` library).
#include <fuse/renderer/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssr.hpp>

namespace fuse::renderer {

using ssfx::SsrHit;
using ssfx::SsrParams;
using ssfx::clampSsrParams;
using ssfx::computeSsrCpu;
using ssfx::ssrReflect;
using ssfx::ssrTracePixel;
using ssfx::ssrTraceRay;

} // namespace fuse::renderer
