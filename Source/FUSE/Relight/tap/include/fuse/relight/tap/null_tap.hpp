// FUSE Relight RL-1.1: the null tap. Receives every event and ignores it (IRelightTap's defaults);
// every draw stays a DXVK raster draw. relight.tap.mode = null. It exercises the whole dispatch
// path (id tracking, state snapshots) without consuming anything, which is what the passthrough
// goldens compare against the tap-off build.
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

namespace fuse::relight::tap {

class NullTap final : public IRelightTap {};

} // namespace fuse::relight::tap
