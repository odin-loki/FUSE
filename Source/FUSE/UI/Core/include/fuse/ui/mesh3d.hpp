// FUSE UI 3D mesh (charts / datavis3d GPU path): indexed, lit triangle meshes drawn with a depth buffer. One spec,
// two backends: rasterMesh() below is the CPU reference (and the software fallback); fuse_ui_vk's UiVkMesh3D draws the
// same Mesh3D with Vulkan vertex / index buffers, a D32 depth buffer and the same per-vertex lighting, into an RGBA8
// image the draw-list kernel composites as an external texture (fuse::ui::Image::externalSlot).
//
// Conventions (both backends): MeshFrame::mvp is column-major and maps model space to Vulkan clip space (x right,
// y down, z_ndc in [0, 1], depth test LESS, cleared to 1). Lighting is per vertex, two-sided Lambert with ambient:
// rgb * (ambient + (1 - ambient) * |dot(normalize(n), light)|), interpolated without perspective correction
// (noperspective) and written opaque. Pixels are covered at their centres with 8-bit sub-pixel vertex snapping and the
// top-left fill rule, so CPU and GPU agree on coverage. Depth is evaluated per pixel with one fixed sequence of float
// operations (meshDepth below, mirrored by ui_mesh3d_exact.frag through gl_PrimitiveID), so equal / near-equal depths
// resolve the same way on both backends: LESS keeps the first-drawn triangle on a tie. An optional R32 id buffer
// records the winning triangle's MeshVertex::id for picking.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fuse::ui {

struct MeshVertex {
    f32 x = 0, y = 0, z = 0;
    f32 nx = 0, ny = 0, nz = 1;
    u32 rgba = 0xFFFFFFFFu; ///< straight RGBA8, R in the low byte
    u32 id = 0; ///< pick id (0: none); a triangle reports its first vertex's id
};
static_assert(sizeof(MeshVertex) == 32);

struct MeshFrame {
    f32 mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    f32 light[4] = {0, 0, 1, 0.35f}; ///< xyz: unit direction towards the light (model space), w: ambient
    u32 width = 0, height = 0;       ///< target size in pixels
    u32 clear = 0;                   ///< premultiplied RGBA8 background (0: transparent, composited over the chart)
};

struct Mesh3D {
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices; ///< triangle list
    MeshFrame frame;
    void clear() { vertices.clear(), indices.clear(); }
    u32 add(const MeshVertex& v) { vertices.push_back(v); return u32(vertices.size() - 1); }
    void tri(u32 a, u32 b, u32 c) { indices.push_back(a), indices.push_back(b), indices.push_back(c); }
    void quad(u32 a, u32 b, u32 c, u32 d) { tri(a, b, c), tri(a, c, d); }
};

namespace mesh3d_detail {
inline u8 unorm(f32 v) { return u8(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }
struct Sv {
    long long x, y; ///< 24.8 fixed point
    f32 fx, fy;     ///< the same, as floats (exact: |v| < 2^24)
    f32 z, r, g, b, a;
    bool ok;
};
/// Depth at the pixel centre (px, py in 1/256 px) of triangle a, b, c in index order. Every operation is a single
/// IEEE float op in this order (the GLSL twin is `precise`), so both backends produce bit-identical depths.
inline f32 meshDepth(const Sv& a, const Sv& b, const Sv& c, f32 px, f32 py) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const f32 area = (b.fx - a.fx) * (c.fy - a.fy) - (b.fy - a.fy) * (c.fx - a.fx);
    const f32 e0 = (c.fx - b.fx) * (py - b.fy) - (c.fy - b.fy) * (px - b.fx);
    const f32 e1 = (a.fx - c.fx) * (py - c.fy) - (a.fy - c.fy) * (px - c.fx);
    const f32 l0 = e0 / area, l1 = e1 / area;
    const f32 l2 = 1.f - l0 - l1;
    return l0 * a.z + l1 * b.z + l2 * c.z;
}
} // namespace mesh3d_detail

/// The lit colour of a vertex (straight, 0..1), shared by both backends' specification.
inline void meshShade(const MeshVertex& v, const MeshFrame& f, f32 out[4]) {
    const f32 l = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
    const f32 d = l > 0.f ? std::abs((v.nx * f.light[0] + v.ny * f.light[1] + v.nz * f.light[2]) / l) : 1.f;
    const f32 k = f.light[3] + (1.f - f.light[3]) * std::min(d, 1.f);
    out[0] = f32(v.rgba & 0xFF) / 255.f * k, out[1] = f32((v.rgba >> 8) & 0xFF) / 255.f * k;
    out[2] = f32((v.rgba >> 16) & 0xFF) / 255.f * k, out[3] = f32(v.rgba >> 24) / 255.f;
}

