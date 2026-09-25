/*
* Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
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
// Test oracle for FUSE Relight RL-0.5: the hashing code of dxvk-remix @0867d3c kept as close to
// verbatim as a standalone build allows, used to derive the known-answer tables (kat_vectors.txt)
// and to cross-check fuse_relight_hash live. Ported from:
//   src/dxvk/rtx_render/rtx_hashing.{h,cpp}          (HashComponents, rules, GeometryHashes,
//                                                     createRule, hash* functions, SSE discretize)
//   src/d3d9/d3d9_rtx_geometry.cpp                   (deduplicateSortIndices, hashGeometryData,
//                                                     D3D9Rtx::computeHash)
//   src/d3d9/d3d9_rtx.cpp                            (copyIndices, prepareDrawGeometryForRT)
//   src/dxvk/rtx_render/rtx_types.h                  (isVertexDataInterleaved, areFormatsGpuFriendly,
//                                                     getHashForRuleLegacy, DrawCallState::getHash)
//   src/dxvk/rtx_render/rtx_geometry_utils.cpp       (computeOptimalVertexStride)
//   src/dxvk/rtx_render/rtx_lights.cpp               (RtLightShaping::getHash, updateCachedHash)
//   src/dxvk/rtx_render/rtx_utils.h                  (hashToString)
//   src/dxvk/rtx_render/rtx_mod_usd.cpp              (getNamedHash)
//   src/dxvk/rtx_render/rtx_option.cpp               (fillHashVector)
//   src/d3d9/d3d9_common_texture.h                   (D3D9_COMMON_TEXTURE_DESC::CalculateHash)
// FUSE changes: DXVK buffers, futures, logging and ref counting removed; Vector types, Flags and
// the D3D enums reduced to what the hashes read; the non-SSE fallbacks dropped (x86-64 only).
#include "upstream_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#define XXH_FORCE_MEMORY_ACCESS 0 // aliasing-safe reads (see hash/src/xxh.cpp)
#define XXH_INLINE_ALL
#include <xxhash.h>

#if defined(__x86_64__) || defined(_M_X64)
#define FUSE_ORACLE_X86 1
#include <immintrin.h>
#else
#define FUSE_ORACLE_X86 0
#endif

#if defined(__GNUC__)
#define ORACLE_SSE41 __attribute__((target("sse4.1")))
#else
#define ORACLE_SSE41
#endif

namespace fuse::relight::hash::test {

namespace dxvk {

// ---- rtx_hashing.h ---------------------------------------------------------------------------------

static constexpr XXH64_hash_t kEmptyHash = 0;

enum class HashComponents : uint32_t {
    VertexPosition = 0,
    LegacyPositions0,
    LegacyPositions1,
    VertexTexcoord,
    Indices,
    LegacyIndices,
    GeometryDescriptor,
    VertexLayout,
    VertexShader,
    Count
};

// Flags<HashComponents>, reduced.
struct HashRule {
    uint32_t bits = 0;
    HashRule() = default;
    HashRule(uint32_t b) : bits(b) {}
    bool test(HashComponents c) const { return (bits >> uint32_t(c)) & 1u; }
    void set(HashComponents c) { bits |= 1u << uint32_t(c); }
    uint32_t raw() const { return bits; }
};

namespace rules {
const uint32_t TopologicalHash = (1 << (uint32_t) HashComponents::Indices) | (1 << (uint32_t) HashComponents::GeometryDescriptor);

const uint32_t VertexDataHash = (1 << (uint32_t) HashComponents::VertexPosition) | (1 << (uint32_t) HashComponents::VertexTexcoord) |
                                (1 << (uint32_t) HashComponents::VertexLayout) | (1 << (uint32_t) HashComponents::VertexShader);

const uint32_t FullGeometryHash = VertexDataHash | TopologicalHash;

const uint32_t LegacyAssetHash0 = (1 << (uint32_t) HashComponents::LegacyPositions0) | (1 << (uint32_t) HashComponents::LegacyIndices);

const uint32_t LegacyAssetHash1 = (1 << (uint32_t) HashComponents::LegacyPositions1) | (1 << (uint32_t) HashComponents::LegacyIndices);
const uint32_t Total = 5;
} // namespace rules

struct HashQuery {
    uint8_t* pBase;     // base pointer of the memory region to hash
    size_t size;        // length of the memory in bytes
    size_t stride;      // byte stride elements within buffer
    size_t elementSize; // byte stride of the specific elements to hash
};

// Upstream leaves `precombined` uninitialised. The oracle fills it with a marker and reports
// whether a legacy slot was actually written (precombinedValid).
static constexpr XXH64_hash_t kUninitialisedMarker = 0xDEADDEADDEADDEADull;

struct GeometryHashes {
    GeometryHashes() {
        memset(&fields[0], kEmptyHash, sizeof(fields));
        for (auto& p : precombined) {
            p = kUninitialisedMarker;
        }
    }

    const XXH64_hash_t& operator[](const HashComponents& field) const { return fields[(uint32_t) field]; }
    XXH64_hash_t& operator[](const HashComponents& field) { return fields[(uint32_t) field]; }

    void precombine() {
        precombined[0] = getHashForRuleImpl(rules::TopologicalHash);
        precombined[1] = getHashForRuleImpl(rules::VertexDataHash);
        precombined[2] = getHashForRuleImpl(rules::FullGeometryHash);
        if (operator[](HashComponents::LegacyPositions0) != kEmptyHash) {
            precombined[3] = getHashForRuleImpl(rules::LegacyAssetHash0);
        }
        if (operator[](HashComponents::LegacyPositions1) != kEmptyHash) {
            precombined[4] = getHashForRuleImpl(rules::LegacyAssetHash1);
        }
    }

    XXH64_hash_t getHashForRule(const HashRule& rule) const {
        switch (rule.raw()) {
        case rules::TopologicalHash: return precombined[0];
        case rules::VertexDataHash: return precombined[1];
        case rules::FullGeometryHash: return precombined[2];
        case rules::LegacyAssetHash0: return precombined[3];
        case rules::LegacyAssetHash1: return precombined[4];
        }
        return getHashForRuleImpl(rule);
    }

    // Legacy hash combiner
    XXH64_hash_t getHashForRuleImpl(const HashRule& rule) const {
        XXH64_hash_t hashResult = kEmptyHash;
        for (uint32_t i = 0; i < (uint32_t) HashComponents::Count; i++) {
            const HashComponents component = (HashComponents) i;

            if (rule.test(component)) {
                if (hashResult == kEmptyHash)
                    // For the first entry, we use the hash directly
                    hashResult = fields[i];
                else
                    // For all other entries, we combine the hash via seeding
                    hashResult = XXH64(&(fields[i]), sizeof(XXH64_hash_t), hashResult);
            }
        }
        return hashResult;
    }

    XXH64_hash_t fields[static_cast<uint32_t>(HashComponents::Count)];
    XXH64_hash_t precombined[rules::Total];
};

// ---- rtx_hashing.cpp ------------------------------------------------------------------------------

const static char* HashComponentNames[] = {
    "positions", "legacypositions0", "legacypositions1", "texcoords", "indices", "legacyindices", "geometrydescriptor",
    "vertexlayout", "vertexshader",
};
static_assert((sizeof(HashComponentNames) / sizeof(char*)) == (size_t) HashComponents::Count);

namespace str {
std::vector<std::string> split(std::string value, const char delimiter = ',') {
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string s;
    while (std::getline(ss, s, delimiter)) {
        result.push_back(s);
    }
    return result;
}
} // namespace str

HashRule createRule(const char* /*rulesetName*/, const std::string& ruleset) {
    HashRule ruleOutput;

    if (ruleset == "") {
        return 0;
    }

    // Remove any empy spaces in case the tokens have spaces occuring after delimiters
    std::string rulesetNoSpaces = ruleset;
    rulesetNoSpaces.erase(std::remove(rulesetNoSpaces.begin(), rulesetNoSpaces.end(), ' '), rulesetNoSpaces.end());

    const std::vector<std::string> tokens = dxvk::str::split(rulesetNoSpaces);
    for (auto&& token : tokens) {
        for (uint32_t i = 0; i < (uint32_t) HashComponents::Count; i++) {
            if (token == HashComponentNames[(uint32_t) i]) {
                ruleOutput.set((HashComponents) i);
            }
        }
    }

    return ruleOutput;
}

