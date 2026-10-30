// FUSE UI GPU backend, CPU side (no Vulkan types): packs a DrawList into the buffers the "ui.draw_list" kernel reads
// (src/shaders/ui_draw_list.comp). Clip stacks are resolved per command; commands are binned into 16x16 tiles in draw
// order. Every vector keeps its capacity across frames, so packing a frame of the same size does not allocate.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <vector>

namespace fuse::ui::render {

/// One command (std430, 160 bytes) — must match `GpuCmd` in ui_draw_list.comp.
struct GpuCmd {
    f32 rect[4];
    f32 fill[4];
    f32 fill2[4];
    f32 border[4];
    f32 radius[4];
    f32 clip[4];
    f32 params[4]; ///< borderWidth, blur, clipRadius, 0
    u32 info[4];   ///< op | gradient << 8, u | v << 16, tw | th << 16, image
    f32 grad[4];   ///< multi-stop gradients: pixel -> gradient space a, b, c, d
    f32 grad2[4];  ///< e, f, stop word offset in `images` (u32 bits), stop count (u32 bits)
};
static_assert(sizeof(GpuCmd) == 160, "GpuCmd must match the kernel's std430 layout");

/// Push constants of ui.draw_list (must match `UiPush`). One dispatch covers origin .. origin + size (the damage
/// region; the whole target for a full frame) and reads the tile table starting at `tileBase`.
struct GpuPush {
    u32 extent[2];
    u32 tilesX;
    u32 atlasSize;
    u32 tileCount;
    u32 imageCount;
    u32 origin[2];
    u32 size[2];
    u32 tileBase;
    u32 flags; ///< kPassClear: start from opaque black instead of the target's contents (damage clear)
};
static_assert(sizeof(GpuPush) == 48, "GpuPush must match the kernel's push block");

inline constexpr u32 kUiTile = 16;
/// Tile entries with this bit set address a compact glyph record (word offset into GpuDrawData::images; u16 values
/// biased by 32768): origin x|y, u|v, tw | th << 16 | flags, word offset of the fill (4 f32), clipped pixel box
/// x0|y0 and x1|y1 (exclusive; read only when kGlyphBoxed), clip-carrier cmd index (only when kGlyphRounded).
inline constexpr u32 kCompactGlyph = 0x80000000u;
inline constexpr u32 kCompactGlyphWords = 7;
inline constexpr u32 kGlyphBoxed = 1u << 30;   ///< record flag: test the pixel box (clipped or fractional glyph)
inline constexpr u32 kGlyphRounded = 1u << 31; ///< record flag: rounded clip, coverage from the carrier cmd
/// Tile entries with both top bits set address a glyph run: n, y0|y1 (union), 16 per-column candidate ranges
/// first|end<<16 (glyphs whose box can contain that tile column), n record offsets (paint order). Built from >= kMinGlyphRun consecutive compact glyphs of one tile.
inline constexpr u32 kGlyphRun = 0xC0000000u;
inline constexpr u32 kMinGlyphRun = 3;
/// GpuCmd op of a clip carrier: holds clip rect / radius for compact glyphs; never binned.
inline constexpr u32 kClipCarrier = 9;
inline constexpr u32 kPassClear = 1u;

/// The frame is a sequence of passes: tile-binned draw segments separated by backdrop blurs (each blur reads what
/// the previous segments drew). kind 0: draw (GpuPush.tileBase = tileBase); kind 1: backdrop blur of cmds[cmd] over
/// the pixel box `area` (x0, y0, x1, y1), sampling `src` (x, y, w, h), box radius `radius`, 3 box passes per axis.
struct GpuPass {
    u32 kind = 0;
    u32 tileBase = 0;
    u32 cmd = 0;
    u32 flags = 0;
    i32 src[4] = {};
    i32 area[4] = {};
    u32 radius = 0;
    f32 inv = 0.f;
};
inline constexpr u32 kPassDraw = 0, kPassBackdrop = 1;

struct GpuDrawData {
    std::vector<GpuCmd> cmds;
    std::vector<u32> tiles;  ///< per draw pass at its tileBase: offsets[tileCount + 1], then command ids
    std::vector<GpuPass> passes; ///< in order; passes[0] is always a draw pass
    std::vector<u32> images; ///< per image: offset (in u32 pixels into `pixels`), width, height, 0; then pixels
    GpuPush push{};
    const u8* atlas = nullptr; ///< glyph coverage atlas (atlasSize^2 bytes), uploaded as-is
    u32 atlasSize = 0;
    /// fuse::text::GlyphAtlas::version() of `atlas`; the renderer re-uploads the atlas only when it changes.
    /// ~0u (packDrawList's default) = unknown, upload every frame.
    u32 atlasGeneration = ~0u;

    /// packDrawList scratch: per-glyph ink boxes (open-addressed by atlas rect, rebuilt every pack; kept for capacity)
    std::vector<u64> inkKeys;
    std::vector<u32> inkBoxes;

    std::size_t cmdBytes() const { return cmds.size() * sizeof(GpuCmd); }
    std::size_t tileBytes() const { return tiles.size() * 4; }
    std::size_t imageBytes() const { return images.size() * 4; }
    std::size_t atlasBytes() const { return std::size_t(atlasSize) * atlasSize; }
};

/// Packs `list` for a width x height target, restricted to `scissor` (whole target when empty). `images` are the
/// rasteriser image views (same indices as DrawCmd::image); `atlas` / `atlasSize` the glyph atlas. The dispatch
/// region (push.origin / size) is the scissor's pixel box, so a damage scissor repaints only the damaged pixels.
void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas = nullptr,
                  u32 atlasSize = 0, const Image* const* images = nullptr, u32 imageCount = 0, Rect scissor = {});

} // namespace fuse::ui::render
