// FUSE Relight RL-1.4: the rtx.conf options of texture hashing (RL-0.6 options system).
//
//   rtx.useObsoleteHashOnTextureUpload   (relight.* twin)  XXH64 instead of XXH3 on upload
//   rtx.recomputeTextureHashOnWrite      (relight.* twin)  clear the hash on a mip-0 write lock
//
// The texture lists that keep a hash on a write lock (rtx.terrainTextures, rtx.lightmapTextures,
// rtx.ignoreTextures, rtx.ignoreBakedLightingTextures) belong to the classifier / material
// packages; they are looked up by name at each write lock, and count as empty while nobody
// declares them.
#pragma once

#include <fuse/relight/capture/texture/texture_tracker.hpp>

#include <array>
#include <string_view>

namespace fuse::relight::capture::texture {

inline constexpr std::array<std::string_view, 4> kKeepHashOnWriteLists = {
    "rtx.terrainTextures", "rtx.lightmapTextures", "rtx.ignoreTextures", "rtx.ignoreBakedLightingTextures"};

/// True when `hash` is in one of kKeepHashOnWriteLists (declared hash-set options only).
bool isHashKeptOnWrite(hash::Hash64 hash);

/// A tracker configuration from the resolved options (the option system must be initialised, or
/// the defaults are used). keepHashOnWrite is isHashKeptOnWrite.
TextureTrackerConfig textureTrackerConfigFromOptions();

} // namespace fuse::relight::capture::texture
