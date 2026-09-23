// FUSE Relight: Remix-compatible geometry hashes (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.1-4.1.2).
//
// Bit-exact with dxvk-remix @0867d3c: src/dxvk/rtx_render/rtx_hashing.{h,cpp},
// src/d3d9/d3d9_rtx_geometry.cpp (D3D9Rtx::computeHash), src/d3d9/d3d9_rtx.cpp
// (D3D9Rtx::prepareDrawGeometryForRT index rebasing) and src/dxvk/rtx_render/rtx_types.h
// (DrawCallState::getHash / RasterGeometry::getHashForRuleLegacy).
//
// Two levels of API:
//   * component functions that mirror the upstream functions one to one (for KATs and tools);
//   * computeDrawGeometryHashes(), which takes one D3D9 draw as the application issued it and
//     reproduces D3D9Rtx's preprocessing (index rebasing, vertex window, generation rule gating).
//
// Upstream behaviours kept on purpose (each has a test):
//   * the rule combiner seeds with the first *non-zero running value*, not the first selected
//     component: a leading zero component is skipped, and the next one is used as-is;
//   * non-indexed draws hash indexCount 0 and indexType 0 (VK_INDEX_TYPE_UINT16: an undefined
//     RasterBuffer's format reads as 0), not VK_INDEX_TYPE_NONE_KHR;
//   * an indexed draw without a texcoord stream still hashes texcoords: XXH3 of 0 bytes, seeded,
//     once per unique index; a non-indexed draw without texcoords hashes 0;
//   * the vertex-shader component is gated on the *geometrydescriptor* bit of the generation rule
//     (and on programmable VS + vertex capture), and hashes maxConstIndexB * 4 / 32 bool bytes;
//   * legacy positions use the SSE4.1 path (multiply by the reciprocal, floor, multiply), emulated
//     bit-exactly in integer arithmetic (so the caller's MXCSR does not matter); legacypositions0
//     is the running hash captured before vertex 20, so meshes with <= 20 vertices keep it at 0;
//   * upstream reads an uninitialised precombined slot for the legacy rules when the legacy
//     position field is 0 (GeometryHashes::precombine); FUSE returns the combiner's value and
//     reports the case through GeometryHashes::isRuleHashDefinedUpstream().
#pragma once

#include <fuse/relight/hash/d3d_types.hpp>
#include <fuse/relight/hash/xxh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::hash {

/// HashComponents, in upstream enum order (the rule combiner walks them in this order).
enum class HashComponent : std::uint32_t {
    Positions = 0,
    LegacyPositions0,
    LegacyPositions1,
    Texcoords,
    Indices,
    LegacyIndices,
    GeometryDescriptor,
    VertexLayout,
    VertexShader,
};
inline constexpr std::uint32_t kHashComponentCount = 9;

/// The rule-string token of a component ("positions", "legacypositions0", ...).
[[nodiscard]] std::string_view hashComponentName(HashComponent component) noexcept;

/// A set of hash components (upstream HashRule = Flags<HashComponents>).
struct HashRule {
    std::uint32_t bits = 0;

    [[nodiscard]] constexpr bool test(HashComponent c) const noexcept {
        return (bits >> std::uint32_t(c)) & 1u;
    }
    constexpr HashRule& set(HashComponent c) noexcept {
        bits |= 1u << std::uint32_t(c);
        return *this;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }
    [[nodiscard]] constexpr bool contains(HashRule other) const noexcept { return (bits & other.bits) == other.bits; }
    friend constexpr bool operator==(HashRule, HashRule) = default;
};

