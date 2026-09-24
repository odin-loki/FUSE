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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (D3D9Rtx::internalPrepareDraw,
// PrepareDrawGeometryForRT, PrepareDrawUPGeometryForRT, processIndexBuffer),
// src/d3d9/d3d9_rtx_geometry.cpp@0867d3c (computeHash, computeAxisAlignedBoundingBox) and
// src/d3d9/d3d9_device.cpp@0867d3c (the draw contexts of DrawPrimitive*, the UP uploads and the
// index-buffer memoization invalidation in LockBuffer).
// FUSE changes: inputs are IRelightTap events instead of DXVK state; the component hashes come
// from the RL-0.5 hash library; jobs run on fuse::jobs::JobScheduler.
#include <fuse/relight/capture/geometry/geometry_capture.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace fuse::relight::capture::geometry {

namespace {

constexpr std::uint32_t kD3DFmtIndex16 = 101;
constexpr std::uint32_t kD3DFmtIndex32 = 102;
constexpr std::uint32_t kResourceTexture = 3;

hash::IndexType indexTypeOfFormat(std::uint32_t format) noexcept {
    return format == kD3DFmtIndex32 ? hash::IndexType::Uint32 : hash::IndexType::Uint16;
}

hash::VertexLayoutElement layoutElement(const VertexAttribute& a) noexcept {
    return hash::VertexLayoutElement{a.defined(), a.stream, a.stride, hash::declTypeVkFormat(a.type)};
}

} // namespace

// ---- jobs ----------------------------------------------------------------------------------------

hash::GeometryHashes runHashJob(const HashJobInput& in) {
    using hash::HashComponent;
    hash::GeometryHashes h = in.precomputed;

    std::span<const std::uint32_t> unique;
    if (in.indices) {
        unique = in.indices->uniqueIndices();
        if (in.rule.test(HashComponent::Indices)) {
            h[HashComponent::Indices] = in.indices->indicesHash();
        }
        if (in.rule.test(HashComponent::LegacyIndices)) {
            h[HashComponent::LegacyIndices] = in.indices->legacyIndicesHash();
        }
    }

    const auto region = [&](const VertexAttribute& a) -> hash::Hash64 {
        if (!a.defined()) {
            return hash::hashVertexRegion(nullptr, 0, 0, 0, unique);
        }
        return hash::hashVertexRegion(a.base(), std::size_t(a.stride) * in.vertexCount, a.stride,
                                      hash::declTypeElementSize(a.type), unique);
    };
    if (in.rule.test(HashComponent::Positions)) {
        h[HashComponent::Positions] = region(in.position);
    }
    if (in.rule.test(HashComponent::Texcoords)) {
        h[HashComponent::Texcoords] = region(in.texcoord);
    }
    if ((in.rule.test(HashComponent::LegacyPositions0) || in.rule.test(HashComponent::LegacyPositions1)) &&
        in.position.defined()) {
        hash::hashPositionsLegacy(in.position.base(), std::size_t(in.position.stride) * in.vertexCount,
                                  in.position.stride, hash::legacyDiscreteStepSize(in.sceneScale),
                                  h[HashComponent::LegacyPositions0], h[HashComponent::LegacyPositions1]);
    }
    return h;
}

BoundingBox computeBoundingBox(const VertexAttribute& position, std::uint32_t vertexCount) noexcept {
    // _mm_min_ps(m, v) = m < v ? m : v and _mm_max_ps(m, v) = m > v ? m : v per lane, starting
    // from +/-FLT_MAX (a NaN coordinate therefore replaces the running value, as upstream).
    constexpr float kMax = 3.40282347e+38f;
    BoundingBox box{{kMax, kMax, kMax}, {-kMax, -kMax, -kMax}};
    if (!position.defined()) {
        return box;
    }
    const std::uint8_t* p = position.base();
    for (std::uint32_t i = 0; i < vertexCount; ++i, p += position.stride) {
        float v[3];
        std::memcpy(v, p, sizeof(v));
        for (int k = 0; k < 3; ++k) {
            box.minPos[k] = box.minPos[k] < v[k] ? box.minPos[k] : v[k];
            box.maxPos[k] = box.maxPos[k] > v[k] ? box.maxPos[k] : v[k];
        }
    }
    return box;
}

// ---- CapturedDraw ----------------------------------------------------------------------------------

std::string_view captureStatusName(CaptureStatus status) noexcept {
    switch (status) {
    case CaptureStatus::Captured:
        return "captured";
    case CaptureStatus::NoPosition:
        return "no_position";
    case CaptureStatus::DegenerateIndices:
        return "degenerate_indices";
    case CaptureStatus::NoVertices:
        return "no_vertices";
    case CaptureStatus::NoIndexData:
        return "no_index_data";
    case CaptureStatus::OutOfBounds:
        return "out_of_bounds";
    }
    return "unknown";
}

hash::Hash64 CapturedDraw::assetHash(hash::HashRule assetRule, hash::Hash64 materialHash) const {
    if (!hashes.valid()) {
        return 0;
    }
    return hash::meshReplacementHash(hashes.get(), assetRule, materialHash);
}

hash::DrawGeometryHashes CapturedDraw::geometryHashes() const {
    hash::DrawGeometryHashes out;
    if (hashes.valid()) {
        out.hashes = hashes.get();
    }
    out.indexCount = indexCount;
    out.vertexCount = vertexCount;
    out.minIndex = minIndex;
    out.maxIndex = maxIndex;
    out.topology = topology;
    out.indexType = indexType;
    out.positionStride = positionStride;
    if (indices) {
        out.rebasedIndices = indices->bytes();
    }
    return out;
}

// ---- GeometryCapture -------------------------------------------------------------------------------

GeometryCapture::GeometryCapture(GeometryCaptureConfig config) : m_config(std::move(config)) {}

GeometryCapture::~GeometryCapture() = default;

void GeometryCapture::setDrawSink(DrawSink sink) {
    std::lock_guard lock(m_mutex);
    m_sink = std::move(sink);
}

std::vector<CapturedDrawPtr> GeometryCapture::takeDraws() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_pending, {});
}

