// FUSE Relight RL-4.1: the capture tap factory's hook (tap/capture createTapForDevice). Declaration only, so
// the capture tap does not see the renderer headers.
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

#include <memory>

namespace fuse::relight::tap {
class CaptureTap;
}

namespace fuse::relight::render::frame {

/// `capture` unchanged when relight.frame.* asks for nothing (frame_options.hpp), else a RenderTap around it
/// (render_tap.hpp).
std::unique_ptr<tap::IRelightTap> attachFrameTap(std::unique_ptr<tap::CaptureTap> capture, unsigned deviceOrdinal);

} // namespace fuse::relight::render::frame