XXH64_hash_t hashGeometryDescriptor(const uint32_t indexCount, const uint32_t vertexCount, const uint32_t indexType,
                                    const uint32_t topology) {
    // Note: Only information relating to how the geometry is structured should be included here.
    XXH64_hash_t h = XXH3_64bits_withSeed(&indexCount, sizeof(indexCount), 0);
    h = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), h);
    h = XXH3_64bits_withSeed(&topology, sizeof(topology), h);
    return XXH3_64bits_withSeed(&indexType, sizeof(indexType), h);
}

XXH64_hash_t hashContiguousMemory(const void* pData, size_t byteSize) {
    return XXH3_64bits(pData, byteSize);
}

template<typename T>
XXH64_hash_t hashVertexRegionIndexed(const HashQuery& query, const std::vector<T>& uniqueIndices) {
    XXH64_hash_t result = 0;

    constexpr bool hasIndices = std::is_same<T, uint16_t>::value || std::is_same<T, uint32_t>::value;

    if (hasIndices && uniqueIndices.size() > 0) {
        for (const T idx : uniqueIndices) {
            const uint8_t* pData = (query.pBase + idx * query.stride);
            result = XXH3_64bits_withSeed(pData, query.elementSize, result);
        }
    } else {
        for (uint32_t i = 0; i < query.size; i += query.stride) {
            const uint8_t* pData = (query.pBase + i);
            result = XXH3_64bits_withSeed(pData, query.elementSize, result);
        }
    }

    return result;
}

constexpr static uint32_t MaxGeomHashSize = 512; // 512b - this is a performance optimization

#if FUSE_ORACLE_X86
#define ROUNDING_EXEPTIONS_MASK _MM_FROUND_NO_EXC
ORACLE_SSE41 inline __m128 discretize_SSE(const float* in, __m128 stepSize, __m128 invStepSize) {
    // Load the input data with mm_set_ps because it is likely not aligned to 16 bytes
    __m128 val = _mm_set_ps(0.f, in[2], in[1], in[0]);
    // Calculate: round(val * invStepSize) * stepSize
    val = _mm_mul_ps(val, invStepSize);
    val = _mm_round_ps(val, _MM_FROUND_FLOOR | ROUNDING_EXEPTIONS_MASK);
    val = _mm_mul_ps(val, stepSize);
    return val;
}
#endif

template<typename T>
XXH64_hash_t hashIndicesLegacy(const void* pIndexData, const size_t indexCount) {
    XXH64_hash_t indexHash = 0;

    if (indexCount * sizeof(T) <= MaxGeomHashSize * 2) {
        // Short buffer
        indexHash = XXH3_64bits(pIndexData, indexCount * sizeof(T));
    } else {
        // Long buffer, sample indices throughout:
        uint32_t step = indexCount * sizeof(T) / MaxGeomHashSize;
        for (uint32_t i = 0; i < indexCount; i += step) {
            indexHash = XXH3_64bits_withSeed((uint8_t*) pIndexData + i * sizeof(T), sizeof(T), indexHash);
        }
    }
    return indexHash;
}

// RtxOptions::getMeterToWorldUnitScale() = 100.f * sceneScale()
float g_sceneScale = 1.f;
float getMeterToWorldUnitScale() { return 100.f * g_sceneScale; }

#if FUSE_ORACLE_X86
ORACLE_SSE41 void hashRegionLegacy(const HashQuery& query, XXH64_hash_t& h0, XXH64_hash_t& h1) {
    const float discreteStepSize = 0.01f * getMeterToWorldUnitScale();

    const uint32_t dataToHash = query.size;
    const uint32_t kInitialHashVertexCount = 20;
    const uint32_t dataForLegacyHash = std::min(query.size, kInitialHashVertexCount * query.stride);

    // (sse41supported path; the oracle only runs where SSE4.1 is present)
    _mm_prefetch((char const*) query.pBase, _MM_HINT_NTA);
    __m128 stepSize = _mm_set1_ps(discreteStepSize);
    __m128 invStepSize = _mm_set1_ps(1.f / discreteStepSize);
    for (uint32_t i = 0; i < dataToHash; i += query.stride) {
        // Save the legacy hash upon reaching 20 vertices (or less)
        if (i == dataForLegacyHash)
            h0 = h1;
        // Discretize
        __m128 vPos = discretize_SSE((const float*) (query.pBase + i), stepSize, invStepSize);
        // Hash the result
        h1 = XXH3_64bits_withSeed(&vPos, sizeof(float) * 3, h1);
    }
}