GeometryCaptureStats GeometryCapture::stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

void GeometryCapture::onTextureCreate(const tap::TextureDesc& desc) {
    std::lock_guard lock(m_mutex);
    m_textures[desc.id] = TextureInfo{desc.type, false};
}

void GeometryCapture::onTextureUpload(const tap::TextureUpload& upload) {
    std::lock_guard lock(m_mutex);
    // The Remix image hash is set on the first upload of subresource 0 of a D3DRTYPE_TEXTURE.
    if (auto it = m_textures.find(upload.texture);
        it != m_textures.end() && upload.face == 0 && upload.level == 0 && it->second.type == kResourceTexture) {
        it->second.hashKnown = true;
    }
}

void GeometryCapture::onTextureCopy(const tap::TextureCopy& copy) {
    std::lock_guard lock(m_mutex);
    const auto src = m_textures.find(copy.source);
    const auto dst = m_textures.find(copy.destination);
    if (src != m_textures.end() && dst != m_textures.end() && src->second.hashKnown) {
        dst->second.hashKnown = true; // UpdateTexture / UpdateSurface inherit the source hash
    }
}

void GeometryCapture::onImageDestroy(const tap::ImageDestroy& destroy) {
    std::lock_guard lock(m_mutex);
    m_textures.erase(destroy.texture);
}

void GeometryCapture::onBufferCreate(const tap::BufferDesc& desc) {
    std::lock_guard lock(m_mutex);
    BufferInfo& info = m_buffers[desc.id];
    info = BufferInfo{};
    info.kind = desc.kind;
    info.size = desc.size;
    info.format = desc.format;
}

