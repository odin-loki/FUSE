// Internal: the .fusefont pack (COOKED text profile), written by fuse_text_cook (font_cook.cpp) and read by the COOKED
// runtime (text_cooked.cpp) without any font parsing. Little-endian, deterministic (sorted tables, no timestamps).
//
//   header (64 bytes): "FUSEFONT", u32 version, u32 sectionCount, i32 ascent, descent, lineGap (font units),
//                      f32 unitsPerPixelScale1, f32 emPerPixelSize, u16 unitsPerEm, u16 numGlyphs, 24 reserved bytes
//   section table:     sectionCount x {char tag[4], u32 offset, u32 size} (offsets from the file start)
//   "CMAP"  u32 n, n x {u32 codepoint, u32 glyph}, sorted by code point
//   "HMTX"  u32 n (= numGlyphs), n x u16 advance (font units)
//   "KERN"  u32 n, n x {u16 left, u16 right, i16 value, u16 0}, sorted by (left, right)
//   "COLR"  u32 nBase, nBase x {u32 glyph, u32 firstLayer, u32 count}; u32 nLayers, nLayers x {u32 glyph, u8 r, g, b, a,
//           u8 foreground, 3 x u8 0}: COLRv0 layers + COLRv1 graphs flattened (Font::colorLayers without colour)
//   "COVR"  coverage strikes, "RGBA" colour images, "MSDF" distance fields: u32 nStrikes, nStrikes x {u32 q (pixel
//           size * 4), f32 range (MSDF only, else 0), u32 n, u32 records, u32 data} (offsets from the section start),
//           each strike's n records {u32 glyph, i16 x0, i16 y0, u16 w, u16 h, u32 offset (from data)} sorted by glyph.
//           Texels: COVR 1 byte, RGBA / MSDF 4 bytes (premultiplied RGBA). An RGBA record with bit 31 of `glyph` set
//           depends on the text colour (COLRv1 foreground): two images follow, rendered with black and with white.
#pragma once

#include <cstdint>
#include <cstring>

namespace fuse::text::detail::fusefont {

inline constexpr char kMagic[8] = {'F', 'U', 'S', 'E', 'F', 'O', 'N', 'T'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kHeaderSize = 64;
inline constexpr std::uint32_t kForegroundBit = 0x80000000u;
inline constexpr std::uint32_t kStrikeHeader = 20;

inline std::uint32_t tag(const char (&t)[5]) {
    return std::uint32_t(std::uint8_t(t[0])) | (std::uint32_t(std::uint8_t(t[1])) << 8) |
           (std::uint32_t(std::uint8_t(t[2])) << 16) | (std::uint32_t(std::uint8_t(t[3])) << 24);
}

template <class T> inline T rd(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

struct GlyphRecord {
    std::uint32_t glyph;
    std::int16_t x0, y0;
    std::uint16_t w, h;
    std::uint32_t offset;
};
static_assert(sizeof(GlyphRecord) == 16);

inline bool isFuseFont(const std::uint8_t* d, std::size_t n) { return n >= kHeaderSize && std::memcmp(d, kMagic, 8) == 0; }

} // namespace fuse::text::detail::fusefont