ORACLE_SSE41 uint32_t discretizeOne(uint32_t bits, float sceneScale) {
    g_sceneScale = sceneScale;
    const float discreteStepSize = 0.01f * getMeterToWorldUnitScale();
    float in[3];
    memcpy(&in[0], &bits, 4);
    in[1] = in[2] = 0.f;
    const __m128 v = discretize_SSE(in, _mm_set1_ps(discreteStepSize), _mm_set1_ps(1.f / discreteStepSize));
    alignas(16) float out[4];
    _mm_store_ps(out, v);
    uint32_t r;
    memcpy(&r, &out[0], 4);
    return r;
}
#endif

// ---- d3d9_rtx_geometry.cpp / d3d9_rtx.cpp ----------------------------------------------------------

typedef int NoIndices;

template<typename T>
void deduplicateSortIndices(const void* pIndexData, const size_t indexCount, const uint32_t maxIndexValue, std::vector<T>& uniqueIndicesOut) {
    // We know there will be at most, this many unique indices
    const uint32_t indexRange = maxIndexValue + 1;

    // Initialize all to 0
    uniqueIndicesOut.resize(indexRange, (T) 0);

    // Use memory as a bin table for index data
    for (uint32_t i = 0; i < indexCount; i++) {
        const T& index = ((T*) pIndexData)[i];
        assert(index <= maxIndexValue);
        uniqueIndicesOut[index] = 1;
    }

    // Repopulate the bins with contiguous index values
    uint32_t uniqueIndexCount = 0;
    for (uint32_t i = 0; i < indexRange; i++) {
        if (uniqueIndicesOut[i])
            uniqueIndicesOut[uniqueIndexCount++] = i;
    }

    // Remove any unused entries
    uniqueIndicesOut.resize(uniqueIndexCount);
}

namespace VertexRegions {
enum Type : uint32_t { Position = 0, Texcoord, Count };
}

#if FUSE_ORACLE_X86
template<typename T>
void hashGeometryData(const HashRule& globalHashRule, const size_t indexCount, const uint32_t maxIndexValue, const void* pIndexData,
                      const HashQuery vertexRegions[VertexRegions::Count], GeometryHashes& hashesOut) {
    std::vector<T> uniqueIndices(0);
    if constexpr (!std::is_same<T, NoIndices>::value) {
        deduplicateSortIndices(pIndexData, indexCount, maxIndexValue, uniqueIndices);

        if (globalHashRule.test(HashComponents::Indices)) {
            hashesOut[HashComponents::Indices] = hashContiguousMemory(pIndexData, indexCount * sizeof(T));
        }

        if (globalHashRule.test(HashComponents::LegacyIndices)) {
            hashesOut[HashComponents::LegacyIndices] = hashIndicesLegacy<T>(pIndexData, indexCount);
        }
    }

    // Do vertex based rules (componentToRegionMap: positions -> Position, texcoords -> Texcoord)
    for (uint32_t i = 0; i < (uint32_t) HashComponents::Count; i++) {
        const HashComponents& component = (HashComponents) i;
        if (globalHashRule.test(component) &&
            (component == HashComponents::VertexPosition || component == HashComponents::VertexTexcoord)) {
            const VertexRegions::Type region =
                component == HashComponents::VertexPosition ? VertexRegions::Position : VertexRegions::Texcoord;
            hashesOut[component] = hashVertexRegionIndexed(vertexRegions[(uint32_t) region], uniqueIndices);
        }
    }

    if (globalHashRule.test(HashComponents::LegacyPositions0) || globalHashRule.test(HashComponents::LegacyPositions1)) {
        hashRegionLegacy(vertexRegions[VertexRegions::Position], hashesOut[HashComponents::LegacyPositions0],
                         hashesOut[HashComponents::LegacyPositions1]);
    }
}
#endif

// ---- rtx_types.h / rtx_geometry_utils.cpp: vertex layout -------------------------------------------

constexpr uint32_t VK_FORMAT_R32_UINT = 98, VK_FORMAT_R32G32_SFLOAT = 103, VK_FORMAT_R32G32B32_SFLOAT = 106,
                   VK_FORMAT_R32G32B32A32_SFLOAT = 109, VK_FORMAT_B8G8R8A8_UNORM = 44;

struct RasterBuffer {
    bool m_defined = false;
    uint32_t m_slice = 0; // identity of the DxvkBufferSlice (one per D3D stream in processVertices)
    uint32_t m_stride = 0;
    uint32_t m_format = 0;
    bool defined() const { return m_defined; }
    bool matches(const RasterBuffer& o) const { return m_slice == o.m_slice; }
    uint32_t stride() const { return m_stride; }
    uint32_t vertexFormat() const { return m_format; }
};

struct RasterGeometry {
    RasterBuffer positionBuffer, normalBuffer, texcoordBuffer, color0Buffer;

    bool isVertexDataInterleaved() const {
        if (normalBuffer.defined() && (!positionBuffer.matches(normalBuffer) || positionBuffer.stride() != normalBuffer.stride()))
            return false;

        if (texcoordBuffer.defined() && (!positionBuffer.matches(texcoordBuffer) || positionBuffer.stride() != texcoordBuffer.stride()))
            return false;

        if (color0Buffer.defined() && (!positionBuffer.matches(color0Buffer) || positionBuffer.stride() != color0Buffer.stride()))
            return false;

        return true;
    }

    bool areFormatsGpuFriendly() const {
        if (positionBuffer.vertexFormat() != VK_FORMAT_R32G32B32_SFLOAT && positionBuffer.vertexFormat() != VK_FORMAT_R32G32B32A32_SFLOAT)
            return false;

        if (normalBuffer.defined() && (normalBuffer.vertexFormat() != VK_FORMAT_R32G32B32_SFLOAT &&
                                       normalBuffer.vertexFormat() != VK_FORMAT_R32G32B32A32_SFLOAT &&
                                       normalBuffer.vertexFormat() != VK_FORMAT_R32_UINT))
            return false;

        if (texcoordBuffer.defined() && (texcoordBuffer.vertexFormat() != VK_FORMAT_R32G32_SFLOAT &&
                                         texcoordBuffer.vertexFormat() != VK_FORMAT_R32G32B32_SFLOAT &&
                                         texcoordBuffer.vertexFormat() != VK_FORMAT_R32G32B32A32_SFLOAT))
            return false;

        if (color0Buffer.defined() && (color0Buffer.vertexFormat() != VK_FORMAT_B8G8R8A8_UNORM))
            return false;

        return true;
    }
};

