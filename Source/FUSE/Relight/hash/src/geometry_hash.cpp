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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_hashing.cpp@0867d3c,
// src/d3d9/d3d9_rtx_geometry.cpp@0867d3c, src/d3d9/d3d9_rtx.cpp@0867d3c (prepareDrawGeometryForRT),
// src/dxvk/rtx_render/rtx_types.h@0867d3c (getHash / getHashForRuleLegacy) and
// src/dxvk/rtx_render/rtx_geometry_utils.cpp@0867d3c (computeOptimalVertexStride).
// FUSE changes: no DXVK types (plain pointers and spans), no worker threads or buffer ref counting,
// the SSE4.1 legacy discretisation is emulated bit-exactly in integer arithmetic, and the D3D9 draw
// preprocessing is a pure function of the draw's inputs.
#include <fuse/relight/hash/geometry_hash.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

#if defined(__FAST_MATH__)
#error "fuse_relight_hash needs IEEE-754 float semantics (legacy position hashes): do not build it with -ffast-math"
#endif

namespace fuse::relight::hash {

namespace {

constexpr std::array<std::string_view, kHashComponentCount> kComponentNames = {
    "positions",   "legacypositions0",   "legacypositions1", "texcoords",    "indices",
    "legacyindices", "geometrydescriptor", "vertexlayout",     "vertexshader",
};

// TODO (REMIX-656) upstream: 512b - this is a performance optimization
constexpr std::uint32_t kMaxGeomHashSize = 512;
constexpr std::uint32_t kInitialHashVertexCount = 20;

template <typename T>
T loadUnaligned(const void* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

std::uint32_t loadIndex(const std::uint8_t* p, std::uint32_t indexSize) noexcept {
    return indexSize == 2 ? std::uint32_t(loadUnaligned<std::uint16_t>(p)) : loadUnaligned<std::uint32_t>(p);
}

} // namespace

std::string_view hashComponentName(HashComponent component) noexcept {
    const auto i = std::uint32_t(component);
    return i < kHashComponentCount ? kComponentNames[i] : std::string_view{};
}

HashRule parseHashRule(std::string_view ruleString) {
    HashRule rule;
    if (ruleString.empty()) {
        return rule;
    }
    // Remove any spaces in case the tokens have spaces after the delimiters.
    std::string noSpaces;
    noSpaces.reserve(ruleString.size());
    for (char c : ruleString) {
        if (c != ' ') {
            noSpaces.push_back(c);
        }
    }
    std::string_view rest = noSpaces;
    while (true) {
        const std::size_t comma = rest.find(',');
        const std::string_view token = rest.substr(0, comma);
        for (std::uint32_t i = 0; i < kHashComponentCount; ++i) {
            if (token == kComponentNames[i]) {
                rule.set(HashComponent(i));
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return rule;
}

std::string formatHashRule(HashRule rule) {
    std::string out;
    for (std::uint32_t i = 0; i < kHashComponentCount; ++i) {
        if (rule.test(HashComponent(i))) {
            if (!out.empty()) {
                out.push_back(',');
            }
            out += kComponentNames[i];
        }
    }
    return out;
}

Hash64 hashRuleId(HashRule rule) {
    const std::string s = formatHashRule(rule);
    return xxh3_64(s.data(), s.size());
}

Hash64 GeometryHashes::hashForRule(HashRule rule) const noexcept {
    Hash64 result = kEmptyHash;
    for (std::uint32_t i = 0; i < kHashComponentCount; ++i) {
        if (rule.test(HashComponent(i))) {
            if (result == kEmptyHash) {
                // For the first entry, we use the hash directly
                result = fields[i];
            } else {
                // For all other entries, we combine the hash via seeding
                result = xxh64(&fields[i], sizeof(Hash64), result);
            }
        }
    }
    return result;
}

bool GeometryHashes::isRuleHashDefinedUpstream(HashRule rule) const noexcept {
    if (rule == rules::kLegacyAsset0) {
        return (*this)[HashComponent::LegacyPositions0] != kEmptyHash;
    }
    if (rule == rules::kLegacyAsset1) {
        return (*this)[HashComponent::LegacyPositions1] != kEmptyHash;
    }
    return true;
}

Hash64 hashGeometryDescriptor(std::uint32_t indexCount, std::uint32_t vertexCount, std::uint32_t indexType,
                              std::uint32_t topology) noexcept {
    // Note: Only information relating to how the geometry is structured should be included here.
    Hash64 h = xxh3_64(&indexCount, sizeof(indexCount), 0);
    h = xxh3_64(&vertexCount, sizeof(vertexCount), h);
    h = xxh3_64(&topology, sizeof(topology), h);
    return xxh3_64(&indexType, sizeof(indexType), h);
}

Hash64 hashVertexLayoutStride(std::uint64_t vertexStride) noexcept {
    // Upstream hashes a size_t, i.e. 8 bytes on the x64-only Remix. Pin the width so 32-bit builds
    // hash the same 8 little-endian bytes instead of a 4-byte size_t.
    static_assert(sizeof(vertexStride) == 8, "Remix vertex-layout hash is over the 8-byte x64 size_t stride");
    return xxh3_64(&vertexStride, sizeof(vertexStride));
}

Hash64 hashContiguousMemory(const void* data, std::size_t size) noexcept {
    return xxh3_64(data, size);
}

Hash64 hashVertexRegion(const std::uint8_t* base, std::size_t size, std::size_t stride, std::size_t elementSize,
                        std::span<const std::uint32_t> uniqueIndices) noexcept {
    Hash64 result = 0;
    if (!uniqueIndices.empty()) {
        for (const std::uint32_t idx : uniqueIndices) {
            const std::uint8_t* p = elementSize != 0 ? base + std::size_t(idx) * stride : base;
            result = xxh3_64(p, elementSize, result);
        }
    } else if (stride != 0) {
        for (std::size_t i = 0; i < size; i += stride) {
            result = xxh3_64(base + i, elementSize, result);
        }
    }
    return result;
}

std::vector<std::uint32_t> sortedUniqueIndices(const void* indices, std::uint32_t indexCount, std::uint32_t indexSize,
                                               std::uint32_t maxIndexValue) {
    // Use memory as a bin table for index data, then repopulate the bins with contiguous values.
    std::vector<std::uint8_t> bins(std::size_t(maxIndexValue) + 1u, 0);
    const auto* p = static_cast<const std::uint8_t*>(indices);
    for (std::uint32_t i = 0; i < indexCount; ++i) {
        const std::uint32_t index = loadIndex(p + std::size_t(i) * indexSize, indexSize);
        if (index <= maxIndexValue) {
            bins[index] = 1;
        }
    }
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i <= maxIndexValue; ++i) {
        if (bins[i]) {
            out.push_back(i);
        }
    }
    return out;
}

Hash64 hashIndicesLegacy(const void* indices, std::size_t indexCount, std::size_t indexSize) noexcept {
    Hash64 indexHash = 0;
    const std::size_t bytes = indexCount * indexSize;
    if (bytes <= std::size_t(kMaxGeomHashSize) * 2) {
        // Short buffer
        indexHash = xxh3_64(indices, bytes);
    } else {
        // Long buffer, sample indices throughout
        const auto step = std::uint32_t(bytes / kMaxGeomHashSize);
        const auto* p = static_cast<const std::uint8_t*>(indices);
        for (std::uint32_t i = 0; i < indexCount; i += step) {
            indexHash = xxh3_64(p + std::size_t(i) * indexSize, indexSize, indexHash);
        }
    }
    return indexHash;
}

float legacyDiscreteStepSize(float sceneScale) noexcept {
    const float meterToWorldUnitScale = 100.f * sceneScale; // RtxOptions::getMeterToWorldUnitScale()
    return 0.01f * meterToWorldUnitScale;
}

std::uint32_t mulF32Bits(std::uint32_t a, std::uint32_t b) noexcept {
    // IEEE-754 binary32 multiply, round to nearest even, no FTZ/DAZ; NaN handling as x86 SSE
    // MULSS/MULPS: a NaN first operand is returned quieted, else a NaN second operand, and an
    // invalid operation (inf * 0) gives the default "real indefinite" NaN 0xFFC00000.
    constexpr std::uint32_t kQuiet = 0x00400000u;
    const std::uint32_t sign = (a ^ b) & 0x80000000u;
    std::int32_t ea = std::int32_t((a >> 23) & 0xffu);
    std::int32_t eb = std::int32_t((b >> 23) & 0xffu);
    std::uint32_t ma = a & 0x007fffffu;
    std::uint32_t mb = b & 0x007fffffu;
    if (ea == 255 && ma != 0) {
        return a | kQuiet;
    }
    if (eb == 255 && mb != 0) {
        return b | kQuiet;
    }
    const bool zeroA = ea == 0 && ma == 0;
    const bool zeroB = eb == 0 && mb == 0;
    if (ea == 255 || eb == 255) {
        return (zeroA || zeroB) ? 0xFFC00000u : (sign | 0x7f800000u);
    }
    if (zeroA || zeroB) {
        return sign;
    }
    // Significands with the implicit bit, normalised so bit 23 is set (subnormals shifted up).
    std::uint32_t sa = ea != 0 ? (ma | 0x00800000u) : ma;
    std::uint32_t sb = eb != 0 ? (mb | 0x00800000u) : mb;
    ea = ea != 0 ? ea : 1;
    eb = eb != 0 ? eb : 1;
    while ((sa & 0x00800000u) == 0) {
        sa <<= 1;
        --ea;
    }
    while ((sb & 0x00800000u) == 0) {
        sb <<= 1;
        --eb;
    }
    std::uint64_t product = std::uint64_t(sa) * sb; // in [2^46, 2^48)
    std::int32_t exp = ea + eb - 127;
    if (product & (std::uint64_t(1) << 47)) {
        ++exp;
    } else {
        product <<= 1;
    }
    // product in [2^47, 2^48): the result significand is its top 24 bits (fewer when subnormal).
    if (exp >= 255) {
        return sign | 0x7f800000u;
    }
    const std::int32_t shift = exp > 0 ? 24 : 24 + (1 - exp);
    if (shift > 48) {
        return sign; // below half the smallest subnormal: rounds to zero
    }
    std::uint64_t sig = product >> shift;
    const std::uint64_t rem = product & ((std::uint64_t(1) << shift) - 1);
    const std::uint64_t half = std::uint64_t(1) << (shift - 1);
    if (rem > half || (rem == half && (sig & 1u))) {
        ++sig;
    }
    if (exp <= 0) {
        // Subnormal (a carry into bit 23 correctly yields the smallest normal encoding).
        return sign | std::uint32_t(sig);
    }
    if (sig == (std::uint64_t(1) << 24)) {
        sig >>= 1;
        ++exp;
        if (exp >= 255) {
            return sign | 0x7f800000u;
        }
    }
    return sign | (std::uint32_t(exp) << 23) | (std::uint32_t(sig) & 0x007fffffu);
}

std::uint32_t floorF32Bits(std::uint32_t u) noexcept {
    // ROUNDSS/ROUNDPS with _MM_FROUND_FLOOR: exact; NaN and infinities pass through, -0 stays -0.
    const std::int32_t e = std::int32_t((u >> 23) & 0xffu) - 127;
    if (e >= 23) {
        return u; // already integral, or inf/NaN
    }
    if (e < 0) {
        if ((u & 0x7fffffffu) == 0) {
            return u; // +-0
        }
        return (u >> 31) ? 0xbf800000u /* -1.0f */ : 0u /* +0.0f */;
    }
    const std::uint32_t m = 0x007fffffu >> e;
    if ((u & m) == 0) {
        return u;
    }
    if (u >> 31) {
        u += m;
    }
    return u & ~m;
}

std::uint32_t discretizeLegacyBits(std::uint32_t valueBits, std::uint32_t stepBits, std::uint32_t invStepBits) noexcept {
    // discretize_SSE, one lane: _mm_mul_ps(val, invStepSize) -> _mm_round_ps(FLOOR) ->
    // _mm_mul_ps(val, stepSize). Emulated in integer arithmetic so the result does not depend on
    // the caller's MXCSR (FTZ/DAZ), the compiler's operand order for NaNs or libm.
    // (Upstream's non-SSE fallback divides by the step instead, which differs in the last bit.)
    return mulF32Bits(floorF32Bits(mulF32Bits(valueBits, invStepBits)), stepBits);
}

float discretizeLegacy(float value, float stepSize, float invStepSize) noexcept {
    return std::bit_cast<float>(
        discretizeLegacyBits(std::bit_cast<std::uint32_t>(value), std::bit_cast<std::uint32_t>(stepSize),
                             std::bit_cast<std::uint32_t>(invStepSize)));
}

void hashPositionsLegacy(const std::uint8_t* base, std::size_t size, std::size_t stride, float stepSize, Hash64& h0,
                         Hash64& h1) noexcept {
    if (stride == 0) {
        return;
    }
    const float invStepSize = 1.f / stepSize;
    const auto stepBits = std::bit_cast<std::uint32_t>(stepSize);
    const auto invStepBits = std::bit_cast<std::uint32_t>(invStepSize);
    const std::size_t dataForLegacyHash = std::min(size, std::size_t(kInitialHashVertexCount) * stride);
    for (std::size_t i = 0; i < size; i += stride) {
        // Save the legacy hash upon reaching 20 vertices (or less)
        if (i == dataForLegacyHash) {
            h0 = h1;
        }
        std::uint32_t v[3];
        std::memcpy(v, base + i, sizeof(v));
        for (std::uint32_t& c : v) {
            c = discretizeLegacyBits(c, stepBits, invStepBits);
        }
        h1 = xxh3_64(v, sizeof(v), h1);
    }
}

Hash64 hashVertexShader(std::span<const std::uint8_t> bytecode, const void* floatConstants, std::uint32_t maxConstIndexF,
                        const void* intConstants, std::uint32_t maxConstIndexI, const void* boolConstants,
                        std::uint32_t maxConstIndexB) noexcept {
    Hash64 h = xxh3_64(bytecode.data(), bytecode.size());
    h = xxh3_64(floatConstants, std::size_t(maxConstIndexF) * sizeof(float) * 4, h);
    h = xxh3_64(intConstants, std::size_t(maxConstIndexI) * sizeof(std::int32_t) * 4, h);
    h = xxh3_64(boolConstants, std::size_t(maxConstIndexB) * sizeof(std::uint32_t) / 32, h);
    return h;
}

std::uint64_t vertexLayoutStride(const VertexLayoutInput& in) noexcept {
    namespace vf = vk_format;
    const auto sameSlice = [&](const VertexLayoutElement& e) {
        return e.stream == in.position.stream && e.stride == in.position.stride;
    };
    const bool interleaved = (!in.normal.defined || sameSlice(in.normal)) && (!in.texcoord.defined || sameSlice(in.texcoord)) &&
                             (!in.color0.defined || sameSlice(in.color0));

    bool gpuFriendly = in.position.vkFormat == vf::kR32G32B32Sfloat || in.position.vkFormat == vf::kR32G32B32A32Sfloat;
    if (in.normal.defined && in.normal.vkFormat != vf::kR32G32B32Sfloat && in.normal.vkFormat != vf::kR32G32B32A32Sfloat &&
        in.normal.vkFormat != vf::kR32Uint) {
        gpuFriendly = false;
    }
    if (in.texcoord.defined && in.texcoord.vkFormat != vf::kR32G32Sfloat && in.texcoord.vkFormat != vf::kR32G32B32Sfloat &&
        in.texcoord.vkFormat != vf::kR32G32B32A32Sfloat) {
        gpuFriendly = false;
    }
    if (in.color0.defined && in.color0.vkFormat != vf::kB8G8R8A8Unorm) {
        gpuFriendly = false;
    }
    if (interleaved && gpuFriendly) {
        return in.position.stride;
    }
    // RtxGeometryUtils::computeOptimalVertexStride(input, forceNormals = false)
    std::uint64_t stride = sizeof(float) * 3; // position is the minimum
    if (in.normal.defined) {
        stride += sizeof(float) * 3;
    }
    if (in.texcoord.defined) {
        stride += sizeof(float) * 2;
    }
    if (in.color0.defined) {
        stride += sizeof(std::uint32_t);
    }
    return stride;
}

std::optional<DrawGeometryHashes> computeDrawGeometryHashes(const DrawGeometryInput& draw, HashRule rule,
                                                            float sceneScale) {
    if (!draw.position.defined()) {
        return std::nullopt;
    }
    DrawGeometryHashes out;
    out.topology = std::uint32_t(topologyFromD3D(draw.primitiveType));
    out.positionStride = draw.position.stride;

    const bool indexed = draw.indexType == IndexType::Uint16 || draw.indexType == IndexType::Uint32;
    const std::uint32_t indexSize = draw.indexType == IndexType::Uint16 ? 2u : 4u;
    if (indexed) {
        out.indexCount = d3dVertexCount(draw.primitiveType, draw.primitiveCount);
        out.indexType = std::uint32_t(draw.indexType);
        const auto* src = static_cast<const std::uint8_t*>(draw.indexData);
        if (out.indexCount == 0 || src == nullptr) {
            return std::nullopt;
        }
        // copyIndices: find min/max, subtract the min, keep the width.
        std::uint32_t minIndex = 0xffffffffu;
        std::uint32_t maxIndex = 0;
        for (std::uint32_t i = 0; i < out.indexCount; ++i) {
            const std::uint32_t idx = loadIndex(src + std::size_t(i) * indexSize, indexSize);
            minIndex = std::min(minIndex, idx);
            maxIndex = std::max(maxIndex, idx);
        }
        if (maxIndex == minIndex) {
            return std::nullopt; // "no triangles detected in index buffer"
        }
        out.minIndex = minIndex;
        out.maxIndex = maxIndex;
        out.rebasedIndices.resize(std::size_t(out.indexCount) * indexSize);
        for (std::uint32_t i = 0; i < out.indexCount; ++i) {
            const std::uint32_t idx = loadIndex(src + std::size_t(i) * indexSize, indexSize) - minIndex;
            std::uint8_t* dst = out.rebasedIndices.data() + std::size_t(i) * indexSize;
            if (indexSize == 2) {
                const auto v16 = std::uint16_t(idx);
                std::memcpy(dst, &v16, 2);
            } else {
                std::memcpy(dst, &idx, 4);
            }
        }
        out.vertexCount = maxIndex - minIndex + 1;
    } else {
        // Non-indexed: RasterGeometry keeps indexCount 0 and an undefined index buffer, whose
        // indexType() reads the zero-initialised format union as VK_INDEX_TYPE_UINT16 (0).
        out.vertexCount = d3dVertexCount(draw.primitiveType, draw.primitiveCount);
        out.indexType = 0;
    }
    if (out.vertexCount == 0) {
        return std::nullopt;
    }

    // processVertices: the vertex window starts at BaseVertexIndex + minIndex.
    const auto region = [&](const DrawVertexElement& e, const std::uint8_t*& base, std::size_t& size, std::size_t& stride,
                            std::size_t& elementSize) {
        base = nullptr;
        size = stride = elementSize = 0;
        if (!e.defined()) {
            return;
        }
        base = e.data + std::size_t(out.minIndex) * e.stride;
        stride = e.stride;
        size = stride * out.vertexCount;
        elementSize = declTypeElementSize(e.type);
    };

    GeometryHashes& h = out.hashes;

    if (draw.programmableVsWithCapture && rule.test(HashComponent::GeometryDescriptor)) {
        const DrawVertexShader& vs = draw.vertexShader;
        h[HashComponent::VertexShader] = hashVertexShader(vs.bytecode, vs.floatConstants, vs.maxConstIndexF, vs.intConstants,
                                                          vs.maxConstIndexI, vs.boolConstants, vs.maxConstIndexB);
    }
    if (rule.test(HashComponent::GeometryDescriptor)) {
        h[HashComponent::GeometryDescriptor] = hashGeometryDescriptor(out.indexCount, out.vertexCount, out.indexType, out.topology);
    }
    if (rule.test(HashComponent::VertexLayout)) {
        const auto layoutElement = [](const DrawVertexElement& e) {
            return VertexLayoutElement{e.defined(), e.stream, e.stride, declTypeVkFormat(e.type)};
        };
        const VertexLayoutInput layout{layoutElement(draw.position), layoutElement(draw.normal), layoutElement(draw.texcoord),
                                       layoutElement(draw.color0)};
        h[HashComponent::VertexLayout] = hashVertexLayoutStride(vertexLayoutStride(layout));
    }

    std::vector<std::uint32_t> uniqueIndices;
    if (indexed) {
        uniqueIndices = sortedUniqueIndices(out.rebasedIndices.data(), out.indexCount, indexSize, out.maxIndex - out.minIndex);
        if (rule.test(HashComponent::Indices)) {
            h[HashComponent::Indices] = hashContiguousMemory(out.rebasedIndices.data(), out.rebasedIndices.size());
        }
        if (rule.test(HashComponent::LegacyIndices)) {
            h[HashComponent::LegacyIndices] = hashIndicesLegacy(out.rebasedIndices.data(), out.indexCount, indexSize);
        }
    }

    const std::uint8_t* base = nullptr;
    std::size_t size = 0, stride = 0, elementSize = 0;
    if (rule.test(HashComponent::Positions)) {
        region(draw.position, base, size, stride, elementSize);
        h[HashComponent::Positions] = hashVertexRegion(base, size, stride, elementSize, uniqueIndices);
    }
    if (rule.test(HashComponent::Texcoords)) {
        region(draw.texcoord, base, size, stride, elementSize);
        h[HashComponent::Texcoords] = hashVertexRegion(base, size, stride, elementSize, uniqueIndices);
    }
    if (rule.test(HashComponent::LegacyPositions0) || rule.test(HashComponent::LegacyPositions1)) {
        region(draw.position, base, size, stride, elementSize);
        hashPositionsLegacy(base, size, stride, legacyDiscreteStepSize(sceneScale), h[HashComponent::LegacyPositions0],
                            h[HashComponent::LegacyPositions1]);
    }
    return out;
}

Hash64 meshReplacementHash(const GeometryHashes& hashes, HashRule assetRule, Hash64 materialHash) noexcept {
    return hashes.hashForRule(assetRule) ^ materialHash;
}

Hash64 meshReplacementHashLegacy(const DrawGeometryHashes& draw, HashRule legacyRule, Hash64 materialHash) noexcept {
    // Note: Only information relating to how the geometry is structured should be included here.
    Hash64 h = draw.hashes.hashForRule(legacyRule);
    h = xxh64(&draw.indexCount, sizeof(draw.indexCount), h);
    h = xxh64(&draw.vertexCount, sizeof(draw.vertexCount), h);
    h = xxh64(&draw.topology, sizeof(draw.topology), h);
    h = xxh64(&draw.positionStride, sizeof(draw.positionStride), h);
    h = xxh64(&draw.indexType, sizeof(draw.indexType), h);
    return h ^ materialHash;
}

} // namespace fuse::relight::hash
