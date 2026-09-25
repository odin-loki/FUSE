#pragma once

// Forwarding header — see <fuse/ssfx/hbao.hpp> (shared `fuse_ssfx` library).
#include <fuse/renderer/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/hbao.hpp>

namespace fuse::renderer {

using ssfx::HbaoParams;
using ssfx::clampHbaoParams;
using ssfx::computeHbaoCpu;
using ssfx::hbaoPixelVisibility;
using ssfx::ssaoHemisphereReferenceVisibility;

} // namespace fuse::renderer