/// CPU reference: rasterises `m` into `out` (resized to frame.width x height, cleared to frame.clear). `depth` is
/// optional scratch reused across calls. Triangles with a vertex at or behind the eye (w <= 1e-6) are skipped.
/// `ids` (optional) receives W*H winning MeshVertex::id values (0 where nothing was drawn).
inline void rasterMesh(const Mesh3D& m, Image& out, std::vector<f32>* depthScratch = nullptr, std::vector<u32>* ids = nullptr) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    using namespace mesh3d_detail;
    const MeshFrame& f = m.frame;
    const int W = int(f.width), H = int(f.height);
    out.resize(W, H, f.clear);
    std::vector<f32> local;
    std::vector<f32>& depth = depthScratch ? *depthScratch : local;
    depth.assign(std::size_t(W) * std::size_t(H), 1.f);
    if (ids) ids->assign(std::size_t(W) * std::size_t(H), 0u);
    std::vector<Sv> sv(m.vertices.size());
    const f32* M = f.mvp;
    for (std::size_t i = 0; i < m.vertices.size(); ++i) {
        const MeshVertex& v = m.vertices[i];
        const f32 cx = M[0] * v.x + M[4] * v.y + M[8] * v.z + M[12];
        const f32 cy = M[1] * v.x + M[5] * v.y + M[9] * v.z + M[13];
        const f32 cz = M[2] * v.x + M[6] * v.y + M[10] * v.z + M[14];
        const f32 cw = M[3] * v.x + M[7] * v.y + M[11] * v.z + M[15];
        Sv& s = sv[i];
        s.ok = cw > 1e-6f;
        if (!s.ok) continue;
        const f32 px = (cx / cw * 0.5f + 0.5f) * f32(W), py = (cy / cw * 0.5f + 0.5f) * f32(H);
        s.fx = std::nearbyint(px * 256.f), s.fy = std::nearbyint(py * 256.f); // round half to even (as the rasteriser)
        s.x = (long long)s.fx, s.y = (long long)s.fy;
        s.z = cz / cw;
        f32 c[4];
        meshShade(v, f, c);
        s.r = c[0], s.g = c[1], s.b = c[2], s.a = c[3];
    }
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const u32 i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
        if (i0 >= sv.size() || i1 >= sv.size() || i2 >= sv.size()) continue;
        const Sv* a = &sv[i0];
        const Sv* b = &sv[i1];
        const Sv* c = &sv[i2];
        if (!a->ok || !b->ok || !c->ok) continue;
        const Sv &oa = *a, &ob = *b, &oc = *c; // index order, for meshDepth
        const u32 id = m.vertices[i0].id;
        long long area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
        if (area == 0) continue;
        if (area < 0) std::swap(b, c), area = -area; // two-sided: orient so the interior is positive
        const long long minX = std::max<long long>(0, (std::min({a->x, b->x, c->x}) - 128) >> 8);
        const long long maxX = std::min<long long>(W - 1, (std::max({a->x, b->x, c->x}) + 128) >> 8);
        const long long minY = std::max<long long>(0, (std::min({a->y, b->y, c->y}) - 128) >> 8);
        const long long maxY = std::min<long long>(H - 1, (std::max({a->y, b->y, c->y}) + 128) >> 8);
        if (minX > maxX || minY > maxY) continue;
        // edge e(p) = (q1 - q0) x (p - q0); top-left rule in y-down space: an edge owns its pixel centres when it
        // is a "top" edge (horizontal, interior below) or a "left" edge (interior to the right).
        auto edge = [](const Sv* p0, const Sv* p1, long long px, long long py) {
            return (p1->x - p0->x) * (py - p0->y) - (p1->y - p0->y) * (px - p0->x);
        };
        auto bias = [](const Sv* p0, const Sv* p1) {
            const long long dx = p1->x - p0->x, dy = p1->y - p0->y;
            const bool topLeft = (dy == 0 && dx > 0) || dy < 0;
            return topLeft ? 0LL : -1LL;
        };
        const long long b0 = bias(b, c), b1 = bias(c, a), b2 = bias(a, b);
        const double inv = 1.0 / double(area);
        for (long long y = minY; y <= maxY; ++y) {
            const long long py = y * 256 + 128;
            for (long long x = minX; x <= maxX; ++x) {
                const long long px = x * 256 + 128;
                const long long w0 = edge(b, c, px, py), w1 = edge(c, a, px, py), w2 = edge(a, b, px, py);
                if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
                const f32 l0 = f32(double(w0) * inv), l1 = f32(double(w1) * inv), l2 = 1.f - l0 - l1;
                const f32 z = meshDepth(oa, ob, oc, f32(px), f32(py));
                if (!(z >= 0.f && z <= 1.f)) continue; // NaN too
                f32& d = depth[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                if (!(z < d)) continue;
                d = z;
                if (ids) (*ids)[std::size_t(y) * std::size_t(W) + std::size_t(x)] = id;
                const f32 al = l0 * a->a + l1 * b->a + l2 * c->a;
                const u32 R = unorm((l0 * a->r + l1 * b->r + l2 * c->r) * al), G = unorm((l0 * a->g + l1 * b->g + l2 * c->g) * al);
                const u32 B = unorm((l0 * a->b + l1 * b->b + l2 * c->b) * al), A = unorm(al);
                out.pixels[std::size_t(y) * std::size_t(W) + std::size_t(x)] = R | (G << 8) | (B << 16) | (A << 24);
            }
        }
    }
}

} // namespace fuse::ui
