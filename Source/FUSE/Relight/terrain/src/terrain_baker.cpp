// FUSE Relight RL-5.6: terrain baking (see terrain_baker.hpp).
#include <fuse/relight/terrain/terrain_baker.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace fuse::relight::terrain {

namespace {

using i64 = std::int64_t;
constexpr u32 kNone = 0xFFFFFFFFu;

u32 wrap(i64 i, u32 n) { return static_cast<u32>(((i % i64(n)) + i64(n)) % i64(n)); }

struct Fnv {
    u64 h = 1469598103934665603ull;
    void bytes(const void* p, usize n) {
        const auto* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T> void value(const T& v) { bytes(&v, sizeof(T)); }
};

} // namespace

void terrainSample(const TerrainTexture& t, float u, float v, float rgba[4]) {
    if (t.rgba == nullptr || t.width == 0u || t.height == 0u) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.f;
        return;
    }
    const float x = u * float(t.width) - 0.5f;
    const float y = v * float(t.height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    const u32 x0 = wrap(i64(fx), t.width), x1 = wrap(i64(fx) + 1, t.width);
    const u32 y0 = wrap(i64(fy), t.height), y1 = wrap(i64(fy) + 1, t.height);
    const float* a = t.rgba + (usize(y0) * t.width + x0) * 4u;
    const float* b = t.rgba + (usize(y0) * t.width + x1) * 4u;
    const float* c = t.rgba + (usize(y1) * t.width + x0) * 4u;
    const float* d = t.rgba + (usize(y1) * t.width + x1) * 4u;
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * ax;
        const float bottom = c[k] + (d[k] - c[k]) * ax;
        rgba[k] = top + (bottom - top) * ay;
    }
}

void TerrainBaker::reserve(u32 texels) {
    m_bestHeight.reserve(texels);
    m_bestTriangle.reserve(texels);
    m_bestBary.reserve(usize(texels) * 2u);
}

bool TerrainBaker::bake(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers, std::vector<float>& out) {
    if (desc.width == 0u || desc.height == 0u || !(desc.maxX > desc.minX) || !(desc.maxZ > desc.minZ)) {
        return false;
    }
    const usize texels = usize(desc.width) * desc.height;
    if (out.size() < texels * 4u) {
        out.resize(texels * 4u);
    }
    if (m_bestHeight.size() < texels) {
        m_bestHeight.resize(texels);
        m_bestTriangle.resize(texels);
        m_bestBary.resize(texels * 2u);
    }
    for (usize i = 0; i < texels; ++i) {
        std::memcpy(&out[i * 4u], desc.clear, sizeof(desc.clear));
    }
    const float sx = float(desc.width) / (desc.maxX - desc.minX);
    const float sz = float(desc.height) / (desc.maxZ - desc.minZ);
    m_stats.texelsWritten = 0;
    for (const TerrainLayer& L : layers) {
        if (L.positions == nullptr || L.indices == nullptr) {
            continue;
        }
        std::fill(m_bestTriangle.begin(), m_bestTriangle.begin() + std::ptrdiff_t(texels), kNone);
        // Raster: per triangle, the texel centres inside its top-down projection (edges inclusive, either winding).
        for (u32 t = 0; t + 2u < L.indexCount; t += 3u) {
            const u32 i0 = L.indices[t], i1 = L.indices[t + 1u], i2 = L.indices[t + 2u];
            if (i0 >= L.vertexCount || i1 >= L.vertexCount || i2 >= L.vertexCount) {
                continue;
            }
            const float* p0 = L.positions + usize(i0) * 3u;
            const float* p1 = L.positions + usize(i1) * 3u;
            const float* p2 = L.positions + usize(i2) * 3u;
            // Texel space: X = (x - minX) sx, Z = (z - minZ) sz.
            const float ax = (p0[0] - desc.minX) * sx, az = (p0[2] - desc.minZ) * sz;
            const float bx = (p1[0] - desc.minX) * sx, bz = (p1[2] - desc.minZ) * sz;
            const float cx = (p2[0] - desc.minX) * sx, cz = (p2[2] - desc.minZ) * sz;
            const float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
            if (area == 0.f) {
                continue;
            }
            const float inv = 1.f / area;
            const i64 x0 = std::max<i64>(0, i64(std::floor(std::min({ax, bx, cx}) - 0.5f)));
            const i64 x1 = std::min<i64>(i64(desc.width) - 1, i64(std::ceil(std::max({ax, bx, cx}) - 0.5f)));
            const i64 z0 = std::max<i64>(0, i64(std::floor(std::min({az, bz, cz}) - 0.5f)));
            const i64 z1 = std::min<i64>(i64(desc.height) - 1, i64(std::ceil(std::max({az, bz, cz}) - 0.5f)));
            for (i64 zi = z0; zi <= z1; ++zi) {
                const float pz = float(zi) + 0.5f;
                for (i64 xi = x0; xi <= x1; ++xi) {
                    const float px = float(xi) + 0.5f;
                    const float w1 = ((px - ax) * (cz - az) - (pz - az) * (cx - ax)) * inv;
                    const float w2 = ((bx - ax) * (pz - az) - (bz - az) * (px - ax)) * inv;
                    const float w0 = 1.f - w1 - w2;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f) {
                        continue;
                    }
                    const float y = p0[1] * w0 + p1[1] * w1 + p2[1] * w2;
                    const usize i = usize(zi) * desc.width + usize(xi);
                    if (m_bestTriangle[i] == kNone || y > m_bestHeight[i]) {
                        m_bestTriangle[i] = t;
                        m_bestHeight[i] = y;
                        m_bestBary[i * 2u] = w1;
                        m_bestBary[i * 2u + 1u] = w2;
                    }
                }
            }
        }
        // Shade and blend.
        for (usize i = 0; i < texels; ++i) {
            const u32 t = m_bestTriangle[i];
            if (t == kNone) {
                continue;
            }
            const float w1 = m_bestBary[i * 2u], w2 = m_bestBary[i * 2u + 1u], w0 = 1.f - w1 - w2;
            const u32 v0 = L.indices[t], v1 = L.indices[t + 1u], v2 = L.indices[t + 2u];
            float u = 0.f, v = 0.f;
            if (L.uvs != nullptr) {
                u = L.uvs[v0 * 2u] * w0 + L.uvs[v1 * 2u] * w1 + L.uvs[v2 * 2u] * w2;
                v = L.uvs[v0 * 2u + 1u] * w0 + L.uvs[v1 * 2u + 1u] * w1 + L.uvs[v2 * 2u + 1u] * w2;
            }
            const float tu = L.uvTransform[0] * u + L.uvTransform[1] * v + L.uvTransform[2];
            const float tv = L.uvTransform[3] * u + L.uvTransform[4] * v + L.uvTransform[5];
            float src[4] = {1.f, 1.f, 1.f, 1.f};
            if (L.texture != nullptr) {
                terrainSample(*L.texture, tu, tv, src);
            }
            if (L.colors != nullptr) {
                for (int k = 0; k < 4; ++k) {
                    src[k] *= L.colors[v0 * 4u + k] * w0 + L.colors[v1 * 4u + k] * w1 + L.colors[v2 * 4u + k] * w2;
                }
            }
            if (L.alphaTest && !(src[3] > L.alphaReference)) {
                continue;
            }
            float* dst = &out[i * 4u];
            const float a = src[3];
            switch (L.blend) {
            case TerrainBlend::Opaque:
                for (int k = 0; k < 4; ++k) {
                    dst[k] = src[k];
                }
                break;
            case TerrainBlend::Alpha:
                for (int k = 0; k < 3; ++k) {
                    dst[k] = src[k] * a + dst[k] * (1.f - a);
                }
                dst[3] = a + dst[3] * (1.f - a);
                break;
            case TerrainBlend::Additive:
                for (int k = 0; k < 3; ++k) {
                    dst[k] = dst[k] + src[k] * a;
                }
                break;
            case TerrainBlend::Multiply:
                for (int k = 0; k < 3; ++k) {
                    dst[k] = dst[k] * src[k];
                }
                break;
            case TerrainBlend::Multiply2x:
                for (int k = 0; k < 3; ++k) {
                    dst[k] = dst[k] * src[k] + src[k] * dst[k];
                }
                break;
            }
            ++m_stats.texelsWritten;
        }
    }
    ++m_stats.bakes;
    return true;
}

