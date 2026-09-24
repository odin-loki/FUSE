/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// FUSE Relight RL-1.3: slicing a draw's vertex streams by declaration usage.
//
// Ported from dxvk-remix @0867d3c (MIT), src/d3d9/d3d9_rtx.cpp D3D9Rtx::processVertices:
// every declaration element whose usage Remix consumes (POSITION[0] / POSITIONT[0], BLENDWEIGHT[0],
// BLENDINDICES[0], NORMAL[0], TEXCOORD[m_texcoordIndex], COLOR[0] unless baked vertex lighting is
// ignored) becomes an attribute over one copy per stream of the draw's vertex window:
// stride * vertexCount bytes starting at streamOffset + stride * vertexIndexOffset, where
// vertexIndexOffset = BaseVertexIndex (+ minIndex for indexed draws) and can be negative.
// Modifications Copyright (c) 2026 FUSE contributors (MIT).
//
// Differences from upstream, both only for draws upstream reads out of bounds for:
//   * a window that is not inside the stream's buffer makes the draw OutOfBounds (upstream checks
//     this only with rtx.validateCPUIndexData, and otherwise reads past the buffer);
//   * each copy is padded (zeros) so that reading an element, or the 12 bytes the legacy position
//     hash reads, at the last vertex stays inside the copy.
#pragma once

#include <fuse/relight/hash/d3d_types.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fuse::relight::capture::geometry {

/// D3DDECLUSAGE values.
enum class DeclUsage : std::uint8_t {
    Position = 0,
    BlendWeight = 1,
    BlendIndices = 2,
    Normal = 3,
    PSize = 4,
    Texcoord = 5,
    Tangent = 6,
    Binormal = 7,
    TessFactor = 8,
    PositionT = 9,
    Color = 10,
    Fog = 11,
    Depth = 12,
    Sample = 13,
};

/// MAXD3DDECLUSAGEINDEX: a texcoord index above it (TCI generation flags) selects no stream.
inline constexpr std::uint32_t kMaxDeclUsageIndex = 15;

using ByteBuffer = std::vector<std::uint8_t>;
using SharedBytes = std::shared_ptr<const ByteBuffer>;

/// One vertex attribute over a stream copy (upstream RasterBuffer).
struct VertexAttribute {
    SharedBytes data;           ///< the stream's vertex window (vertex 0 = vertexIndexOffset), padded
    std::uint32_t offset = 0;   ///< element offset within a vertex
    std::uint32_t stride = 0;   ///< stream stride
    hash::D3DDeclType type = hash::D3DDeclType::Unused;
    std::uint32_t stream = 0;   ///< D3D stream (identity of the copy)
    std::uint32_t usageIndex = 0;

    [[nodiscard]] bool defined() const noexcept { return data != nullptr; }
    /// The element of vertex 0.
    [[nodiscard]] const std::uint8_t* base() const noexcept { return data->data() + offset; }
    /// Bytes of the window (stride * vertexCount), without the padding.
    std::size_t windowBytes = 0;
};

/// A vertex stream as bound at the draw: the whole buffer's bytes plus the binding.
struct StreamSource {
    const std::uint8_t* data = nullptr; ///< byte 0 of the buffer (nullptr: nothing bound / no data)
    std::size_t size = 0;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

struct SliceRequest {
    std::int64_t vertexIndexOffset = 0; ///< BaseVertexIndex (+ minIndex); may be negative
    std::uint32_t vertexCount = 0;
    std::uint32_t texcoordIndex = 0;    ///< m_texcoordIndex (raw D3DTSS_TEXCOORDINDEX value)
    bool colorAllowed = true;           ///< !ignoreAllVertexColorBakedLighting && texture 0 not listed
};

enum class SliceStatus : std::uint32_t { Ok = 0, OutOfBounds = 1 };

struct SlicedVertices {
    VertexAttribute position;     ///< POSITION[0] or POSITIONT[0] (the later element wins)
    VertexAttribute blendWeight;  ///< BLENDWEIGHT[0]
    VertexAttribute blendIndices; ///< BLENDINDICES[0]
    VertexAttribute normal;       ///< NORMAL[0]
    VertexAttribute texcoord;     ///< TEXCOORD[texcoordIndex]
    VertexAttribute color0;       ///< COLOR[0]
    bool hasBlendIndicesElement = false; ///< D3D9VertexDeclFlag::HasBlendIndices (any usage index)
    bool hasPositionT = false;           ///< D3D9VertexDeclFlag::HasPositionT
    std::uint32_t streamsCopied = 0;
    std::size_t bytesCopied = 0;
};

/// D3D9VertexDecl::GetSize(stream): max(offset + D3D size of the type) over the stream's elements.
[[nodiscard]] std::uint32_t declStreamSize(std::span<const tap::VertexElement> elements, std::uint32_t stream) noexcept;

/// processVertices. `streams` is indexed by D3D stream (kStreamCount entries).
[[nodiscard]] SliceStatus sliceVertexStreams(std::span<const tap::VertexElement> elements,
                                             std::span<const StreamSource> streams, const SliceRequest& request,
                                             SlicedVertices& out);

} // namespace fuse::relight::capture::geometry
