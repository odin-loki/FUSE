// FUSE UI icon packs (.fuseicons, plan §4.9 / UI4): monochrome vector icons cooked by fuse_ui_icon_cook into an
// 8-bit coverage atlas at a few pixel sizes, plus a name index. The runtime never parses SVG.
//
// Drawing: UiContext::setIcons(set); widgets get an icon through the `icon` style property (`icon: save`,
// `icon-size: 24px`) or widgets::icon(). Icons are copied into the context's glyph atlas on first use and drawn as
// Glyph commands tinted with the widget's text colour (SVG currentColor), so they follow theme, hover and disabled
// colours, and the GPU backend draws them with no extra ops. Sizes that were not cooked are area-resampled from the
// nearest larger cooked size (or upscaled from the largest).
//
// Pack layout (little endian, version 1):
//   "FICN" u32 version | u16 atlasW, atlasH | u16 sizeCount, u16 sizes[sizeCount] | u32 iconCount
//   icons sorted by name: u32 nameHash (hashId), u16 nameLen, name bytes, then per size: u16 u, v, w, h, ox, oy
//     (the trimmed bitmap at atlas (u, v), placed at (ox, oy) inside the size x size cell; w = 0 for an empty icon)
//   atlas: atlasW * atlasH coverage bytes
#pragma once

#include "fuse/ui/types.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

inline constexpr char kIconPackMagic[4] = {'F', 'I', 'C', 'N'};
inline constexpr u32 kIconPackVersion = 1;

/// A coverage bitmap inside a size x size icon cell (pixels point into the pack's atlas or caller scratch).
struct IconBitmap {
    const u8* pixels = nullptr;
    int w = 0, h = 0, stride = 0;
    int ox = 0, oy = 0; ///< offset of the bitmap in the cell
};

class IconSet {
public:
    struct Entry {
        u16 u = 0, v = 0, w = 0, h = 0, ox = 0, oy = 0;
        friend bool operator==(const Entry&, const Entry&) = default;
    };

    /// Validates and loads a pack (copies the bytes it needs). Errors are messages, never crashes.
    static std::expected<IconSet, std::string> fromBytes(std::span<const u8> bytes);
    static std::expected<IconSet, std::string> fromFile(const std::string& path);

    u32 count() const { return u32(m_names.size()); }
    std::string_view name(u32 icon) const { return m_names[icon]; }
    u32 nameHash(u32 icon) const { return m_hashes[icon]; }
    /// Icon index, -1 when unknown.
    i32 find(std::string_view name) const;
    i32 findHash(u32 nameHash) const;
    std::span<const u16> sizes() const { return m_sizes; }
    /// Atlas entry of `icon` at cooked size index `sizeIndex`.
    const Entry& entry(u32 icon, u32 sizeIndex) const { return m_entries[std::size_t(icon) * m_sizes.size() + sizeIndex]; }
    int atlasWidth() const { return m_atlasW; }
    int atlasHeight() const { return m_atlasH; }
    const u8* atlas() const { return m_atlas.data(); }

    /// The cooked bitmap of `icon` at exactly `size` px; false when that size was not cooked.
    bool cooked(u32 icon, int size, IconBitmap& out) const;
    /// size x size coverage of `icon` (cooked, or resampled from a cooked size) into `out` (resized; keeps capacity).
    void render(u32 icon, int size, std::vector<u8>& out) const;

private:
    int m_atlasW = 0, m_atlasH = 0;
    std::vector<u16> m_sizes;
    std::vector<std::string> m_names;
    std::vector<u32> m_hashes;
    std::vector<u32> m_byHash; ///< icon indices sorted by hash
    std::vector<Entry> m_entries;
    std::vector<u8> m_atlas;
};

} // namespace fuse::ui
