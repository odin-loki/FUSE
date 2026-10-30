// FUSE UI GPU backend, CPU side (no Vulkan types): packs a DrawList into the buffers the "ui.draw_list" kernel reads
// (src/shaders/ui_draw_list.comp). Clip stacks are resolved per command; commands are binned into 16x16 tiles in draw
// order. Every vector keeps its capacity across frames, so packing a frame of the same size does not allocate.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <cstring>
#include <unordered_map>
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
/// Image table word 3: 0 = RGBA8 pixels, 1..4 = external slot + 1, kImageHalf = RGBA16F pixels (2 words each).
inline constexpr u32 kImageHalf = 0x100u;

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
    /// backdrop / layer passes: flags bit 0 colour matrix, bit 1 drop shadow (box radius `radius2`, 1 / (2 r2 + 1) `inv2`)
    u32 radius2 = 0;
    f32 inv2 = 0.f;
    /// layer passes: scratch floats of the enclosing layers' saved pixels plus this one's (w * h * 4), counted from
    /// the scratch end; backdrop passes: the enclosing layers' total. The renderer skips a backdrop, or both passes
    /// of a layer, whose work floats do not fit below it.
    u32 saveTop = 0;
    /// filter passes: flags bit 2 = SVG filter program (`ops` instructions, bit i of `blurMask` = op i blurs);
    /// `planes` float planes of w * h * 4 work; `margin` rows a band of the layer needs around it (tiling)
    u32 ops = 0, blurMask = 0, planes = 2, margin = 0;
};
/// kPassLayerBegin: save the pixels under a PushFilter's box to scratch and clear it; kPassLayerEnd: the PopFilter
/// composite (src = area = the layer box)
inline constexpr u32 kPassDraw = 0, kPassBackdrop = 1, kPassLayerBegin = 2, kPassLayerEnd = 3;

struct GpuDrawData {
    std::vector<GpuCmd> cmds;
    /// per draw pass at its tileBase: begin | end per tile (2 words, absolute indices into `tiles`); after all pass
    /// headers, one slot per (pass, tile) holding its entries then its glyph run records. Slots keep their place
    /// (headroom 1/8) while their content fits, so a change rewrites only the tiles it touches.
    std::vector<u32> tiles;
    std::vector<GpuPass> passes; ///< in order; passes[0] is always a draw pass
    std::vector<u32> images; ///< per image: offset (in u32 pixels into `pixels`), width, height, 0; then pixels
    std::vector<u32> prevImages; ///< the last full pack's images (a full pack swaps it in; outgrown slots restore their old words from it)
    u32 prevArenaBase = 0;
    GpuPush push{};
    const u8* atlas = nullptr; ///< glyph coverage atlas (atlasSize^2 bytes), uploaded as-is
    u32 atlasSize = 0;
    /// fuse::text::GlyphAtlas::version() of `atlas`; the renderer re-uploads the atlas only when it changes.
    /// ~0u (packDrawList's default) = unknown, upload every frame.
    u32 atlasGeneration = ~0u;

    /// packDrawList scratch: per-glyph ink boxes (open-addressed by atlas rect, rebuilt every pack; kept for capacity)
    std::vector<u64> inkKeys;
    std::vector<u32> inkBoxes;
    const u8* inkAtlas = nullptr; ///< ink boxes stay valid while atlas pointer, size and generation match
    u32 inkSize = 0, inkGen = ~0u;
    std::size_t inkCount = 0;

    /// Persistent packing: a new process-unique value whenever packDrawList produces new content. When the inputs equal the previous pack
    /// (same list contents, size, scissor, image pixels, and a known unchanged atlasGeneration), packDrawList keeps the
    /// packed buffers, sets `reused`, and leaves `contentVersion` alone, so UiVkRenderer::upload skips the copy.
    u64 contentVersion = 0;
    bool reused = false;
    /// Colour-only frame: the inputs differ from the previous pack only in DrawCmd::fill, so packDrawList patched the
    /// packed fills in place (GpuCmd::fill, or the glyph record's fill word pointing at words appended to `images`)
    /// instead of repacking; the rest of the buffers keep their layout and the upload diff stays small.
    bool patched = false;
    std::vector<u32> cmdSlot;  ///< per DrawList command: GpuCmd index, kCompactGlyph | record offset, or ~0u (not packed)
    std::vector<u32> patches;  ///< scratch: fill-only changed command indices
    u32 packedImageWords = 0;