size_t computeOptimalVertexStride(const RasterGeometry& input, bool forceNormals = false) {
    // Calculate stride
    size_t stride = sizeof(float) * 3; // position is the minimum

    if (input.normalBuffer.defined() || forceNormals) {
        stride += sizeof(float) * 3;
    }

    if (input.texcoordBuffer.defined()) {
        stride += sizeof(float) * 2;
    }

    if (input.color0Buffer.defined()) {
        stride += sizeof(uint32_t);
    }

    return stride;
}

XXH64_hash_t hashVertexLayout(const RasterGeometry& input) {
    // Upstream declares this `const size_t` and hashes sizeof(size_t) bytes. Remix only ships for
    // x64, so the contract is the 8-byte little-endian stride; a literal size_t would hash only 4
    // bytes on a 32-bit build of this oracle and disagree with the (x64-generated) KAT table.
    const uint64_t vertexStride = (input.isVertexDataInterleaved() && input.areFormatsGpuFriendly())
                                      ? input.positionBuffer.stride()
                                      : computeOptimalVertexStride(input);
    static_assert(sizeof(vertexStride) == 8, "Remix hashes the x64 size_t stride: 8 bytes on every target");
    return XXH3_64bits(&vertexStride, sizeof(vertexStride));
}

// ---- rtx_lights.cpp --------------------------------------------------------------------------------

struct Vector2 {
    float x, y;
};
struct Vector3 {
    float data[3];
    const float& operator[](int i) const { return data[i]; }
};

enum class RtLightType { Sphere = 0, Rect = 1, Disk = 2, Cylinder = 3, Distant = 4 };

struct RtLightShaping {
    uint32_t m_enabled;
    Vector3 m_direction;
    float m_cosConeAngle;
    float m_coneSoftness;
    float m_focusExponent;

    XXH64_hash_t getHash() const {
        XXH64_hash_t h = 0;

        if (m_enabled) {
            h = XXH64(&m_direction[0], sizeof(m_direction), h);
            h = XXH64(&m_cosConeAngle, sizeof(m_cosConeAngle), h);
            h = XXH64(&m_coneSoftness, sizeof(m_coneSoftness), h);
            h = XXH64(&m_focusExponent, sizeof(m_focusExponent), h);
        }

        return h;
    }
};

XXH64_hash_t sphereHash(const Vector3& m_position, float m_radius, const RtLightShaping& m_shaping) {
    XXH64_hash_t h = (XXH64_hash_t) RtLightType::Sphere;
    h = XXH64(&m_position[0], sizeof(m_position), h);
    h = XXH64(&m_radius, sizeof(m_radius), h);
    h = XXH64(&h, sizeof(h), m_shaping.getHash());
    return h;
}

XXH64_hash_t rectHash(const Vector3& m_position, const Vector2& m_dimensions, const Vector3& m_xAxis, const Vector3& m_yAxis,
                      const Vector3& m_direction, const RtLightShaping& m_shaping) {
    XXH64_hash_t h = (XXH64_hash_t) RtLightType::Rect;
    h = XXH64(&m_position[0], sizeof(m_position), h);
    h = XXH64(&m_dimensions, sizeof(m_dimensions), h);
    h = XXH64(&m_xAxis[0], sizeof(m_xAxis), h);
    h = XXH64(&m_yAxis[0], sizeof(m_yAxis), h);
    h = XXH64(&m_direction[0], sizeof(m_direction), h);
    h = XXH64(&h, sizeof(h), m_shaping.getHash());
    return h;
}

XXH64_hash_t diskHash(const Vector3& m_position, const Vector2& m_halfDimensions, const Vector3& m_xAxis, const Vector3& m_yAxis,
                      const Vector3& m_direction, const RtLightShaping& m_shaping) {
    XXH64_hash_t h = (XXH64_hash_t) RtLightType::Disk;
    h = XXH64(&m_position[0], sizeof(m_position), h);
    h = XXH64(&m_halfDimensions, sizeof(m_halfDimensions), h);
    h = XXH64(&m_xAxis[0], sizeof(m_xAxis), h);
    h = XXH64(&m_yAxis[0], sizeof(m_yAxis), h);
    h = XXH64(&m_direction[0], sizeof(m_direction), h);
    h = XXH64(&h, sizeof(h), m_shaping.getHash());
    return h;
}

XXH64_hash_t cylinderHash(const Vector3& m_position, float m_radius, const Vector3& m_axis, float m_axisLength) {
    XXH64_hash_t h = (XXH64_hash_t) RtLightType::Cylinder;
    h = XXH64(&m_position[0], sizeof(m_position), h);
    h = XXH64(&m_radius, sizeof(m_radius), h);
    h = XXH64(&m_axis[0], sizeof(m_axis), h);
    h = XXH64(&m_axisLength, sizeof(m_axisLength), h);
    return h;
}

XXH64_hash_t distantHash(const Vector3& m_direction, float m_halfAngle) {
    XXH64_hash_t h = (XXH64_hash_t) RtLightType::Distant;
    h = XXH64(&m_direction[0], sizeof(m_direction), h);
    h = XXH64(&m_halfAngle, sizeof(m_halfAngle), h);
    return h;
}

// ---- d3d9_common_texture.h -------------------------------------------------------------------------

typedef uint32_t UINT;
typedef uint32_t DWORD;
enum class D3D9Format : uint32_t {};
enum D3DPOOL : uint32_t {};
enum D3DMULTISAMPLE_TYPE : uint32_t {};

struct D3D9_COMMON_TEXTURE_DESC {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT ArraySize;
    UINT MipLevels;
    DWORD Usage;
    D3D9Format Format;
    D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSample;
    DWORD MultisampleQuality;
    bool Discard;
    bool IsBackBuffer;
    bool IsAttachmentOnly;
    // NV-DXVK start: stable descriptor hashing for identifying render targets.
    // Forcing the extra padding byte at the end of the struct to be 0 initialized.  This is required for a stable hash.
    unsigned char padding = 0;

    XXH64_hash_t CalculateHash() const {
        static_assert(sizeof(D3D9_COMMON_TEXTURE_DESC) == 44);
        return XXH3_64bits(this, sizeof(D3D9_COMMON_TEXTURE_DESC));
    }
    // NV-DXVK end
};

// ---- rtx_utils.h / rtx_mod_usd.cpp / rtx_option.cpp -------------------------------------------------

