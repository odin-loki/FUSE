// FUSE Relight RL-4.1: frame orchestration options (RL-0.6 registry; each also answers to its rtx.* twin).
//
//   relight.frame.mode          off | passthrough | solid | raster | pathtrace (env FUSE_RELIGHT_FRAME_MODE, default off).
//                               Needs relight.tap.mode = capture (the classifier finds the injection point).
//                               passthrough: FUSE's image of the frame is the back buffer as DXVK rendered it up
//                               to the injection point, composited back (bit-identical to the tap being off);
//                               solid: FUSE's image is relight.frame.solidColor (the UI still draws on top);
//                               raster: RL-4.2's raster remaster of the captured scene (relight.raster.*), on the
//                               renderer adopted from the host's device; passthrough when it cannot render.
//   relight.frame.solidColor    RRGGBB hex of the solid mode (env FUSE_RELIGHT_FRAME_SOLID_COLOR).
//   relight.frame.textureSwap   every sampled game texture is replaced by a FUSE-owned twin that DXVK keeps in
//                               sync with the game's content (passthrough texture swap; env
//                               FUSE_RELIGHT_FRAME_TEXTURE_SWAP, default false).
//   relight.frame.injectAtUi    inject at the first UI draw (Remix semantics) when the classifier finds one;
//                               false: always at Present (env FUSE_RELIGHT_FRAME_INJECT_AT_UI, default true).
//   relight.frame.statsPath     JSON Lines per frame (injection point, timeline values, swaps, bindless and GPU
//                               scene counts); empty: none (env FUSE_RELIGHT_FRAME_STATS).
//   relight.frame.dumpPath      tests: FUSE's composited image (the output image, before the UI) of the last injected
//                               frame as raw RGBA8 rows (top-down); empty: none (env FUSE_RELIGHT_FRAME_DUMP). A game
//                               that reads its back buffer before Present never sees an injection at Present.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace fuse::relight::render::frame {

enum class FrameMode : std::uint8_t { Off = 0, Passthrough, Solid, Raster, PathTrace };

/// "off" / "passthrough" / "solid" / "raster" / "pathtrace" (case-insensitive). False (out unchanged) for anything else.
bool parseFrameMode(std::string_view text, FrameMode& out);
const char* frameModeName(FrameMode mode);
/// "RRGGBB" (optionally "#" or "0x" prefixed) -> 0xRRGGBB. False for anything else.
bool parseRgbHex(std::string_view text, std::uint32_t& out);

struct FrameOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.frame", std::string, mode, "off", "FUSE_RELIGHT_FRAME_MODE",
                            "RL-4.1 frame orchestration: off, passthrough (FUSE's image is the back buffer at the "
                            "injection point, composited back), solid (FUSE's image is relight.frame.solidColor) or "
                            "raster (RL-4.2 raster remaster of the captured scene). Needs relight.tap.mode = capture.");
    FUSE_RELIGHT_OPTION_ENV("relight.frame", std::string, solidColor, "2050d0", "FUSE_RELIGHT_FRAME_SOLID_COLOR",
                            "RRGGBB colour of relight.frame.mode = solid.");
    FUSE_RELIGHT_OPTION_ENV("relight.frame", bool, textureSwap, false, "FUSE_RELIGHT_FRAME_TEXTURE_SWAP",
                            "Passthrough texture swap: DXVK samples a FUSE-owned twin of every game texture, kept in "
                            "sync with the game's content.");
    FUSE_RELIGHT_OPTION_ENV("relight.frame", bool, injectAtUi, true, "FUSE_RELIGHT_FRAME_INJECT_AT_UI",
                            "Inject at the first UI draw of the frame (Remix semantics); false: at Present.");
    FUSE_RELIGHT_OPTION_ENV("relight.frame", std::string, statsPath, "", "FUSE_RELIGHT_FRAME_STATS",
                            "JSON Lines file with one record per frame (empty: none).");
    FUSE_RELIGHT_OPTION_ENV("relight.frame", std::string, dumpPath, "", "FUSE_RELIGHT_FRAME_DUMP",
                            "Raw RGBA8 dump of FUSE's output image of the last injected frame (tests; empty: none).");
};

/// References every option above (static libraries: keeps the registrations linked).
void registerFrameOptions();

/// The options above, resolved now.
struct FrameConfig {
    FrameMode mode = FrameMode::Off;
    std::uint32_t solidColor = 0x2050d0; ///< 0xRRGGBB
    bool textureSwap = false;
    bool injectAtUi = true;
    std::string statsPath;
    std::string dumpPath;

    bool enabled() const { return mode != FrameMode::Off || textureSwap; }
    static FrameConfig fromOptions();
};

} // namespace fuse::relight::render::frame
