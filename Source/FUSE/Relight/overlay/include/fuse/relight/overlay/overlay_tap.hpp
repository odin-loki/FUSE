// FUSE Relight RL-6.1: the developer overlay inside the tap (RenderTap's IFrameOverlay, render_tap.hpp).
//
//   attach    subclasses the device window's WndProc (win32_hook.hpp) and maps client to back-buffer pixels;
//   onFrame   (each flushed frame, under the capture tap's lock) feeds a running RL-1.8 capture (Capture button:
//             LiveCaptureExport over the next relight.overlay.captureFrames frames) and, while shown, lists the
//             frame's sampled textures (image hash, size, uses) for the Textures tab;
//   present   (after RenderTap's frame record, before DXVK presents) runs scripted input, drains the window events
//             (OverlayCore: toggles; while hidden nothing else happens and nothing is allocated), and while shown
//             runs the menu, rasterises the layer and draws it with FrameOrchestrator::postComposite (overlay_gpu.hpp)
//             over the finished frame, UI included; a debug view shows the frame renderer's DebugImage full screen.
// The GPU side needs the FUSE renderer adopted on the device (RL-4.1): without it the menu runs but nothing is drawn
// (the stats record says why).
#pragma once

#include <fuse/relight/render/frame/render_tap.hpp>

#include <memory>

namespace fuse::relight::overlay {

/// The overlay for a RenderTap; null when relight.overlay.enable is off.
std::unique_ptr<render::frame::IFrameOverlay> createFrameOverlay();

} // namespace fuse::relight::overlay
