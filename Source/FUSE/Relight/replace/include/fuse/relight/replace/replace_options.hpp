// FUSE Relight RL-3.4: the replacement engine's options (RL-0.6 registry; each also answers to its relight.* /
// rtx.* twin).
//
// Owned here:
//   rtx.enableReplacementAssets / Meshes / Materials / Lights   upstream names and defaults (true): the master
//                        switch and the per-kind switches of runtime replacements. Mods' rtx.conf layers may set
//                        them (plan §4.2: a mod's rtx.conf is an option layer).
//   relight.replace.*    discovery, hot reload, residency (below)
//   relight.modOrder     the stacking order by mod name (plan §4.2), strongest first
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

#include <cstdint>
#include <string>

namespace fuse::relight::replace {

struct ReplaceOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.replace", bool, enable, true, "FUSE_RELIGHT_REPLACE",
                            "Runtime replacements in capture mode (relight.tap.mode = capture): mods are discovered, "
                            "stacked and applied to every captured frame, and the replaced scene is written into the "
                            "capture record.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::string, modPaths, "fuse:fuse-relight/mods,remix:rtx-remix/mods",
                            "FUSE_RELIGHT_REPLACE_MOD_PATHS",
                            "Mod search roots (',' or ';' separated; each child directory is a mod). Prefix 'fuse:' "
                            "marks FUSE-native roots, 'remix:' (default) Remix roots. Relative to the working "
                            "directory.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::string, mods, "", "FUSE_RELIGHT_REPLACE_MODS",
                            "Additional single mod directories (same syntax as relight.replace.modPaths).");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::string, gameId, "", "FUSE_RELIGHT_REPLACE_GAME_ID",
                            "Remaster game id the mods are imported under; empty: the executable's name without "
                            "extension.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", bool, hotReload, true, "FUSE_RELIGHT_REPLACE_HOT_RELOAD",
                            "Watch the mods and reload a changed mod at the next frame boundary.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::string, watchBackend, "auto", "FUSE_RELIGHT_REPLACE_WATCH_BACKEND",
                            "Hot reload file notifications: auto (notify; poll under Wine), notify (inotify / "
                            "ReadDirectoryChangesW) or poll.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::uint32_t, pollIntervalMs, 250, "FUSE_RELIGHT_REPLACE_POLL_INTERVAL_MS",
                            "Poll backend: the shortest time between two scans of the mod directories (0: every "
                            "frame).");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::uint32_t, textureBudgetMiB, 1024, "FUSE_RELIGHT_REPLACE_TEXTURE_BUDGET_MIB",
                            "Replacement texture residency budget in MiB (0: unlimited).");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", bool, preloadTextures, false, "FUSE_RELIGHT_REPLACE_PRELOAD_TEXTURES",
                            "Request every replacement texture as soon as its mod loads (materials can ask for it "
                            "with preload_textures).");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::uint32_t, textureMipBias, 0, "FUSE_RELIGHT_REPLACE_TEXTURE_MIP_BIAS",
                            "Top mip levels of every replacement texture that are never made resident (e.g. 1 when "
                            "rendering at half resolution and upscaling).");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::uint32_t, debugReloadWaitFrame, 0,
                            "FUSE_RELIGHT_REPLACE_DEBUG_RELOAD_WAIT_FRAME",
                            "Test hook (0: off). At the start of this frame the engine writes the marker file "
                            "relight.replace.debugReloadMarker and waits (at most debugReloadWaitMs) for a file "
                            "notification from the mods, so a test can edit a mod at a known frame.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::string, debugReloadMarker, "relight_replace_wait",
                            "FUSE_RELIGHT_REPLACE_DEBUG_RELOAD_MARKER", "Marker file of debugReloadWaitFrame.");
    FUSE_RELIGHT_OPTION_ENV("relight.replace", std::uint32_t, debugReloadWaitMs, 20000,
                            "FUSE_RELIGHT_REPLACE_DEBUG_RELOAD_WAIT_MS", "Longest wait of debugReloadWaitFrame.");
    FUSE_RELIGHT_OPTION_ENV("relight", std::string, modOrder, "", "FUSE_RELIGHT_MOD_ORDER",
                            "Mod stacking order by mod name, strongest first (',' separated). Listed mods sit above "
                            "unlisted ones; the rest order by relight.mod.priority in their own rtx.conf, "
                            "FUSE-native above Remix, then name.");
    FUSE_RELIGHT_OPTION("rtx", bool, enableReplacementAssets, true,
                        "Globally enables or disables all enhanced asset replacement (materials, meshes, lights).");
    FUSE_RELIGHT_OPTION("rtx", bool, enableReplacementLights, true, "Enables or disables light replacements.");
    FUSE_RELIGHT_OPTION("rtx", bool, enableReplacementMeshes, true, "Enables or disables mesh replacements.");
    FUSE_RELIGHT_OPTION("rtx", bool, enableReplacementMaterials, true, "Enables or disables material replacements.");
};

/// References every option above (static libraries: keeps the registrations linked).
void registerReplaceOptions();

} // namespace fuse::relight::replace
