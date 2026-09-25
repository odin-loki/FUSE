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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (D3D9Rtx::processVertices) and DXVK's
// D3D9VertexDecl size classification (src/d3d9/d3d9_vertex_declaration.cpp).
// FUSE changes: stream windows are bounds-checked and copied into padded, shared CPU buffers
// (upstream copies into its staging ring or uses the buffer directly).
#include <fuse/relight/capture/geometry/vertex_streams.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::geometry {

namespace {

/// GetDecltypeSize: the D3D byte size of a declaration type.
std::uint32_t d3dDeclTypeSize(std::uint8_t type) noexcept {
    static constexpr std::uint32_t kSizes[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0};
    return type < std::size(kSizes) ? kSizes[type] : 0;
}

/// Bytes read past the start of an element by any consumer: the element itself (the vertex
/// region hashes), 12 bytes (legacy positions, the bounding box) and 4 bytes (blend indices).
std::uint32_t readExtent(const tap::VertexElement& e) noexcept {
    return std::max<std::uint32_t>(hash::declTypeElementSize(hash::D3DDeclType(e.type)), 12u);
}

} // namespace

std::uint32_t declStreamSize(std::span<const tap::VertexElement> elements, std::uint32_t stream) noexcept {
    std::uint32_t size = 0;
    for (const tap::VertexElement& e : elements) {
        if (e.stream == stream) {
            size = std::max<std::uint32_t>(size, std::uint32_t(e.offset) + d3dDeclTypeSize(e.type));
        }
    }
    return size;
}

SliceStatus sliceVertexStreams(std::span<const tap::VertexElement> elements, std::span<const StreamSource> streams,
                               const SliceRequest& request, SlicedVertices& out) {
    out = SlicedVertices{};
    std::vector<SharedBytes> copies(streams.size());

    for (const tap::VertexElement& e : elements) {
        if (e.usage == std::uint8_t(DeclUsage::BlendIndices)) {
            out.hasBlendIndicesElement = true;
        }
        if (e.usage == std::uint8_t(DeclUsage::PositionT)) {
            out.hasPositionT = true;
        }
    }

    for (const tap::VertexElement& e : elements) {
        if (e.stream >= streams.size()) {
            continue;
        }
        const StreamSource& src = streams[e.stream];
        if (src.data == nullptr) {
            continue; // ctx.mappedSlice.handle == VK_NULL_HANDLE
        }

        VertexAttribute* target = nullptr;
        switch (DeclUsage(e.usage)) {
        case DeclUsage::PositionT:
        case DeclUsage::Position:
            if (e.usageIndex == 0) {
                target = &out.position;
            }
            break;
        case DeclUsage::BlendWeight:
            if (e.usageIndex == 0) {
                target = &out.blendWeight;
            }
            break;
        case DeclUsage::BlendIndices:
            if (e.usageIndex == 0) {
                target = &out.blendIndices;
            }
            break;
        case DeclUsage::Normal:
            if (e.usageIndex == 0) {
                target = &out.normal;
            }
            break;
        case DeclUsage::Texcoord:
            if (request.texcoordIndex <= kMaxDeclUsageIndex && e.usageIndex == request.texcoordIndex) {
                target = &out.texcoord;
            }
            break;
        case DeclUsage::Color:
            if (e.usageIndex == 0 && request.colorAllowed) {
                target = &out.color0;
            }
            break;
        default:
            break;
        }
        if (target == nullptr) {
            continue;
        }

        // Only do once for each stream.
        if (!copies[e.stream]) {
            const std::int64_t vertexOffset =
                std::int64_t(src.offset) + std::int64_t(src.stride) * request.vertexIndexOffset;
            const std::uint64_t numVertexBytes = std::uint64_t(src.stride) * request.vertexCount;
            if (vertexOffset < 0 || std::uint64_t(vertexOffset) + numVertexBytes > src.size) {
                out = SlicedVertices{};
                return SliceStatus::OutOfBounds;
            }
            // Pad so that any consumer's read at the last vertex stays inside the copy.
            std::uint32_t maxEnd = 0;
            for (const tap::VertexElement& other : elements) {
                if (other.stream == e.stream) {
                    maxEnd = std::max<std::uint32_t>(maxEnd, std::uint32_t(other.offset) + readExtent(other));
                }
            }
            const std::size_t pad = maxEnd > src.stride ? maxEnd - src.stride : 0;
            auto copy = std::make_shared<ByteBuffer>(std::size_t(numVertexBytes) + pad, std::uint8_t(0));
            if (numVertexBytes != 0) {
                std::memcpy(copy->data(), src.data + vertexOffset, std::size_t(numVertexBytes));
            }
            // Bytes past the window that the buffer does have (upstream reads the buffer there).
            const std::uint64_t available = src.size - (std::uint64_t(vertexOffset) + numVertexBytes);
            const std::size_t tail = std::size_t(std::min<std::uint64_t>(available, pad));
            if (tail != 0) {
                std::memcpy(copy->data() + numVertexBytes, src.data + vertexOffset + numVertexBytes, tail);
            }
            out.streamsCopied += 1;
            out.bytesCopied += copy->size();
            copies[e.stream] = std::move(copy);
        }

        VertexAttribute attribute;
        attribute.data = copies[e.stream];
        attribute.offset = e.offset;
        attribute.stride = src.stride;
        attribute.type = hash::D3DDeclType(e.type);
        attribute.stream = e.stream;
        attribute.usageIndex = e.usageIndex;
        attribute.windowBytes = std::size_t(src.stride) * request.vertexCount;
        *target = std::move(attribute);
    }
    // FUSE (RL-4.2 emissive from COLOR1): COLOR[1] over a stream copy made above; never a copy of its own, so the
    // hashes, the copied bytes and the status stay upstream's.
    for (const tap::VertexElement& e : elements) {
        if (e.usage != std::uint8_t(DeclUsage::Color) || e.usageIndex != 1 || !request.colorAllowed ||
            e.stream >= streams.size() || !copies[e.stream]) {
            continue;
        }
        VertexAttribute attribute;
        attribute.data = copies[e.stream];
        attribute.offset = e.offset;
        attribute.stride = streams[e.stream].stride;
        attribute.type = hash::D3DDeclType(e.type);
        attribute.stream = e.stream;
        attribute.usageIndex = e.usageIndex;
        attribute.windowBytes = std::size_t(streams[e.stream].stride) * request.vertexCount;
        out.color1 = std::move(attribute);
    }
    return SliceStatus::Ok;
}

} // namespace fuse::relight::capture::geometry