void GeometryCapture::onBufferWrite(const tap::BufferWrite& write) {
    std::lock_guard lock(m_mutex);
    auto it = m_buffers.find(write.buffer);
    if (it == m_buffers.end()) {
        return;
    }
    BufferInfo& info = it->second;
    // LockBuffer: DISCARD invalidates every memoized range, other writable locks the locked range.
    if (write.lockFlags & kD3DLockDiscard) {
        info.memo.invalidateAll();
    } else if (!(write.lockFlags & kD3DLockReadOnly)) {
        info.memo.invalidate(write.offset, write.size);
    }
    if (m_config.shadowBuffers && !(write.lockFlags & kD3DLockReadOnly) && write.data != nullptr) {
        const std::size_t size = std::max<std::size_t>(info.size, write.bufferSize);
        if (info.shadow.size() < size) {
            info.shadow.resize(size, 0);
        }
        const std::size_t end = std::min<std::size_t>(std::size_t(write.offset) + write.size, info.shadow.size());
        if (end > write.offset) {
            std::memcpy(info.shadow.data() + write.offset, write.data, end - write.offset);
        }
    }
}

void GeometryCapture::onBufferDestroy(tap::ResourceId id) {
    std::lock_guard lock(m_mutex);
    m_buffers.erase(id);
}

void GeometryCapture::onPresent(const tap::FrameEvent&) {
    std::lock_guard lock(m_mutex);
    ++m_frame;
    m_drawInFrame = 0;
}

tap::DrawDecision GeometryCapture::onDraw(const tap::DrawCall& call, const tap::DrawState& state) {
    recordDraw(capture(call, state));
    return tap::DrawDecision::Raster;
}

void GeometryCapture::recordDraw(CapturedDrawPtr draw) {
    DrawSink sink;
    {
        std::lock_guard lock(m_mutex);
        ++m_drawInFrame;
        ++m_stats.draws;
        if (draw->captured()) {
            ++m_stats.captured;
        }
        if (draw->skinning.valid()) {
            ++m_stats.skinnedDraws;
        }
        m_stats.streamBytesCopied += draw->vertices.bytesCopied;
        if (!m_sink) {
            m_pending.push_back(std::move(draw));
            return;
        }
        sink = m_sink;
    }
    sink(draw);
}

StageTexture GeometryCapture::stageTexture(tap::ResourceId id) const {
    StageTexture t;
    if (id == tap::kNoResource) {
        return t;
    }
    t.bound = true;
    if (auto it = m_textures.find(id); it != m_textures.end()) {
        t.resourceType = it->second.type;
        t.hashKnown = it->second.hashKnown;
    }
    if (m_config.textureHashKnown) {
        t.hashKnown = m_config.textureHashKnown(id);
    }
    if (m_config.isLightmapTexture) {
        t.lightmap = m_config.isLightmapTexture(id);
    }
    return t;
}