inline const std::string hashToString(XXH64_hash_t hash) {
    // Two Hex Digits per byte
    constexpr uint8_t kNumHexits = sizeof(hash) * 2;
    std::stringstream ss;
    ss << std::uppercase << std::setfill('0') << std::setw(kNumHexits) << std::hex << hash;
    return ss.str();
}

XXH64_hash_t getNamedHash(const std::string& name, const char* prefix, const size_t len) {
    if (name.compare(0, len, prefix) == 0) {
        // is a mesh replacement.
        return std::strtoull(name.c_str() + len, nullptr, 16);
    } else {
        // Not a mesh replacements
        return 0;
    }
}

} // namespace dxvk

// ---- oracle entry points ---------------------------------------------------------------------------

namespace {

std::string dec(uint64_t v) {
    return std::to_string(v);
}

float bitsToFloat(uint32_t b) {
    return std::bit_cast<float>(b);
}

} // namespace

bool oracleAvailable() {
#if FUSE_ORACLE_X86 && defined(__GNUC__)
    return __builtin_cpu_supports("sse4.1");
#else
    return false;
#endif
}

std::optional<KeyValues> oracleCompute(std::string_view fn, const KeyValues& in) {
    using namespace dxvk;
    if (fn == "xxh64") {
        const Bytes d = reqBytes(in, "d");
        return KeyValues{{"out", hex64(XXH64(d.data(), d.size(), reqHex(in, "seed")))}};
    }
    if (fn == "xxh3") {
        const Bytes d = reqBytes(in, "d");
        const XXH64_hash_t h = req(in, "seed") == "none" ? XXH3_64bits(d.data(), d.size())
                                                        : XXH3_64bits_withSeed(d.data(), d.size(), reqHex(in, "seed"));
        return KeyValues{{"out", hex64(h)}};
    }
    if (fn == "geomdesc") {
        return KeyValues{
            {"out", hex64(hashGeometryDescriptor(reqU32(in, "ic"), reqU32(in, "vc"), reqU32(in, "it"), reqU32(in, "topo")))}};
    }
    if (fn == "region") {
        Bytes d = reqBytes(in, "d");
        const std::vector<uint32_t> uniq = parseListU32(req(in, "uniq"));
        const HashQuery q{d.empty() ? nullptr : d.data(), size_t(reqDec(in, "size")), size_t(reqDec(in, "stride")),
                          size_t(reqDec(in, "esize"))};
        return KeyValues{{"out", hex64(hashVertexRegionIndexed(q, uniq))}};
    }
    if (fn == "uniq") {
        const Bytes d = reqBytes(in, "d");
        std::vector<uint32_t> out;
        if (reqU32(in, "isize") == 2) {
            std::vector<uint16_t> u;
            deduplicateSortIndices(d.data(), reqU32(in, "count"), reqU32(in, "max"), u);
            out.assign(u.begin(), u.end());
        } else {
            deduplicateSortIndices(d.data(), reqU32(in, "count"), reqU32(in, "max"), out);
        }
        return KeyValues{{"out", listU32(out)}};
    }
    if (fn == "legidx") {
        const Bytes d = reqBytes(in, "d");
        const uint32_t count = reqU32(in, "count");
        const XXH64_hash_t h = reqU32(in, "isize") == 2 ? hashIndicesLegacy<uint16_t>(d.data(), count)
                                                        : hashIndicesLegacy<uint32_t>(d.data(), count);
        return KeyValues{{"out", hex64(h)}};
    }
    if (fn == "rule") {
        const Bytes s = reqBytes(in, "s");
        const HashRule rule = createRule("oracle", std::string(s.begin(), s.end()));
        std::string canon;
        for (uint32_t i = 0; i < uint32_t(HashComponents::Count); ++i) {
            if (rule.test(HashComponents(i))) {
                canon += (canon.empty() ? "" : ",");
                canon += HashComponentNames[i];
            }
        }
        const auto* p = reinterpret_cast<const uint8_t*>(canon.data());
        return KeyValues{{"out", dec(rule.raw())},
                         {"fmt", specHex(std::span(p, canon.size())).spec},
                         {"id", hex64(XXH3_64bits(canon.data(), canon.size()))}};
    }
    if (fn == "combine") {
        const auto parts = splitList(req(in, "f"), ',');
        GeometryHashes g;
        for (size_t i = 0; i < parts.size() && i < 9; ++i) {
            g.fields[i] = *parseU64(parts[i], 16);
        }
        g.precombine();
        const HashRule rule(reqU32(in, "rule"));
        XXH64_hash_t h = g.getHashForRule(rule);
        const bool defined = h != kUninitialisedMarker;
        if (!defined) {
            h = g.getHashForRuleImpl(rule); // what FUSE reports for an undefined upstream slot
        }
        return KeyValues{{"out", hex64(h)}, {"def", defined ? "1" : "0"}};
    }
    if (fn == "vshader") {
        const Bytes bc = reqBytes(in, "bc"), f = reqBytes(in, "f"), i = reqBytes(in, "i"), b = reqBytes(in, "b");
        const uint32_t maxConstIndexF = reqU32(in, "nf"), maxConstIndexI = reqU32(in, "ni"), maxConstIndexB = reqU32(in, "nb");
        XXH64_hash_t vertexShaderHash = XXH3_64bits(bc.data(), bc.size());
        vertexShaderHash = XXH3_64bits_withSeed(f.data(), maxConstIndexF * sizeof(float) * 4, vertexShaderHash);
        vertexShaderHash = XXH3_64bits_withSeed(i.data(), maxConstIndexI * sizeof(int) * 4, vertexShaderHash);
        vertexShaderHash = XXH3_64bits_withSeed(b.data(), maxConstIndexB * sizeof(uint32_t) / 32, vertexShaderHash);
        return KeyValues{{"out", hex64(vertexShaderHash)}};
    }
    if (fn == "vlayout") {
        RasterGeometry g;
        RasterBuffer* bufs[] = {&g.positionBuffer, &g.normalBuffer, &g.texcoordBuffer, &g.color0Buffer};
        const char* keys[] = {"p", "n", "t", "c"};
        for (int k = 0; k < 4; ++k) {
            const auto parts = splitList(req(in, keys[k]), ':');
            bufs[k]->m_defined = parts[0] != "0";
            bufs[k]->m_slice = uint32_t(*parseU64(parts[1], 10));
            bufs[k]->m_stride = uint32_t(*parseU64(parts[2], 10));
            bufs[k]->m_format = uint32_t(*parseU64(parts[3], 10));
        }
        const size_t stride = (g.isVertexDataInterleaved() && g.areFormatsGpuFriendly()) ? g.positionBuffer.stride()
                                                                                        : computeOptimalVertexStride(g);
        return KeyValues{{"stride", dec(stride)}, {"out", hex64(hashVertexLayout(g))}};
    }
    if (fn == "texdesc") {
        const auto w = splitList(req(in, "w"), ',');
        const uint32_t flags = reqU32(in, "flags");
        D3D9_COMMON_TEXTURE_DESC d{};
        d.Width = uint32_t(*parseU64(w[0], 10));
        d.Height = uint32_t(*parseU64(w[1], 10));
        d.Depth = uint32_t(*parseU64(w[2], 10));
        d.ArraySize = uint32_t(*parseU64(w[3], 10));
        d.MipLevels = uint32_t(*parseU64(w[4], 10));
        d.Usage = uint32_t(*parseU64(w[5], 10));
        d.Format = D3D9Format(uint32_t(*parseU64(w[6], 10)));
        d.Pool = D3DPOOL(uint32_t(*parseU64(w[7], 10)));
        d.MultiSample = D3DMULTISAMPLE_TYPE(uint32_t(*parseU64(w[8], 10)));
        d.MultisampleQuality = uint32_t(*parseU64(w[9], 10));
        d.Discard = (flags & 1) != 0;
        d.IsBackBuffer = (flags & 2) != 0;
        d.IsAttachmentOnly = (flags & 4) != 0;
        return KeyValues{{"out", hex64(d.CalculateHash())}};
    }
    if (fn == "light") {
        const uint32_t type = reqU32(in, "type");
        std::vector<float> v;
        for (const auto& p : splitList(req(in, "f"), ',')) {
            v.push_back(bitsToFloat(uint32_t(*parseU64(p, 16))));
        }
        const auto f3 = [&](size_t i) { return Vector3{{v[i], v[i + 1], v[i + 2]}}; };
        const auto shaping = [&](size_t i) {
            return RtLightShaping{req(in, "en") == "1" ? 1u : 0u, f3(i), v[i + 3], v[i + 4], v[i + 5]};
        };
        XXH64_hash_t h = 0;
        switch (type) {
        case 0: h = sphereHash(f3(0), v[3], shaping(4)); break;
        case 1:
        case 2: {
            const Vector3 position = f3(0), xAxis = f3(5), yAxis = f3(8), direction = f3(11);
            Vector2 dims;
            dims.x = v[3];
            dims.y = v[4];
            const RtLightShaping s = shaping(14);
            h = type == 1 ? rectHash(position, dims, xAxis, yAxis, direction, s) : diskHash(position, dims, xAxis, yAxis, direction, s);
            break;
        }
        case 3: h = cylinderHash(f3(0), v[3], f3(4), v[7]); break;
        default: h = distantHash(f3(0), v[3]); break;
        }
        return KeyValues{{"out", hex64(h)}};
    }
    if (fn == "hexfmt") {
        const XXH64_hash_t h = reqHex(in, "h");
        return KeyValues{{"out", hashToString(h)}, {"opt", "0x" + hashToString(h)}, {"mesh", "mesh_" + hashToString(h)}};
    }
    if (fn == "parse") {
        const Bytes b = reqBytes(in, "s");
        const std::string s(b.begin(), b.end());
        std::string opt;
        try {
            opt = hex64(std::stoull(s, nullptr, 16)); // fillHashVector
        } catch (const std::exception&) {
            opt = "none";
        }
        XXH64_hash_t prim = getNamedHash(s, "mesh_", 5);
        // Upstream runs on the Windows CRT; two corners differ between C libraries, and the oracle
        // pins them to the Windows (UCRT) behaviour whatever CRT it is built against:
        //  * glibc accepts a bare "0x" prefix as the number 0; Windows reports no conversion;
        //  * on overflow of a negated value Wine's msvcrt returns -(2^64 - 1); glibc/UCRT 2^64 - 1.
        const auto crtCorner = [](const std::string& t, bool& bareHexPrefix, bool& negativeOverflow) {
            size_t i = 0;
            while (i < t.size() && (t[i] == ' ' || (t[i] >= '\t' && t[i] <= '\r'))) {
                ++i;
            }
            const bool negative = i < t.size() && t[i] == '-';
            if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
                ++i;
            }
            bareHexPrefix = i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X') &&
                            (i + 2 >= t.size() || !std::isxdigit(static_cast<unsigned char>(t[i + 2])));
            if (!bareHexPrefix && i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
                i += 2;
            }
            size_t digits = 0;
            while (i + digits < t.size() && std::isxdigit(static_cast<unsigned char>(t[i + digits]))) {
                ++digits;
            }
            size_t lead = 0;
            while (lead < digits && t[i + lead] == '0') {
                ++lead;
            }
            negativeOverflow = negative && digits - lead > 16;
        };
        bool bare = false, negOverflow = false;
        crtCorner(s, bare, negOverflow);
        if (bare) {
            opt = "none";
        }
        if (s.compare(0, 5, "mesh_") == 0) {
            bool primBare = false, primNegOverflow = false;
            crtCorner(s.substr(5), primBare, primNegOverflow);
            if (primBare) {
                prim = 0;
            }
            if (primNegOverflow) {
                prim = ~XXH64_hash_t(0);
            }
        }
        (void) negOverflow; // stoull throws out_of_range for it on every CRT
        return KeyValues{{"opt", opt}, {"prim", hex64(prim)}};
    }