namespace rules {
inline constexpr HashRule kTopological{(1u << 4) | (1u << 6)};                 // indices, geometrydescriptor
inline constexpr HashRule kVertexData{(1u << 0) | (1u << 3) | (1u << 7) | (1u << 8)}; // positions, texcoords, vertexlayout, vertexshader
inline constexpr HashRule kFullGeometry{kTopological.bits | kVertexData.bits};
inline constexpr HashRule kLegacyAsset0{(1u << 1) | (1u << 5)};                // legacypositions0, legacyindices
inline constexpr HashRule kLegacyAsset1{(1u << 2) | (1u << 5)};                // legacypositions1, legacyindices
/// rtx.geometryAssetHashRuleString default: the mesh replacement key.
inline constexpr std::string_view kDefaultAssetRuleString = "positions,indices,geometrydescriptor";
/// rtx.geometryGenerationHashRuleString default: which components are computed at all.
inline constexpr std::string_view kDefaultGenerationRuleString =
    "positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader";
} // namespace rules

/// createRule(): "" -> empty; spaces removed; comma-separated tokens matched exactly
/// (case-sensitive) against the component names; unknown tokens ignored.
[[nodiscard]] HashRule parseHashRule(std::string_view ruleString);

/// The canonical rule string (component names in enum order, comma-separated).
[[nodiscard]] std::string formatHashRule(HashRule rule);

/// rule_id for the replacement DB (plan §4.1.5): XXH3_64bits of formatHashRule(rule).
[[nodiscard]] Hash64 hashRuleId(HashRule rule);

/// Upstream GeometryHashes: one value per component (0 = not computed).
struct GeometryHashes {
    std::array<Hash64, kHashComponentCount> fields{};

    [[nodiscard]] Hash64& operator[](HashComponent c) noexcept { return fields[std::uint32_t(c)]; }
    [[nodiscard]] Hash64 operator[](HashComponent c) const noexcept { return fields[std::uint32_t(c)]; }

    /// getHashForRuleImpl: walk the components in enum order; while the running value is 0 take the
    /// component's value as-is, afterwards combine with XXH64(&field, 8, running).
    [[nodiscard]] Hash64 hashForRule(HashRule rule) const noexcept;

    /// False only for the legacy rules when their legacy position field is 0: upstream then returns
    /// an uninitialised precombined value, so no replacement can depend on it.
    [[nodiscard]] bool isRuleHashDefinedUpstream(HashRule rule) const noexcept;
};

// ---- component functions (one per upstream function) -------------------------------------------

/// hashGeometryDescriptor: XXH3 chain over indexCount, vertexCount, topology, indexType (u32 each).
[[nodiscard]] Hash64 hashGeometryDescriptor(std::uint32_t indexCount, std::uint32_t vertexCount, std::uint32_t indexType,
                                            std::uint32_t topology) noexcept;

/// hashVertexLayout's final step: XXH3_64bits(&stride, sizeof(size_t) == 8).
[[nodiscard]] Hash64 hashVertexLayoutStride(std::uint64_t vertexStride) noexcept;

/// hashContiguousMemory: XXH3_64bits(data, size). Used for the indices component.
[[nodiscard]] Hash64 hashContiguousMemory(const void* data, std::size_t size) noexcept;

/// hashVertexRegionIndexed. `base` points at the element of vertex 0 of the region and `size` is
/// stride * vertexCount. With a non-empty `uniqueIndices`, chains XXH3_withSeed over
/// (base + i * stride, elementSize) for each index i; otherwise over every vertex in order.
/// base may be null when elementSize is 0 (an undefined stream).
[[nodiscard]] Hash64 hashVertexRegion(const std::uint8_t* base, std::size_t size, std::size_t stride,
                                      std::size_t elementSize, std::span<const std::uint32_t> uniqueIndices) noexcept;

/// deduplicateSortIndices: the sorted unique values of `indexCount` indices of `indexSize` bytes
/// (2 or 4), all <= maxIndexValue.
[[nodiscard]] std::vector<std::uint32_t> sortedUniqueIndices(const void* indices, std::uint32_t indexCount,
                                                             std::uint32_t indexSize, std::uint32_t maxIndexValue);

