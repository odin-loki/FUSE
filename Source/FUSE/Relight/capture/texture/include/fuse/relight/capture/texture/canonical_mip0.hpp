// FUSE Relight RL-1.4: the canonical mip-0 shadow of a texture (docs/plans/FUSE_REMIX_PORT_PLAN.md
// §4.1.3).
//
// Remix hashes the whole mip-0 staging buffer of a texture in DXVK's packed layout
// (D3D9CommonTexture::GetMipSize): min(planeCount, 2) planes of blocksHigh * depth rows of
// align(elementSize * blocksWide, 4) bytes. CanonicalMip0 keeps that buffer on the FUSE side and
// re-packs every write the application makes through LockRect / LockBox into it, whatever pitch
// the vendored DXVK handed out, so the hash never depends on DXVK's pitch or allocator. Like DXVK's
// staging buffer it starts zero-filled, and partial locks update only their rectangle.
#pragma once

#include <fuse/relight/hash/texture_hash.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuse::relight::capture::texture {

/// A texel rectangle of mip 0 (D3DBOX / RECT semantics: right and bottom exclusive).
struct TexelRect {
    std::uint32_t left = 0, top = 0, right = 0, bottom = 0;
};

class CanonicalMip0 {
public:
    CanonicalMip0() = default;
    CanonicalMip0(hash::D3DFormat format, std::uint32_t width, std::uint32_t height,
                  const hash::FormatTableOptions& options = {});

    const hash::TextureMip0Layout& layout() const { return m_layout; }
    const hash::TextureFormatInfo& formatInfo() const { return m_info; }
    /// The buffer exists (DXVK: the subresource's staging buffer was created by a lock or, for
    /// D3DPOOL_SYSTEMMEM, at creation).
    bool allocated() const { return !m_bytes.empty() || m_layout.size == 0; }
    /// Create the zero-filled buffer if it does not exist yet.
    void allocate();
    /// Drop the buffer (memory); allocated() becomes false.
    void release();

    /// A full-subresource lock: `rows` rows of the canonical layout, `rowPitch` bytes apart, from
    /// `data`. Each row copies min(rowPitch, rowBytes) bytes; rows past `rows` keep their contents.
    void writeFull(const void* data, std::size_t rowPitch, std::uint64_t rows);
    /// A partial lock of `rect` (texels, block-aligned by D3D9 rules): `data` points at the
    /// rectangle's first block, rows `rowPitch` apart. Only the first plane of planar formats is
    /// addressed. Returns false (and writes nothing) for an empty or out-of-range rectangle.
    bool writeRect(const void* data, std::size_t rowPitch, const TexelRect& rect);

    const std::vector<std::uint8_t>& bytes() const { return m_bytes; }

    /// XXH3_64bits over the buffer ("remix.tex"), or XXH64(.., 0) with `obsolete`
    /// (rtx.useObsoleteHashOnTextureUpload). The buffer is allocated first if needed.
    hash::Hash64 hash(bool obsolete);

private:
    hash::TextureFormatInfo m_info;
    hash::TextureMip0Layout m_layout;
    std::vector<std::uint8_t> m_bytes;
};

} // namespace fuse::relight::capture::texture
