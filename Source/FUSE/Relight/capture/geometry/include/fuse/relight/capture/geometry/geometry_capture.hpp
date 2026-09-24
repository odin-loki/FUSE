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
// FUSE Relight RL-1.3: geometry extraction, Remix-compatible geometry hashing and skinning data
// for every draw the tap reports (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.1, Wave R1).
//
// GeometryCapture is an IRelightTap consumer. Per draw it reproduces the geometry half of
// dxvk-remix @0867d3c D3D9Rtx::internalPrepareDraw (src/d3d9/d3d9_rtx.cpp, MIT):
//   1. the draw context: DrawPrimitive (BaseVertexIndex = StartVertex), DrawIndexedPrimitive,
//      DrawPrimitiveUP / DrawIndexedPrimitiveUP (DXVK's UP upload: vertex data, zero padding to
//      GetUPBufferSize, then the indices, which Remix reads at GetUPDataSize - kept as upstream);
//   2. index rebasing (memoized per index buffer range, invalidated by buffer writes);
//   3. texcoordIndex selection (processTextures) and stream slicing by usage (processVertices);
//   4. the geometry hashes (computeHash), the bounding box (computeAxisAlignedBoundingBox) and the
//      fixed-function skinning data (processSkinning) as jobs on fuse::jobs::JobScheduler.
// It does not classify draws (RL-1.2) or decide what DXVK does with them: onDraw always returns
// DrawDecision::Raster. Consumers take the CapturedDraw records (sink or takeDraws()).
// Modifications Copyright (c) 2026 FUSE contributors (MIT).
#pragma once

#include <fuse/relight/capture/geometry/index_rebase.hpp>
#include <fuse/relight/capture/geometry/job_future.hpp>
#include <fuse/relight/capture/geometry/skinning.hpp>
#include <fuse/relight/capture/geometry/texcoord_select.hpp>
#include <fuse/relight/capture/geometry/vertex_streams.hpp>
#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace fuse::relight::capture::geometry {

struct GeometryCaptureConfig {
    /// rtx.geometryGenerationHashRuleString: which components are computed.
    hash::HashRule generationRule = hash::parseHashRule(hash::rules::kDefaultGenerationRuleString);
    /// rtx.geometryAssetHashRuleString: the mesh replacement key (CapturedDraw::assetHash).
    hash::HashRule assetRule = hash::parseHashRule(hash::rules::kDefaultAssetRuleString);
    float sceneScale = 1.0f;                        ///< rtx.sceneScale (legacy positions step)
    bool indexBufferMemoization = true;             ///< rtx.enableIndexBufferMemoization
    bool computeBoundingBox = true;                 ///< RtxOptions::needsMeshBoundingBox()
    bool useVertexCapture = true;                   ///< rtx.useVertexCapture (VS draws)
    bool ignoreAllVertexColorBakedLighting = false; ///< rtx.ignoreAllVertexColorBakedLighting
    bool allowCubemaps = false;                     ///< rtx.allowCubemaps
    bool asyncJobs = true;                          ///< false: jobs run inline in onDraw
    /// Keep a CPU copy of every buffer write, used when a binding has no CPU mapping.
    bool shadowBuffers = true;

    /// The vertexshader component for programmable-VS draws (needs shader analysis for the
    /// constant ranges: RL-1.6). Called in onDraw when the VS is programmable, useVertexCapture is
    /// on and the generation rule has geometrydescriptor (upstream's gate). Unset or nullopt: 0.
    std::function<std::optional<hash::Hash64>(const tap::DrawState&)> vertexShaderHash;
    /// Texture facts the capture cannot know by itself (RL-1.4 texture hashes and rtx.* lists).
    /// hashKnown: default = the texture had a level-0 upload or a copy from such a texture.
    std::function<bool(tap::ResourceId)> textureHashKnown;
    std::function<bool(tap::ResourceId)> isLightmapTexture;         ///< rtx.lightmapTextures
    std::function<bool(tap::ResourceId)> ignoreBakedLightingTexture; ///< rtx.ignoreBakedLightingTextures

    /// The rtx.* options above that this package owns (rule strings, memoization), resolved now.
    [[nodiscard]] static GeometryCaptureConfig fromOptions();
};

enum class CaptureStatus : std::uint32_t {
    Captured = 0,
    NoPosition,        ///< no POSITION[0] / POSITIONT[0] stream
    DegenerateIndices, ///< maxIndex == minIndex ("no triangles detected in index buffer")
    NoVertices,        ///< PrimitiveCount 0, or vertexCount == 0
    NoIndexData,       ///< indexed draw without readable index data
    OutOfBounds,       ///< the vertex or index window is not inside its buffer
};
[[nodiscard]] std::string_view captureStatusName(CaptureStatus status) noexcept;

/// AxisAlignedBoundingBox of the positions (SSE MINPS / MAXPS semantics).
struct BoundingBox {
    std::array<float, 3> minPos{};
    std::array<float, 3> maxPos{};
};

/// One draw as the geometry capture saw it (upstream RasterGeometry + the geometry futures).
struct CapturedDraw {
    std::uint64_t frame = 0;     ///< presents before the draw
    std::uint32_t drawIndex = 0; ///< draw number within the frame
    tap::DrawCallType call = tap::DrawCallType::DrawPrimitive;
    std::uint32_t primitiveType = 0;
    std::uint32_t primitiveCount = 0;
    CaptureStatus status = CaptureStatus::Captured;