#if FUSE_ORACLE_X86
    if (!oracleAvailable()) {
        return std::nullopt;
    }
    if (fn == "disc") {
        const float scale = bitsToFloat(uint32_t(reqHex(in, "scale")));
        g_sceneScale = scale;
        const float step = 0.01f * getMeterToWorldUnitScale();
        const float inv = 1.f / step;
        return KeyValues{{"step", hex32(std::bit_cast<uint32_t>(step))},
                         {"inv", hex32(std::bit_cast<uint32_t>(inv))},
                         {"out", hex32(discretizeOne(uint32_t(reqHex(in, "v")), scale))}};
    }
    if (fn == "legpos") {
        Bytes d = reqBytes(in, "d");
        g_sceneScale = bitsToFloat(uint32_t(reqHex(in, "scale")));
        XXH64_hash_t h0 = reqHex(in, "h0"), h1 = reqHex(in, "h1");
        const HashQuery q{d.data(), size_t(reqDec(in, "size")), size_t(reqDec(in, "stride")), 12};
        if (q.stride != 0) {
            hashRegionLegacy(q, h0, h1);
        }
        return KeyValues{{"out", hex64(h0) + "," + hex64(h1)}};
    }
    if (fn == "draw") {
        return oracleDraw(in);
    }
#endif
    return std::nullopt;
}

