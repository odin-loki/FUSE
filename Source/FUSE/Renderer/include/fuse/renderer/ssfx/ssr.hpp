#pragma once

// Forwarding header — see <fuse/ssfx/ssr.hpp> (shared `fuse_ssfx` library).
#include <fuse/renderer/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssr.hpp>

namespace fuse::renderer {

using ::fuse::ssfx::SsrHit;
using ::fuse::ssfx::SsrParams;
using ::fuse::ssfx::clampSsrParams;
using ::fuse::ssfx::computeSsrCpu;
using ::fuse::ssfx::ssrReflect;
using ::fuse::ssfx::ssrTracePixel;
using ::fuse::ssfx::ssrTraceRay;

} // namespace fuse::renderer
