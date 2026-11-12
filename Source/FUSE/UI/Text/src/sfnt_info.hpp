// Internal: face description read straight from the sfnt tables (name, OS/2, head, post) through a random-access
// reader, so the system font scan touches a few hundred bytes per file instead of loading it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fuse::text::detail {

struct SfntReader {
    void* user = nullptr;
    bool (*read)(void* user, std::uint64_t offset, void* dst, std::size_t n) = nullptr; ///< false: out of range
};

struct FaceInfo {
    std::string family, style;
    int weight = 400;
    bool italic = false, monospace = false;
    bool outlines = false; ///< has glyf or CFF outlines (what the rasterisers can draw)
    bool hasPost = false, hasStrike = false;
    int underlinePosition = 0, underlineThickness = 0; ///< font units, y up
    int strikePosition = 0, strikeSize = 0;
};

/// Number of faces in the file: 1 for .ttf / .otf, n for a .ttc collection, 0 when it is not an sfnt font.
int sfntFaceCount(const SfntReader& r);
/// Reads the description of face `index`. False when the file is malformed.
bool sfntFaceInfo(const SfntReader& r, int index, FaceInfo& out);

} // namespace fuse::text::detail