u64 TerrainBaker::contentHash(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers) {
    Fnv f;
    f.value(desc);
    for (const TerrainLayer& L : layers) {
        f.value(L.vertexCount);
        f.value(L.indexCount);
        f.value(L.blend);
        f.value(L.uvTransform);
        f.value(L.alphaTest);
        f.value(L.alphaReference);
        if (L.positions != nullptr) {
            f.bytes(L.positions, usize(L.vertexCount) * 3u * sizeof(float));
        }
        if (L.uvs != nullptr) {
            f.bytes(L.uvs, usize(L.vertexCount) * 2u * sizeof(float));
        }
        if (L.colors != nullptr) {
            f.bytes(L.colors, usize(L.vertexCount) * 4u * sizeof(float));
        }
        if (L.indices != nullptr) {
            f.bytes(L.indices, usize(L.indexCount) * sizeof(u32));
        }
        if (L.texture != nullptr && L.texture->rgba != nullptr) {
            f.value(L.texture->width);
            f.value(L.texture->height);
            f.bytes(L.texture->rgba, usize(L.texture->width) * L.texture->height * 4u * sizeof(float));
        }
    }
    return f.h == 0u ? 1u : f.h;
}

bool TerrainBaker::bakeCached(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers,
                              std::vector<float>& out) {
    const u64 h = contentHash(desc, layers);
    if (m_cacheValid && h == m_cachedHash && out.size() >= usize(desc.width) * desc.height * 4u) {
        ++m_stats.cacheHits;
        return false;
    }
    if (!bake(desc, layers, out)) {
        return false;
    }
    m_cachedHash = h;
    m_cacheValid = true;
    return true;
}

void terrainLookup(const TerrainBakeDesc& desc, const std::vector<float>& baked, float x, float z, float rgba[4]) {
    const float fx = std::min(std::max((x - desc.minX) / (desc.maxX - desc.minX) * float(desc.width) - 0.5f, 0.f),
                              float(desc.width - 1u));
    const float fz = std::min(std::max((z - desc.minZ) / (desc.maxZ - desc.minZ) * float(desc.height) - 0.5f, 0.f),
                              float(desc.height - 1u));
    const u32 x0 = u32(fx), z0 = u32(fz);
    const u32 x1 = std::min(x0 + 1u, desc.width - 1u), z1 = std::min(z0 + 1u, desc.height - 1u);
    const float ax = fx - float(x0), az = fz - float(z0);
    const float* a = &baked[(usize(z0) * desc.width + x0) * 4u];
    const float* b = &baked[(usize(z0) * desc.width + x1) * 4u];
    const float* c = &baked[(usize(z1) * desc.width + x0) * 4u];
    const float* d = &baked[(usize(z1) * desc.width + x1) * 4u];
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * ax;
        const float bottom = c[k] + (d[k] - c[k]) * ax;
        rgba[k] = top + (bottom - top) * az;
    }
}

} // namespace fuse::relight::terrain