/// hashIndicesLegacy<T>: whole buffer when <= 1024 bytes, else every (bytes / 512)-th index.
[[nodiscard]] Hash64 hashIndicesLegacy(const void* indices, std::size_t indexCount, std::size_t indexSize) noexcept;

/// 0.01f * RtxOptions::getMeterToWorldUnitScale() = 0.01f * (100.f * sceneScale), in float.
[[nodiscard]] float legacyDiscreteStepSize(float sceneScale) noexcept;

/// IEEE-754 binary32 multiply on bit patterns (round to nearest even, no FTZ/DAZ), with x86 SSE
/// NaN rules: a NaN first operand is returned quieted, else a NaN second one; inf * 0 = 0xFFC00000.
[[nodiscard]] std::uint32_t mulF32Bits(std::uint32_t a, std::uint32_t b) noexcept;

/// ROUNDPS with _MM_FROUND_FLOOR on a bit pattern (exact; NaN/inf pass through; -0 stays -0).
[[nodiscard]] std::uint32_t floorF32Bits(std::uint32_t bits) noexcept;

/// One lane of discretize_SSE on bit patterns: mul(floor(mul(value, invStep)), step).
[[nodiscard]] std::uint32_t discretizeLegacyBits(std::uint32_t valueBits, std::uint32_t stepBits,
                                                 std::uint32_t invStepBits) noexcept;

/// One lane of discretize_SSE: floor(v * invStepSize) * stepSize, each step rounded to float
/// (_mm_mul_ps, _mm_round_ps(_MM_FROUND_FLOOR), _mm_mul_ps). invStepSize must be 1.f / stepSize.
/// Computed in integer arithmetic, so the caller's MXCSR (FTZ/DAZ) does not matter.
[[nodiscard]] float discretizeLegacy(float value, float stepSize, float invStepSize) noexcept;

/// hashRegionLegacy (SSE4.1 path). Reads 3 floats (12 bytes) at base + k * stride for every vertex
/// k with k * stride < size, so those 12 bytes must be readable even for narrower position formats.
/// `h0` and `h1` are in/out (upstream passes the GeometryHashes fields, normally 0).
void hashPositionsLegacy(const std::uint8_t* base, std::size_t size, std::size_t stride, float stepSize, Hash64& h0,
                         Hash64& h1) noexcept;

/// The vertex-shader component: XXH3(bytecode), then seeded XXH3 over maxConstIndexF * 16 float
/// constant bytes, maxConstIndexI * 16 int constant bytes and maxConstIndexB * 4 / 32 bool bytes.
[[nodiscard]] Hash64 hashVertexShader(std::span<const std::uint8_t> bytecode, const void* floatConstants,
                                      std::uint32_t maxConstIndexF, const void* intConstants, std::uint32_t maxConstIndexI,
                                      const void* boolConstants, std::uint32_t maxConstIndexB) noexcept;

/// What hashVertexLayout needs to know about one vertex stream element.
struct VertexLayoutElement {
    bool defined = false;
    std::uint32_t stream = 0;   // identity of the buffer slice (same D3D stream = same slice)
    std::uint32_t stride = 0;   // stream stride in bytes
    std::uint32_t vkFormat = 0; // declTypeVkFormat(element type)
};

struct VertexLayoutInput {
    VertexLayoutElement position; // must be defined
    VertexLayoutElement normal;
    VertexLayoutElement texcoord;
    VertexLayoutElement color0;
};

/// RasterGeometry::isVertexDataInterleaved() && areFormatsGpuFriendly() ? position stride
/// : RtxGeometryUtils::computeOptimalVertexStride() (12 + 12 normal + 8 texcoord + 4 color0).
[[nodiscard]] std::uint64_t vertexLayoutStride(const VertexLayoutInput& layout) noexcept;

// ---- one draw call ------------------------------------------------------------------------------