    /// Glyph slot arena (the tail of `images`, from `arenaBase`): every source node (DrawCmd::node) owns a slot of
    /// compact glyph records that stays put while its glyphs fit (headroom 1/8), and fill colours are interned there.
    /// Adding, removing or moving a node's glyphs rewrites only its slot; the other records keep their words, so
    /// UiVkRenderer::upload's page diff skips them. Volatile words (images, edges, stops, runs) stay below
    /// `arenaBase`, which moves up (one repack) only when they outgrow it; holes are compacted when they dominate.
    struct GlyphSlot {
        u32 off = 0, cap = 0; ///< arena word offset, capacity in records
        u32 count = 0, cursor = 0, stamp = 0;
    };
    struct FillKey {
        u32 w[4];
        bool operator==(const FillKey& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3]; }
    };
    struct FillHash {
        std::size_t operator()(const FillKey& k) const {
            return std::size_t((u64(k.w[0]) * 0x9E3779B97F4A7C15ull) ^ (u64(k.w[1]) * 0xC2B2AE3D27D4EB4Full) ^
                               (u64(k.w[2]) * 0x165667B19E3779F9ull) ^ (u64(k.w[3]) * 0x27D4EB2F165667C5ull));
        }
    };
    std::vector<u32> arena;
    std::unordered_map<u32, GlyphSlot> glyphSlots;
    std::unordered_map<FillKey, u32, FillHash> fillSlots; ///< colour -> absolute images word offset
    u32 arenaBase = 0, arenaHoles = 0, arenaLive = 0, frameStamp = 0;
    std::vector<u32> glyphScratch;          ///< glyphs not written in place (incremental packs, outgrown nodes): 7 record words + node + source
    std::vector<u32> touched;
    std::vector<u32> arenaSrc; ///< per arena word: source DrawList index of the record starting there

    /// Command slots: `cmds` is a node-keyed arena like the glyph records (GpuCmd slots per DrawCmd::node).
    std::unordered_map<u32, GlyphSlot> cmdSlots;
    u32 cmdHoles = 0, cmdLive = 0;
    std::vector<u32> cmdSrc;               ///< per cmds entry: source DrawList index
    std::vector<GpuCmd> cmdScratch;        ///< packDrawList scratch: GpuCmds in pack order
    std::vector<u32> cmdScratchNode, cmdScratchSrc, cmdMap;
    std::vector<GlyphSlot*> cmdSlotOf;
    /// Tile slots per (pass, tile): word offset and capacity in `tiles`
    std::vector<u32> tileSlotOff, tileSlotCap;
    u32 tilePasses = 0, tileCountPrev = 0, tileHoles = 0, tileLive = 0;

    /// Incremental structural pack: a frame whose commands differ from the previous one in one contiguous range
    /// of plain draw commands (no clip / backdrop / path / stop-gradient ops, single draw pass) re-emits only that
    /// range: its records go into free spots of their node's slots, and only the tiles it touches are rebuilt.
    bool incremental = false;
    std::vector<u64> cmdBox;             ///< per DrawList command: pixel box (u16 x0|y0|x1|y1), ~0 = none
    std::vector<u64> cmdVol;             ///< per DrawList command: its volatile words (offset | count << 32) in the arena tail
    std::vector<u32> passHeader;         ///< per draw segment: tile header offset in `tiles`
    std::vector<GpuPass> blurPasses;     ///< per backdrop: the blur pass after its segment
    std::vector<u32> backdropSrc;        ///< DrawList indices of the effective backdrops (one per segment)
    struct ClipRun {
        u32 from; ///< first DrawList index under this clip
        Rect r;
        f32 radius;
        u32 pathList;
        u32 depth; ///< clip stack depth there
    };
    std::vector<ClipRun> clipRuns;
    std::vector<std::pair<u32, u32>> freeRecs, freeCmds; ///< (node, absolute record offset / cmds index)
    void resetArena() {
        arena.clear();
        arenaSrc.clear();
        freeRecs.clear();
        glyphSlots.clear();
        fillSlots.clear();
        arenaHoles = arenaLive = 0;
    }
    /// Interns a fill colour in the arena; returns its absolute images word offset.
    u32 fillSlot(Color c) {
        FillKey k;
        const f32 col[4] = {c.r, c.g, c.b, c.a};
        std::memcpy(k.w, col, sizeof col);
        const auto it = fillSlots.find(k);
        if (it != fillSlots.end()) return it->second;
        const u32 at = arenaBase + u32(arena.size());
        arena.insert(arena.end(), k.w, k.w + 4);
        fillSlots.emplace(k, at);
        return at;
    }  ///< images.size() after the last full pack (patch fills go after it)
    /// previous inputs (compared, then refreshed from the first difference on); kept for capacity
    std::vector<DrawCmd> prevCmds;
    std::vector<PathEdge> prevEdges;
    std::vector<GradientStop> prevStops;
    u32 prevKey[9] = {};
    const u8* prevAtlas = nullptr;

    std::size_t cmdBytes() const { return cmds.size() * sizeof(GpuCmd); }
    std::size_t tileBytes() const { return tiles.size() * 4; }
    std::size_t imageBytes() const { return images.size() * 4; }
    std::size_t atlasBytes() const { return std::size_t(atlasSize) * atlasSize; }
};

/// Packs `list` for a width x height target, restricted to `scissor` (whole target when empty). `images` are the
/// rasteriser image views (same indices as DrawCmd::image); `atlas` / `atlasSize` the glyph atlas. The dispatch
/// region (push.origin / size) is the scissor's pixel box, so a damage scissor repaints only the damaged pixels.
/// `atlasGeneration` (fuse::text::GlyphAtlas::version()) is stored in out.atlasGeneration; when known (not ~0u) an
/// unchanged frame reuses the previous pack (GpuDrawData::reused) instead of repacking.
void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas = nullptr,
                  u32 atlasSize = 0, const Image* const* images = nullptr, u32 imageCount = 0, Rect scissor = {},
                  u32 atlasGeneration = ~0u);

} // namespace fuse::ui::render