CapturedDrawPtr GeometryCapture::capture(const tap::DrawCall& call, const tap::DrawState& state) {
    auto draw = std::make_shared<CapturedDraw>();
    std::lock_guard lock(m_mutex);

    draw->frame = m_frame;
    draw->drawIndex = m_drawInFrame;
    draw->call = call.call;
    draw->primitiveType = call.primitiveType;
    draw->primitiveCount = call.primitiveCount;
    draw->programmableVs = state.vertexShader.id != tap::kNoResource;
    draw->programmablePs = state.pixelShader.id != tap::kNoResource;

    const auto primitive = hash::D3DPrimitiveType(call.primitiveType);
    draw->topology = std::uint32_t(hash::topologyFromD3D(primitive));

    // ---- texcoordIndex (processRenderState -> processTextures, before processVertices) ----------
    TexcoordSelectInput texInput;
    texInput.textureStageStates = state.textureStageStates;
    texInput.fixedFunctionPixel = !draw->programmablePs;
    texInput.allowCubemaps = m_config.allowCubemaps;
    for (std::uint32_t stage = 0; stage < kFixedFunctionStages; ++stage) {
        texInput.textures[stage] = stageTexture(state.textures[tap::samplerSlot(stage)]);
    }
    draw->texcoord = selectTexcoordIndex(texInput);

    // ---- the draw context and the vertex / index sources ----------------------------------------
    const bool indexed =
        call.call == tap::DrawCallType::DrawIndexedPrimitive || call.call == tap::DrawCallType::DrawIndexedPrimitiveUP;
    const bool up = call.call == tap::DrawCallType::DrawPrimitiveUP || call.call == tap::DrawCallType::DrawIndexedPrimitiveUP;
    std::int64_t baseVertexIndex = 0;
    std::array<StreamSource, tap::kStreamCount> streams{};
    std::shared_ptr<ByteBuffer> upBuffer; // DXVK's UP upload (kept alive while slicing)

    const auto finish = [&](CaptureStatus status) {
        draw->status = status;
        return CapturedDrawPtr(std::move(draw));
    };
    // makeDrawCallType: "Skipped invalid drawcall, primitive count was 0" (GetVertexCount would
    // still give 2 vertices for a strip or fan).
    if (call.primitiveCount == 0) {
        return finish(CaptureStatus::NoVertices);
    }

    if (!up) {
        baseVertexIndex = call.call == tap::DrawCallType::DrawPrimitive ? std::int64_t(call.startVertex)
                                                                        : std::int64_t(call.baseVertex);
        for (std::uint32_t s = 0; s < tap::kStreamCount; ++s) {
            const tap::StreamBinding& b = state.streams[s];
            if (b.buffer == tap::kNoResource) {
                continue;
            }
            StreamSource& src = streams[s];
            src.offset = b.offset;
            src.stride = b.stride;
            if (b.base != nullptr) {
                src.data = static_cast<const std::uint8_t*>(b.base);
                src.size = b.bufferSize;
            } else if (auto it = m_buffers.find(b.buffer); it != m_buffers.end() && !it->second.shadow.empty()) {
                src.data = it->second.shadow.data();
                src.size = it->second.shadow.size();
            }
        }
    }

    std::uint32_t upIndexOffset = 0;
    if (up) {
        const std::uint32_t stride = call.upVertexStride;
        const std::span<const tap::VertexElement> elements(state.elements, std::min(state.elementCount, tap::kMaxVertexElements));
        // DrawPrimitiveUP: GetVertexCount vertices. DrawIndexedPrimitiveUP: MinVertexIndex + NumVertices.
        const std::uint32_t upVertices =
            indexed ? call.minIndex + call.numVertices : hash::d3dVertexCount(primitive, call.primitiveCount);
        const std::uint64_t dataSize = std::uint64_t(upVertices) * stride; // GetUPDataSize
        std::uint64_t bufferSize = dataSize;                                // GetUPBufferSize
        if (upVertices > 0) {
            bufferSize = std::uint64_t(upVertices - 1) * stride + std::max(declStreamSize(elements, 0), stride);
        }
        const std::uint32_t indexSize = call.upIndexFormat == kD3DFmtIndex32 ? 4u : 2u;
        const std::uint64_t indicesSize = indexed ? std::uint64_t(hash::d3dVertexCount(primitive, call.primitiveCount)) * indexSize : 0;
        if (dataSize > 0xffffffffull || bufferSize + indicesSize > 0xffffffffull) {
            return finish(CaptureStatus::OutOfBounds);
        }
        upBuffer = std::make_shared<ByteBuffer>(std::size_t(bufferSize + indicesSize), std::uint8_t(0));
        if (call.upVertexData != nullptr) {
            std::memcpy(upBuffer->data(), call.upVertexData, std::size_t(std::min<std::uint64_t>(dataSize, call.upVertexBytes)));
        }
        if (indexed && call.upIndexData != nullptr) {
            std::memcpy(upBuffer->data() + bufferSize, call.upIndexData,
                        std::size_t(std::min<std::uint64_t>(indicesSize, call.upIndexBytes)));
        }
        // PrepareDrawUPGeometryForRT: vertices[0] = the first vertexSize (= data size) bytes.
        streams[0].data = upBuffer->data();
        streams[0].size = std::size_t(dataSize);
        streams[0].offset = 0;
        streams[0].stride = stride;
        // The indices were written at GetUPBufferSize but Remix reads them at GetUPDataSize.
        upIndexOffset = std::uint32_t(dataSize);
        if (call.upVertexData == nullptr || call.upVertexBytes < dataSize) {
            return finish(CaptureStatus::OutOfBounds);
        }
    }

    // ---- index buffer (processIndexBuffer) --------------------------------------------------------
    std::int64_t vertexIndexOffset = baseVertexIndex; // "This can be negative!!"
    if (indexed) {
        draw->indexCount = hash::d3dVertexCount(primitive, call.primitiveCount);
        std::uint32_t format = up ? call.upIndexFormat : state.indices.format;
        if (!up && format != kD3DFmtIndex16 && format != kD3DFmtIndex32) {
            if (auto it = m_buffers.find(state.indices.buffer); it != m_buffers.end()) {
                format = it->second.format; // the binding did not say: the buffer's creation format
            }
        }
        const hash::IndexType indexType = indexTypeOfFormat(format);
        const std::uint32_t indexSize = indexType == hash::IndexType::Uint32 ? 4u : 2u;
        draw->indexType = std::uint32_t(indexType);
        if (draw->indexCount == 0) {
            return finish(CaptureStatus::NoVertices);
        }
        const std::size_t numIndexBytes = std::size_t(draw->indexCount) * indexSize;
        const auto rebase = [&](const std::uint8_t* data, std::size_t offset, std::size_t) {
            return rebaseIndices(data + offset, draw->indexCount, indexSize);
        };
        if (up) {
            if (std::size_t(upIndexOffset) + numIndexBytes > upBuffer->size()) {
                return finish(CaptureStatus::OutOfBounds);
            }
            draw->indices = rebase(upBuffer->data(), upIndexOffset, numIndexBytes); // no IBO, no memoization
        } else {
            auto it = m_buffers.find(state.indices.buffer);
            const std::uint8_t* data = static_cast<const std::uint8_t*>(state.indices.base);
            std::size_t size = state.indices.bufferSize;
            if (data == nullptr && it != m_buffers.end() && !it->second.shadow.empty()) {
                data = it->second.shadow.data();
                size = it->second.shadow.size();
            }
            if (state.indices.buffer == tap::kNoResource || data == nullptr) {
                return finish(CaptureStatus::NoIndexData);
            }
            const std::size_t indexOffset = std::size_t(indexSize) * call.startIndex;
            if (indexOffset + numIndexBytes > size) {
                return finish(CaptureStatus::OutOfBounds);
            }
            if (m_config.indexBufferMemoization && it != m_buffers.end()) {
                const std::uint64_t missesBefore = it->second.memo.misses();
                draw->indices = it->second.memo.memoize(indexOffset, numIndexBytes, [&](std::size_t offset, std::size_t n) {
                    return rebase(data, offset, n);
                });
                draw->indicesMemoized = it->second.memo.misses() == missesBefore;
                m_stats.memoHits += draw->indicesMemoized ? 1 : 0;
                m_stats.memoMisses += draw->indicesMemoized ? 0 : 1;
            } else {
                draw->indices = rebase(data, indexOffset, numIndexBytes);
            }
        }
        draw->minIndex = draw->indices->minIndex();
        draw->maxIndex = draw->indices->maxIndex();
        if (draw->maxIndex == draw->minIndex) {
            return finish(CaptureStatus::DegenerateIndices);
        }
        draw->vertexCount = draw->maxIndex - draw->minIndex + 1;
        vertexIndexOffset += draw->minIndex;
    } else {
        draw->vertexCount = hash::d3dVertexCount(primitive, call.primitiveCount);
        draw->indexType = 0; // an undefined RasterBuffer reports VK_INDEX_TYPE_UINT16
    }
    draw->vertexIndexOffset = vertexIndexOffset;
    if (draw->vertexCount == 0) {
        return finish(CaptureStatus::NoVertices);
    }

    // ---- vertex streams (processVertices) ------------------------------------------------------
    bool colorAllowed = !m_config.ignoreAllVertexColorBakedLighting;
    if (colorAllowed && m_config.ignoreBakedLightingTexture && draw->texcoord.colorTextureStages[0] != kInvalidStage) {
        colorAllowed = !m_config.ignoreBakedLightingTexture(state.textures[draw->texcoord.colorTextureStages[0]]);
    }
    SliceRequest request;
    request.vertexIndexOffset = vertexIndexOffset;
    request.vertexCount = draw->vertexCount;
    request.texcoordIndex = draw->texcoord.texcoordIndex;
    request.colorAllowed = colorAllowed;
    const std::span<const tap::VertexElement> elements(state.elements, std::min(state.elementCount, tap::kMaxVertexElements));
    if (sliceVertexStreams(elements, streams, request, draw->vertices) != SliceStatus::Ok) {
        return finish(CaptureStatus::OutOfBounds);
    }
    if (!draw->vertices.position.defined()) {
        return finish(CaptureStatus::NoPosition);
    }
    draw->positionStride = draw->vertices.position.stride;

    // ---- computeHash: the synchronous components, then the job -----------------------------------
    const hash::HashRule rule = m_config.generationRule;
    HashJobInput job;
    job.rule = rule;
    job.sceneScale = m_config.sceneScale;
    job.vertexCount = draw->vertexCount;
    job.indices = draw->indices;
    job.position = draw->vertices.position;
    job.texcoord = draw->vertices.texcoord;
    if (draw->programmableVs && m_config.useVertexCapture && rule.test(hash::HashComponent::GeometryDescriptor) &&
        m_config.vertexShaderHash) {
        if (const std::optional<hash::Hash64> vs = m_config.vertexShaderHash(state)) {
            job.precomputed[hash::HashComponent::VertexShader] = *vs;
        }
    }
    if (rule.test(hash::HashComponent::GeometryDescriptor)) {
        job.precomputed[hash::HashComponent::GeometryDescriptor] =
            hash::hashGeometryDescriptor(draw->indexCount, draw->vertexCount, draw->indexType, draw->topology);
    }
    if (rule.test(hash::HashComponent::VertexLayout)) {
        const hash::VertexLayoutInput layout{layoutElement(draw->vertices.position), layoutElement(draw->vertices.normal),
                                             layoutElement(draw->vertices.texcoord), layoutElement(draw->vertices.color0)};
        job.precomputed[hash::HashComponent::VertexLayout] = hash::hashVertexLayoutStride(hash::vertexLayoutStride(layout));
    }
    const bool async = m_config.asyncJobs;
    draw->hashes = JobFuture<hash::GeometryHashes>::schedule([job = std::move(job)] { return runHashJob(job); }, async);

    if (m_config.computeBoundingBox) {
        draw->boundingBox = JobFuture<BoundingBox>::schedule(
            [position = draw->vertices.position, count = draw->vertexCount] { return computeBoundingBox(position, count); },
            async);
    }

    // ---- processSkinning ----------------------------------------------------------------------------
    SkinningInput skin;
    skin.programmableVs = draw->programmableVs;
    skin.vertexBlend = state.renderStates ? state.renderStates[kD3DRSVertexBlend] : 0;
    skin.indexedVertexBlendEnable = state.renderStates && state.renderStates[kD3DRSIndexedVertexBlendEnable] != 0;
    skin.vertices = &draw->vertices;
    skin.vertexCount = draw->vertexCount;
    skin.transforms = state.transforms;
    if (std::optional<SkinningJob> skinJob = prepareSkinning(skin)) {
        draw->skinning = JobFuture<SkinningData>::schedule(
            [skinJob = std::move(*skinJob)] { return runSkinningJob(skinJob); }, async);
    }
    return finish(CaptureStatus::Captured);
}

} // namespace fuse::relight::capture::geometry
