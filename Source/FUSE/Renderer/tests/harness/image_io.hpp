// WP-0.7 renderer test harness: minimal, dependency-free image encoders / decoders.
//
//   PNG  8-bit greyscale / RGB / RGBA, non-interlaced. The encoder writes RGB when every alpha is
//        255 (else RGBA), picks the per-row filter by the minimum-sum-of-absolute-differences
//        heuristic and deflates with LZ77 (hash chains) + fixed Huffman codes. The decoder is a
//        full inflate (stored, fixed and dynamic blocks) with all five PNG filters, so goldens
//        re-saved by other tools (optipng, GIMP, Python) still load.
//   EXR  scanline, NO_COMPRESSION, FLOAT channels (written) and FLOAT / HALF (read). Used for
//        linear / depth debug artefacts, never for goldens (see Tests/golden/renderer/README.md).
//
// No Vulkan types: this file builds in the stub backend and is exercised by the CPU-only
// `rp_harness_image_io` ctest.
#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::harness {

/// Tightly packed 8-bit RGBA image, row 0 at the top.
struct ImageRgba8 {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels; ///< width * height * 4

    ImageRgba8() = default;
    ImageRgba8(u32 w, u32 h) : width(w), height(h), pixels(static_cast<usize>(w) * h * 4u, 0u) {}

    bool valid() const { return width != 0u && height != 0u && pixels.size() == static_cast<usize>(width) * height * 4u; }
    u8* at(u32 x, u32 y) { return pixels.data() + (static_cast<usize>(y) * width + x) * 4u; }
    const u8* at(u32 x, u32 y) const { return pixels.data() + (static_cast<usize>(y) * width + x) * 4u; }
};

/// Encodes `image` as PNG bytes. Returns false for an invalid image.
bool encodePng(const ImageRgba8& image, std::vector<u8>& out);
/// Decodes PNG bytes into RGBA8 (grey / RGB are expanded, alpha 255). `error` names the failure.
bool decodePng(const u8* data, usize size, ImageRgba8& out, std::string* error = nullptr);

bool writePng(const std::string& path, const ImageRgba8& image);
bool readPng(const std::string& path, ImageRgba8& out, std::string* error = nullptr);

/// One planar float channel of an EXR image (`data` holds width * height values, row 0 at the top).
struct ExrChannel {
    std::string name;
    std::vector<f32> data;
};

/// Writes an uncompressed scanline OpenEXR file with FLOAT channels (sorted by name on write, as
/// the format requires). Returns false on an invalid layout or an I/O failure.
bool writeExr(const std::string& path, u32 width, u32 height, std::vector<ExrChannel> channels);
/// Reads an uncompressed scanline OpenEXR file (FLOAT or HALF channels, converted to f32).
bool readExr(const std::string& path, u32& width, u32& height, std::vector<ExrChannel>& channels,
             std::string* error = nullptr);

bool readFileBytes(const std::string& path, std::vector<u8>& out);
bool writeFileBytes(const std::string& path, const std::vector<u8>& bytes);
/// Creates `path` and its parents (no-op when it exists).
bool ensureDirectory(const std::string& path);

u32 crc32(const u8* data, usize size, u32 crc = 0u);

} // namespace fuse::renderer::harness