// ---- the draw oracle: D3D9Rtx::prepareDrawGeometryForRT + computeHash --------------------------------

#if FUSE_ORACLE_X86
namespace dxvk {

// d3d9_util.cpp: GetVertexCount / DecodeInputAssemblyState (topology only) / DecodeDecltype, and
// dxvk_format.cpp element sizes of the resulting formats.
enum D3DPRIMITIVETYPE : uint32_t {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3, D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
};

uint32_t GetVertexCount(D3DPRIMITIVETYPE type, UINT count) {
    switch (type) {
    default:
    case D3DPT_TRIANGLELIST: return count * 3;
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return count * 2;
    case D3DPT_LINESTRIP: return count + 1;
    case D3DPT_TRIANGLESTRIP: return count + 2;
    case D3DPT_TRIANGLEFAN: return count + 2;
    }
}

uint32_t DecodeTopology(D3DPRIMITIVETYPE type) {
    switch (type) {
    default:
    case D3DPT_TRIANGLELIST: return 3;  // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    case D3DPT_POINTLIST: return 0;     // VK_PRIMITIVE_TOPOLOGY_POINT_LIST
    case D3DPT_LINELIST: return 1;      // VK_PRIMITIVE_TOPOLOGY_LINE_LIST
    case D3DPT_LINESTRIP: return 2;     // VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
    case D3DPT_TRIANGLESTRIP: return 4; // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
    case D3DPT_TRIANGLEFAN: return 5;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
    }
}

uint32_t DecodeDecltype(uint32_t type) {
    switch (type) {
    case 0: return 100;          // FLOAT1    -> R32_SFLOAT
    case 1: return 103;          // FLOAT2    -> R32G32_SFLOAT
    case 2: return 106;          // FLOAT3    -> R32G32B32_SFLOAT
    case 3: return 109;          // FLOAT4    -> R32G32B32A32_SFLOAT
    case 4: return 44;           // D3DCOLOR  -> B8G8R8A8_UNORM
    case 5: return 39;           // UBYTE4    -> R8G8B8A8_USCALED
    case 6: return 80;           // SHORT2    -> R16G16_SSCALED
    case 7: return 94;           // SHORT4    -> R16G16B16A16_SSCALED
    case 8: return 37;           // UBYTE4N   -> R8G8B8A8_UNORM
    case 9: return 78;           // SHORT2N   -> R16G16_SNORM
    case 10: return 92;          // SHORT4N   -> R16G16B16A16_SNORM
    case 11: return 77;          // USHORT2N  -> R16G16_UNORM
    case 12: return 91;          // USHORT4N  -> R16G16B16A16_UNORM
    case 13: return 66;          // UDEC3     -> A2B10G10R10_USCALED_PACK32
    case 15: return 83;          // FLOAT16_2 -> R16G16_SFLOAT
    case 16: return 97;          // FLOAT16_4 -> R16G16B16A16_SFLOAT
    case 14: return 65;          // DEC3N     -> A2B10G10R10_SNORM_PACK32
    default: return 0;           // UNUSED    -> VK_FORMAT_UNDEFINED
    }
}

size_t elementSizeOf(uint32_t vkFormat) {
    switch (vkFormat) {
    case 100: case 44: case 39: case 80: case 37: case 78: case 77: case 66: case 83: case 65: return 4;
    case 103: case 94: case 92: case 91: case 97: return 8;
    case 106: return 12;
    case 109: return 16;
    default: return 0;
    }
}

template<typename T>
void copyIndices(const uint32_t indexCount, T* pIndicesDst, const T* pIndices, uint32_t& minIndex, uint32_t& maxIndex) {
    // fast::findMinMax<T>
    minIndex = pIndices[0];
    maxIndex = pIndices[0];
    for (uint32_t i = 1; i < indexCount; ++i) {
        minIndex = std::min<uint32_t>(minIndex, pIndices[i]);
        maxIndex = std::max<uint32_t>(maxIndex, pIndices[i]);
    }
    // Modify the indices if the min index is non-zero (fast::copySubtract<T>)
    if (minIndex != 0) {
        for (uint32_t i = 0; i < indexCount; ++i) {
            pIndicesDst[i] = T(pIndices[i] - T(minIndex));
        }
    } else {
        memcpy(pIndicesDst, pIndices, sizeof(T) * indexCount);
    }
}

} // namespace dxvk

