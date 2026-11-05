// PNG decode / encode shared by the UI libraries (fuse_text's bitmap colour glyphs, fuse_app's clipboard and
// screenshots): one stb_image / stb_image_write instance per binary instead of one per user. Each function sits in its
// own section, so a binary links only the ones it calls.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fuse::text::png {

/// Width and height of a PNG without decoding it. False when `bytes` is not a PNG stb_image can read.
bool info(std::span<const std::uint8_t> bytes, int& w, int& h);
/// Decodes to 8-bit RGBA (straight alpha, w * h * 4 bytes). Fails for images wider or taller than `maxDim`.
bool decodeRgba(std::span<const std::uint8_t> bytes, int maxDim, int& w, int& h, std::vector<std::uint8_t>& rgba);
/// Encodes 8-bit RGBA rows (`stride` bytes apart) as a PNG, appended to `out`.
bool encodeRgba(const std::uint8_t* rgba, int w, int h, int stride, std::vector<std::uint8_t>& out);
/// encodeRgba to a file (UTF-8 path).
bool writeRgba(const std::string& path, const std::uint8_t* rgba, int w, int h, int stride);

} // namespace fuse::text::png
