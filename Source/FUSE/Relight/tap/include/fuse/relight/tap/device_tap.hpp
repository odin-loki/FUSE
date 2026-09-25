// FUSE Relight RL-1.1: the tap the d3d9 dispatcher attaches to a device, for every relight.tap.mode.
//
// createTap (tap_config.hpp) builds the modes that live in fuse_relight_tap itself (null, record).
// The capture mode (TapMode::Capture) runs the RL-1.2 classifier, RL-1.3 geometry capture and RL-1.4
// texture tracking in-process (CaptureTap, capture_tap.hpp); those packages depend on
// fuse_relight_tap, so the factory that knows every mode is defined by fuse_relight_tap_capture
// (Source/FUSE/Relight/tap/capture). The DXVK dispatcher (dxvk/fuse_tap_dxvk.cpp) calls this, once
// per device, at the first ResetSwapChain; with the tap off it returns nullptr and nothing else of
// Relight runs.
//
// Plain C++17 (the dispatcher is compiled with DXVK's C++17 settings).
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>
#include <fuse/relight/tap/tap_config.hpp>

#include <memory>

namespace fuse::relight::tap {

/// createTap(config, deviceOrdinal), plus TapMode::Capture: a CaptureTap writing
/// devicePath(config.capturePath, deviceOrdinal), which also forwards every event to a RecordingTap
/// writing devicePath(config.recordPath, deviceOrdinal) when config.captureRecord is set.
std::unique_ptr<IRelightTap> createTapForDevice(const RuntimeConfig& config, unsigned deviceOrdinal);

} // namespace fuse::relight::tap
