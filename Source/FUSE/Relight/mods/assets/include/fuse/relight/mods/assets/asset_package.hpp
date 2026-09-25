// FUSE Relight RL-3.3: Remix `.pkg` / `.rtxio` asset package reader (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6).
//
// Port of dxvk-remix's AssetPackage (src/dxvk/rtx_render/rtx_asset_package.h) and the blob addressing of
// PackagedAssetData (rtx_asset_data_manager.cpp) at 0867d3c:
//   Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved. MIT licence (dxvk-remix LICENSE-MIT):
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
//   associated documentation files (the "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
//   following conditions: The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//   ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
//   FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Modified for FUSE (2026): rewritten as a bounds-checked parser over a byte source (no FILE* state, no Rc),
// images assembled through the RL-3.3 GDeflate codec, plus a writer for tools and tests.
//
// File layout (little-endian):
//   Header    {u32 magic = 0xbaadd00d, u32 version = 1, u64 dictOffset}
//   blobs     raw or GDeflate tile streams, anywhere before dictOffset
//   @dictOffset: u16 assetCount, u16 blobCount, AssetDesc[assetCount] (20 bytes), BlobDesc[blobCount] (16)
//   names     assetCount NUL-terminated names up to EOF; asset n is named by the n-th string (upstream keys
//             its name map by position, AssetDesc::nameIdx is carried but unused)
// AssetDesc {u16 nameIdx, u8 type, u8 VkFormat, union{u32 size | u16 width, u16 height}, u16 depth,
//            u16 numMips, u16 numTailMips, u16 arraySize, u16 baseBlobIdx, u16 tailBlobIdx}
// BlobDesc  {u64 offset:40, compression:8, flags:8; u32 size (stored bytes); u32 crc32}
//
// Blob addressing (upstream getBlobIndex): cube images address layer * 6 + face; with
// numLoose = numMips - numTailMips, level < numLoose is blob baseBlobIdx + level + layer * numLoose, the
// mip tail is blob tailBlobIdx + layer * numLoose (upstream's formula, kept) and holds levels
// numLoose..numMips-1 back to back, tightly packed. Compression != 0 means GDeflate (the only method
// upstream supports). Upstream never checks crc32; this reader exposes it (blobCrcMatches) without
// enforcing it.
#pragma once

#include <fuse/relight/mods/assets/texture_format.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::relight::mods::assets {

enum class PackageAssetType : std::uint8_t { Unknown, Image1D, Image2D, Image3D, ImageCube, Buffer };

struct PackageAssetDesc {
    std::uint16_t nameIdx = 0;
    PackageAssetType type = PackageAssetType::Unknown;
    std::uint8_t format = 0; ///< VkFormat (narrowed)
    std::uint32_t size = 0;  ///< Buffer: byte size (shares storage with width/height)
    std::uint16_t width = 0, height = 0;
    std::uint16_t depth = 0;
    std::uint16_t numMips = 0, numTailMips = 0, arraySize = 0;
    std::uint16_t baseBlobIdx = 0, tailBlobIdx = 0;
};

struct PackageBlobDesc {
    std::uint64_t offset = 0; ///< 40 bits
    std::uint8_t compression = 0;
    std::uint8_t flags = 0;
    std::uint32_t size = 0; ///< stored (compressed) bytes
    std::uint32_t crc32 = 0;
};

class AssetPackage {
public:
    static constexpr std::uint32_t kMagic = 0xbaadd00d;
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint32_t kNoIndex = ~0u;
    static constexpr std::size_t kHeaderSize = 16, kAssetDescSize = 20, kBlobDescSize = 16;
    /// Largest image or buffer the reader decodes (guards against hostile descriptors).
    static constexpr std::uint64_t kMaxDecodedBytes = std::uint64_t(1) << 31;

    /// Reads `size` bytes at `offset` into `dst`; false when out of range or on I/O failure.
    using ReadFn = std::function<bool(std::uint64_t offset, std::size_t size, std::uint8_t* dst)>;