/// A vertex element as the application bound it.
struct DrawVertexElement {
    /// Address of this element for vertex 0 of the draw: stream data + stream offset
    /// + BaseVertexIndex * stride + element offset. Null = element not present.
    const std::uint8_t* data = nullptr;
    std::uint32_t stride = 0;
    D3DDeclType type = D3DDeclType::Unused;
    std::uint32_t stream = 0;

    [[nodiscard]] bool defined() const noexcept { return data != nullptr; }
};

/// Vertex-shader state, used only for programmable-VS draws with vertex capture.
struct DrawVertexShader {
    std::span<const std::uint8_t> bytecode;
    const void* floatConstants = nullptr; // D3D9 vsConsts.fConsts (16 bytes per register)
    std::uint32_t maxConstIndexF = 0;
    const void* intConstants = nullptr; // vsConsts.iConsts (16 bytes per register)
    std::uint32_t maxConstIndexI = 0;
    const void* boolConstants = nullptr; // vsConsts.bConsts (u32 words)
    std::uint32_t maxConstIndexB = 0;
};

/// One DrawPrimitive / DrawIndexedPrimitive as the application issued it.
struct DrawGeometryInput {
    D3DPrimitiveType primitiveType = D3DPrimitiveType::TriangleList;
    std::uint32_t primitiveCount = 0;
    /// Uint16 / Uint32 for indexed draws (indexData points at StartIndex), NoneKhr for non-indexed.
    IndexType indexType = IndexType::NoneKhr;
    const void* indexData = nullptr;

    DrawVertexElement position; // POSITION[0] or POSITIONT[0]; required
    DrawVertexElement texcoord; // TEXCOORD[m_texcoordIndex]
    DrawVertexElement normal;   // NORMAL[0] (vertex layout only)
    DrawVertexElement color0;   // COLOR[0] unless baked lighting is ignored (vertex layout only)

    /// UseProgrammableVS() && useVertexCapture(): enables the vertexshader component.
    bool programmableVsWithCapture = false;
    DrawVertexShader vertexShader;
};

/// The hashes of one draw plus the RasterGeometry fields the legacy key and the tools need.
struct DrawGeometryHashes {
    GeometryHashes hashes;
    std::uint32_t indexCount = 0;  // RasterGeometry::indexCount (0 for non-indexed draws)
    std::uint32_t vertexCount = 0; // maxIndex - minIndex + 1, or GetVertexCount for non-indexed
    std::uint32_t minIndex = 0;
    std::uint32_t maxIndex = 0;
    std::uint32_t topology = 0;       // VkPrimitiveTopology
    std::uint32_t indexType = 0;      // VkIndexType as RasterGeometry reports it (0 when non-indexed)
    std::uint32_t positionStride = 0; // position stream stride
    std::vector<std::uint8_t> rebasedIndices; // indices - minIndex, original width (indexed only)
};

/// D3D9Rtx::prepareDrawGeometryForRT + computeHash. Returns nullopt where upstream skips the draw
/// (no position element, maxIndex == minIndex, or zero vertices). `sceneScale` is rtx.sceneScale.
[[nodiscard]] std::optional<DrawGeometryHashes> computeDrawGeometryHashes(const DrawGeometryInput& draw,
                                                                         HashRule generationRule, float sceneScale = 1.0f);

/// DrawCallState::getHash(rule): the mesh replacement key ("mesh_<hash>") = rule hash ^ material hash.
[[nodiscard]] Hash64 meshReplacementHash(const GeometryHashes& hashes, HashRule assetRule, Hash64 materialHash) noexcept;

/// DrawCallState::getHashLegacy(rule): the legacy rule hash chained with XXH64 over indexCount,
/// vertexCount, topology, position stride and index type (u32 each), then ^ material hash.
/// Upstream tries rules::kLegacyAsset0 / kLegacyAsset1 only when the generation rule contains them.
[[nodiscard]] Hash64 meshReplacementHashLegacy(const DrawGeometryHashes& draw, HashRule legacyRule,
                                               Hash64 materialHash) noexcept;

} // namespace fuse::relight::hash