std::optional<KeyValues> oracleDraw(const KeyValues& in) {
    using namespace dxvk;
    Bytes vb = reqBytes(in, "vb");
    const Bytes idx = reqBytes(in, "idx");
    const auto prim = D3DPRIMITIVETYPE(reqU32(in, "prim"));
    const uint32_t pc = reqU32(in, "pc");
    const uint32_t itype = reqU32(in, "itype"); // VK_INDEX_TYPE_UINT16 / UINT32 / NONE_KHR
    const HashRule rule(reqU32(in, "rule"));
    g_sceneScale = bitsToFloat(uint32_t(reqHex(in, "scale")));

    struct Element {
        bool defined = false;
        uint32_t off = 0, stride = 0, type = 17, stream = 0;
    };
    const auto element = [&](const char* key) {
        Element e;
        const std::string& t = req(in, key);
        if (t != "-") {
            const auto p = splitList(t, ':');
            e.defined = true;
            e.off = uint32_t(*parseU64(p[0], 10));
            e.stride = uint32_t(*parseU64(p[1], 10));
            e.type = uint32_t(*parseU64(p[2], 10));
            e.stream = uint32_t(*parseU64(p[3], 10));
        }
        return e;
    };
    const Element pos = element("pos"), tc = element("tc"), nrm = element("n"), col = element("c");

    // prepareDrawGeometryForRT
    const uint32_t topology = DecodeTopology(prim);
    int vertexIndexOffset = 0; // BaseVertexIndex is folded into the element offsets
    uint32_t indexCount = 0, vertexCount = 0, indexStride = 0, indexType = 0; // undefined RasterBuffer: stride 0, format 0
    uint32_t minIndex = 0, maxIndex = 0;
    std::vector<uint8_t> indices;
    if (itype != 1000165000u) {
        indexCount = GetVertexCount(prim, pc);
        indexStride = itype == 0 ? 2 : 4;
        indexType = itype;
        indices.resize(size_t(indexCount) * indexStride);
        if (indexStride == 2) {
            copyIndices<uint16_t>(indexCount, (uint16_t*) indices.data(), (const uint16_t*) idx.data(), minIndex, maxIndex);
        } else {
            copyIndices<uint32_t>(indexCount, (uint32_t*) indices.data(), (const uint32_t*) idx.data(), minIndex, maxIndex);
        }
        if (maxIndex == minIndex) {
            return KeyValues{{"ok", "0"}};
        }
        vertexCount = maxIndex - minIndex + 1;
        vertexIndexOffset += minIndex;
    } else {
        vertexCount = GetVertexCount(prim, pc);
    }
    if (vertexCount == 0) {
        return KeyValues{{"ok", "0"}};
    }

    // processVertices + getVertexRegion
    const auto region = [&](const Element& e) {
        HashQuery q;
        memset(&q, 0, sizeof(q));
        if (e.defined) {
            q.pBase = vb.data() + e.off + size_t(e.stride) * size_t(vertexIndexOffset);
            q.elementSize = elementSizeOf(DecodeDecltype(e.type));
            q.stride = e.stride;
            q.size = q.stride * vertexCount;
        }
        return q;
    };
    HashQuery vertexRegions[VertexRegions::Count];
    vertexRegions[VertexRegions::Position] = region(pos);
    vertexRegions[VertexRegions::Texcoord] = region(tc);

    // computeHash
    XXH64_hash_t vertexShaderHash = kEmptyHash;
    if (req(in, "vs") == "1") {
        if (rule.test(HashComponents::GeometryDescriptor)) {
            const Bytes bc = reqBytes(in, "vsbc"), f = reqBytes(in, "vsf"), i = reqBytes(in, "vsi"), b = reqBytes(in, "vsb");
            vertexShaderHash = XXH3_64bits(bc.data(), bc.size());
            vertexShaderHash = XXH3_64bits_withSeed(f.data(), reqU32(in, "vsnf") * sizeof(float) * 4, vertexShaderHash);
            vertexShaderHash = XXH3_64bits_withSeed(i.data(), reqU32(in, "vsni") * sizeof(int) * 4, vertexShaderHash);
            vertexShaderHash = XXH3_64bits_withSeed(b.data(), reqU32(in, "vsnb") * sizeof(uint32_t) / 32, vertexShaderHash);
        }
    }
    XXH64_hash_t geometryDescriptorHash = kEmptyHash;
    if (rule.test(HashComponents::GeometryDescriptor)) {
        geometryDescriptorHash = hashGeometryDescriptor(indexCount, vertexCount, indexType, topology);
    }
    XXH64_hash_t vertexLayoutHash = kEmptyHash;
    if (rule.test(HashComponents::VertexLayout)) {
        RasterGeometry g;
        const auto buffer = [](const Element& e) {
            RasterBuffer b;
            b.m_defined = e.defined;
            b.m_slice = e.stream;
            b.m_stride = e.stride;
            b.m_format = DecodeDecltype(e.type);
            return b;
        };
        g.positionBuffer = buffer(pos);
        g.normalBuffer = buffer(nrm);
        g.texcoordBuffer = buffer(tc);
        g.color0Buffer = buffer(col);
        vertexLayoutHash = hashVertexLayout(g);
    }

    GeometryHashes hashes;
    hashes[HashComponents::GeometryDescriptor] = geometryDescriptorHash;
    hashes[HashComponents::VertexLayout] = vertexLayoutHash;
    hashes[HashComponents::VertexShader] = vertexShaderHash;
    switch (indexStride) {
    case 2: hashGeometryData<uint16_t>(rule, indexCount, maxIndex - minIndex, indices.data(), vertexRegions, hashes); break;
    case 4: hashGeometryData<uint32_t>(rule, indexCount, maxIndex - minIndex, indices.data(), vertexRegions, hashes); break;
    default: hashGeometryData<NoIndices>(rule, indexCount, 0, nullptr, vertexRegions, hashes); break;
    }
    hashes.precombine();

    // DrawCallState::getHash / getHashForRuleLegacy. An uninitialised upstream slot is replaced by
    // the combiner's value, which is what FUSE reports (and flags through def0/def1).
    const auto forRule = [&](const HashRule& r) {
        const XXH64_hash_t h = hashes.getHashForRule(r);
        return h == kUninitialisedMarker ? hashes.getHashForRuleImpl(r) : h;
    };
    const XXH64_hash_t materialHash = reqHex(in, "mat");
    const auto legacy = [&](const HashRule& r) {
        XXH64_hash_t h = forRule(r);
        h = XXH64(&indexCount, sizeof(indexCount), h);
        h = XXH64(&vertexCount, sizeof(vertexCount), h);
        h = XXH64(&topology, sizeof(topology), h);
        const uint32_t vertexStride = pos.stride;
        h = XXH64(&vertexStride, sizeof(vertexStride), h);
        h = XXH64(&indexType, sizeof(indexType), h);
        return h ^ materialHash;
    };
    std::string f;
    for (uint32_t i = 0; i < uint32_t(HashComponents::Count); ++i) {
        f += (i ? "," : "") + hex64(hashes.fields[i]);
    }
    return KeyValues{{"ok", "1"},
                     {"f", f},
                     {"ic", dec(indexCount)},
                     {"vc", dec(vertexCount)},
                     {"min", dec(minIndex)},
                     {"max", dec(maxIndex)},
                     {"topo", dec(topology)},
                     {"it", dec(indexType)},
                     {"ps", dec(pos.stride)},
                     {"key", hex64(forRule(HashRule(reqU32(in, "asset"))) ^ materialHash)},
                     {"leg0", hex64(legacy(rules::LegacyAssetHash0))},
                     {"leg1", hex64(legacy(rules::LegacyAssetHash1))},
                     {"def0", hashes.precombined[3] != kUninitialisedMarker ? "1" : "0"},
                     {"def1", hashes.precombined[4] != kUninitialisedMarker ? "1" : "0"}};
}
#else
std::optional<KeyValues> oracleDraw(const KeyValues&) {
    return std::nullopt;
}
#endif

} // namespace fuse::relight::hash::test
