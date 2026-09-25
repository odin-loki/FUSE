// FUSE Relight RL-6.1: the in-game developer overlay's options (RL-0.6 registry; each also answers to its rtx.* twin).
//
//   relight.overlay.enable         the overlay is available (Alt+X toggles it) whenever the frame orchestration runs
//                                  (relight.frame.mode != off or relight.frame.textureSwap): the tap subclasses the
//                                  device window's WndProc to read input (env FUSE_RELIGHT_OVERLAY, default true).
//   relight.overlay.startVisible   shown from the first frame (env FUSE_RELIGHT_OVERLAY_VISIBLE, default false).
//   relight.overlay.scale          pixel scale of the 5x7 font and every widget; 0: 1 below 720 lines, 2 below 1440,
//                                  else 3 (env FUSE_RELIGHT_OVERLAY_SCALE).
//   relight.overlay.debugView      the full-screen debug view (DebugView values; runtime only, never saved).
//   relight.overlay.captureFrames  frames the Capture button records (RL-1.8 LiveCaptureExport; default 1).
//   relight.overlay.captureDir     where it writes them (env FUSE_RELIGHT_OVERLAY_CAPTURE_DIR, default
//                                  relight_capture_overlay; <dir>_1, _2 ... when it exists and is not empty).
//   relight.overlay.deterministic  wall-clock values (frame time, fps) print as "--": tests, goldens
//                                  (env FUSE_RELIGHT_OVERLAY_DETERMINISTIC).
//   relight.overlay.script         tests / automation: a scripted input sequence (script.hpp) sent through the
//                                  window hook (env FUSE_RELIGHT_OVERLAY_SCRIPT; empty: none).
//   relight.overlay.statsPath      tests: JSON Lines, one record per frame (visibility, consumed / forwarded window
//                                  messages, overlay rounds, tab, panel rectangle) (env FUSE_RELIGHT_OVERLAY_STATS).
//   relight.overlay.dumpPath       tests: the back buffer after the overlay of the last shown frame as raw RGBA8 rows,
//                                  top-down, with a 16-byte header "RLOV" w h 0 (env FUSE_RELIGHT_OVERLAY_DUMP).
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

#include <cstdint>
#include <string>

namespace fuse::relight::overlay {

struct OverlayOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", bool, enable, true, "FUSE_RELIGHT_OVERLAY",
                            "RL-6.1 developer overlay: available whenever the frame orchestration runs; Alt+X toggles "
                            "it (the device window's WndProc is subclassed to read input).");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", bool, startVisible, false, "FUSE_RELIGHT_OVERLAY_VISIBLE",
                            "Show the developer overlay from the first frame.");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", std::int32_t, scale, 0, "FUSE_RELIGHT_OVERLAY_SCALE",
                            "Pixel scale of the developer overlay (0: from the back-buffer height).");
    FUSE_RELIGHT_OPTION_FLAG("relight.overlay", std::int32_t, debugView, 0, options::OptionFlags::NoSave,
                             "Full-screen debug view of the developer overlay: 0 off, 1 albedo, 2 normals, 3 depth, "
                             "4 motion, 5 demodulated diffuse, 6 demodulated specular (what the frame renderer "
                             "offers).");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", std::int32_t, captureFrames, 1, "FUSE_RELIGHT_OVERLAY_CAPTURE_FRAMES",
                            "Frames the developer overlay's Capture button records (RL-1.8 capture).");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", std::string, captureDir, "relight_capture_overlay",
                            "FUSE_RELIGHT_OVERLAY_CAPTURE_DIR", "Directory of the developer overlay's captures.");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", bool, deterministic, false, "FUSE_RELIGHT_OVERLAY_DETERMINISTIC",
                            "Print wall-clock values of the developer overlay as '--' (tests).");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", std::string, script, "", "FUSE_RELIGHT_OVERLAY_SCRIPT",
                            "Scripted input sequence for the developer overlay (tests / automation).");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", std::string, statsPath, "", "FUSE_RELIGHT_OVERLAY_STATS",
                            "JSON Lines record of the developer overlay per frame (tests; empty: none).");
    FUSE_RELIGHT_OPTION_ENV("relight.overlay", std::string, dumpPath, "", "FUSE_RELIGHT_OVERLAY_DUMP",
                            "Raw dump of the back buffer after the developer overlay (tests; empty: none).");
};

/// References every option above (static libraries: keeps the registrations linked).
void registerOverlayOptions();

/// The options above, resolved now.
struct OverlayConfig {
    bool enable = true;
    bool startVisible = false;
    bool deterministic = false;
    std::int32_t scale = 0;
    std::uint32_t captureFrames = 1;
    std::string captureDir = "relight_capture_overlay";
    std::string script;
    std::string statsPath;
    std::string dumpPath;

    static OverlayConfig fromOptions();
};

} // namespace fuse::relight::overlay