    // RasterGeometry
    std::uint32_t indexCount = 0;  ///< 0 for non-indexed draws
    std::uint32_t vertexCount = 0;
    std::uint32_t minIndex = 0, maxIndex = 0;
    std::uint32_t topology = 0;    ///< VkPrimitiveTopology
    std::uint32_t indexType = 0;   ///< VkIndexType as RasterGeometry reports it (0 when non-indexed)
    std::uint32_t positionStride = 0;
    std::int64_t vertexIndexOffset = 0; ///< first vertex of the window (BaseVertexIndex + minIndex)
    RebasedIndicesPtr indices;          ///< rebased indices (indexed draws)
    bool indicesMemoized = false;       ///< served from the memoizer
    SlicedVertices vertices;
    TexcoordSelection texcoord;
    bool programmableVs = false;
    bool programmablePs = false;

    JobFuture<hash::GeometryHashes> hashes;      ///< valid when status == Captured
    JobFuture<BoundingBox> boundingBox;          ///< valid when captured and computeBoundingBox
    JobFuture<SkinningData> skinning;            ///< valid when the draw has FF skinning data

    [[nodiscard]] bool captured() const noexcept { return status == CaptureStatus::Captured; }
    /// DrawCallState::getHash(assetRule) ^ materialHash (waits for the hash job). 0 when not captured.
    [[nodiscard]] hash::Hash64 assetHash(hash::HashRule assetRule, hash::Hash64 materialHash = 0) const;
    /// The hash library's view of this draw (waits), for meshReplacementHashLegacy and tools.
    [[nodiscard]] hash::DrawGeometryHashes geometryHashes() const;
};
using CapturedDrawPtr = std::shared_ptr<const CapturedDraw>;

struct GeometryCaptureStats {
    std::uint64_t draws = 0;
    std::uint64_t captured = 0;
    std::uint64_t memoHits = 0;
    std::uint64_t memoMisses = 0;
    std::uint64_t streamBytesCopied = 0;
    std::uint64_t skinnedDraws = 0;
};

class GeometryCapture final : public tap::IRelightTap {
public:
    explicit GeometryCapture(GeometryCaptureConfig config = {});
    ~GeometryCapture() override;
    GeometryCapture(const GeometryCapture&) = delete;
    GeometryCapture& operator=(const GeometryCapture&) = delete;

    [[nodiscard]] const GeometryCaptureConfig& config() const noexcept { return m_config; }

    /// Where captured draws go. Without a sink they are kept until takeDraws().
    using DrawSink = std::function<void(const CapturedDrawPtr&)>;
    void setDrawSink(DrawSink sink);
    [[nodiscard]] std::vector<CapturedDrawPtr> takeDraws();

    /// The per-draw work of onDraw, without recording the result.
    [[nodiscard]] CapturedDrawPtr capture(const tap::DrawCall& call, const tap::DrawState& state);

    [[nodiscard]] GeometryCaptureStats stats() const;

    // IRelightTap
    void onTextureCreate(const tap::TextureDesc& desc) override;
    void onTextureUpload(const tap::TextureUpload& upload) override;
    void onTextureCopy(const tap::TextureCopy& copy) override;
    void onImageDestroy(const tap::ImageDestroy& destroy) override;
    void onBufferCreate(const tap::BufferDesc& desc) override;
    void onBufferWrite(const tap::BufferWrite& write) override;
    void onBufferDestroy(tap::ResourceId id) override;
    tap::DrawDecision onDraw(const tap::DrawCall& call, const tap::DrawState& state) override;
    void onPresent(const tap::FrameEvent& frame) override;

private:
    struct BufferInfo {
        tap::BufferKind kind = tap::BufferKind::Vertex;
        std::uint32_t size = 0;
        std::uint32_t format = 0;
        std::vector<std::uint8_t> shadow;
        IndexRangeMemoizer memo;
    };
    struct TextureInfo {
        std::uint32_t type = 0;
        bool hashKnown = false;
    };

    StageTexture stageTexture(tap::ResourceId id) const;
    void recordDraw(CapturedDrawPtr draw);

    GeometryCaptureConfig m_config;
    mutable std::mutex m_mutex;
    std::map<tap::ResourceId, BufferInfo> m_buffers;
    std::map<tap::ResourceId, TextureInfo> m_textures;
    std::uint64_t m_frame = 0;
    std::uint32_t m_drawInFrame = 0;
    GeometryCaptureStats m_stats;
    DrawSink m_sink;
    std::vector<CapturedDrawPtr> m_pending;
};

// ---- pieces shared with the jobs (exposed for the unit tests) -------------------------------------

/// The computeHash job: every generation-rule component that needs vertex or index data, over the
/// sliced attributes and the rebased indices (the same values as hash::computeDrawGeometryHashes).
struct HashJobInput {
    hash::HashRule rule;
    float sceneScale = 1.0f;
    std::uint32_t vertexCount = 0;
    RebasedIndicesPtr indices; ///< nullptr for non-indexed draws
    VertexAttribute position;
    VertexAttribute texcoord;
    hash::GeometryHashes precomputed; ///< geometrydescriptor, vertexlayout, vertexshader
};
[[nodiscard]] hash::GeometryHashes runHashJob(const HashJobInput& input);

/// The computeAxisAlignedBoundingBox job over `vertexCount` positions (3 floats each).
[[nodiscard]] BoundingBox computeBoundingBox(const VertexAttribute& position, std::uint32_t vertexCount) noexcept;

} // namespace fuse::relight::capture::geometry