    /// Parses a package held in memory (the bytes are copied).
    static std::optional<AssetPackage> fromBytes(std::span<const std::uint8_t> bytes, std::string* error = nullptr);
    /// Opens a package file: the header, dictionary and names are read now, blobs on demand.
    static std::optional<AssetPackage> open(const std::filesystem::path& path, std::string* error = nullptr);
    /// Any byte source of `size` bytes.
    static std::optional<AssetPackage> fromSource(ReadFn read, std::uint64_t size, std::string* error = nullptr);

    std::uint32_t assetCount() const { return static_cast<std::uint32_t>(assets_.size()); }
    std::uint32_t blobCount() const { return static_cast<std::uint32_t>(blobs_.size()); }
    const PackageAssetDesc* asset(std::uint32_t idx) const { return idx < assets_.size() ? &assets_[idx] : nullptr; }
    const PackageBlobDesc* blob(std::uint32_t idx) const { return idx < blobs_.size() ? &blobs_[idx] : nullptr; }
    const std::string& assetName(std::uint32_t idx) const;
    /// Index of the asset named `name` (first match, as upstream), kNoIndex when absent.
    std::uint32_t findAsset(const std::string& name) const;
    /// Bytes before the dictionary (upstream getDataSize).
    std::uint64_t dataSize() const { return dictOffset_; }

    /// Stored bytes of a blob.
    std::optional<std::vector<std::uint8_t>> readBlobRaw(std::uint32_t idx, std::string* error = nullptr) const;
    /// Decoded bytes of a blob (GDeflate when compression != 0). `maxDecoded` bounds the output.
    std::optional<std::vector<std::uint8_t>> readBlob(std::uint32_t idx, std::string* error = nullptr,
                                                      std::uint64_t maxDecoded = kMaxDecodedBytes) const;
    /// True when crc32 of the stored bytes equals BlobDesc::crc32 (informational: upstream never checks).
    bool blobCrcMatches(std::uint32_t idx) const;

    /// Blob holding (layer, face, level) of an image asset, or the buffer blob; kNoIndex when out of range.
    std::uint32_t blobIndex(std::uint32_t assetIdx, std::uint32_t layer, std::uint32_t face, std::uint32_t level) const;
    /// Every subresource of an image asset, decoded and laid out like a DDS file (TextureImage).
    std::optional<TextureImage> loadImage(std::uint32_t assetIdx, std::string* error = nullptr) const;
    /// A buffer asset's bytes.
    std::optional<std::vector<std::uint8_t>> loadBuffer(std::uint32_t assetIdx, std::string* error = nullptr) const;

private:
    ReadFn read_;
    std::uint64_t size_ = 0;
    std::uint64_t dictOffset_ = 0;
    std::vector<PackageAssetDesc> assets_;
    std::vector<PackageBlobDesc> blobs_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, std::uint32_t> nameIndex_;
};

/// Builds a package (tools and tests). Blobs are laid out in the order added.
class AssetPackageWriter {
public:
    /// Adds a blob; `gdeflateLevel` > 0 stores it GDeflate-compressed (needs the codec). kNoIndex on failure.
    std::uint32_t addBlob(std::span<const std::uint8_t> bytes, std::uint32_t gdeflateLevel = 0, std::string* error = nullptr);
    void addAsset(const std::string& name, const PackageAssetDesc& desc);
    /// Adds an image asset from `image` (format must fit in 8 bits): one blob per loose level and layer,
    /// one mip-tail blob per layer holding the last `tailMips` levels.
    bool addImage(const std::string& name, const TextureImage& image, std::uint32_t tailMips, std::uint32_t gdeflateLevel,
                  std::string* error = nullptr);
    std::vector<std::uint8_t> finish() const;

private:
    std::vector<std::uint8_t> data_; ///< blob bytes, written after the header
    std::vector<PackageBlobDesc> blobs_;
    std::vector<PackageAssetDesc> assets_;
    std::vector<std::string> names_;
};

} // namespace fuse::relight::mods::assets
