// Screen-space radiance cascades: CPU reference, brute force and metrics. See ssrc_reference.hpp.
//
// trace_segment / ssrc_cascade_record / ssrc_gather_pixel are the twins of shaders/ssrc/src_common.* /
// src_cascade.* / src_gather.*: the same f32 operations in the same order (the kernels are built with `precise` /
// -fp-mode precise, so no multiply-add is contracted on either side; only +, -, *, /, sqrt, floor / truncation,
// min / max and comparisons, all IEEE-exact on both).
#include <fuse/renderer/ssrc/ssrc_reference.hpp>

#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/ssfx/ssgi_kernel.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace fuse::renderer::ssrc {

namespace {

struct F3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
};

inline F3 add(const F3& a, const F3& b) { return F3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline F3 sub(const F3& a, const F3& b) { return F3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline F3 scale(const F3& v, f32 s) { return F3{v.x * s, v.y * s, v.z * s}; }
inline f32 dot(const F3& a, const F3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline F3 normalize(const F3& v) {
    const f32 len = std::sqrt(dot(v, v));
    if (len < 1e-8f) {
        return F3{};
    }
    const f32 inv = 1.f / len;
    return scale(v, inv);
}

inline void load4(const f32* p, u64 index, f32 (&v)[4]) {
    const f32* q = p + index * 4u;
    v[0] = q[0];
    v[1] = q[1];
    v[2] = q[2];
    v[3] = q[3];
}

bool isPow2(u32 v) { return v != 0u && (v & (v - 1u)) == 0u; }

u32 clampIndex(i32 v, u32 count) {
    if (v < 0) {
        return 0u;
    }
    const u32 u = static_cast<u32>(v);
    return u >= count ? count - 1u : u;
}

struct Bilinear {
    u32 x[2];
    u32 y[2];
    f32 w[4]; ///< (x0, y0), (x1, y0), (x0, y1), (x1, y1)
};

Bilinear bilinear(f32 px, f32 py, f32 spacing, u32 countX, u32 countY) {
    const f32 u = px / spacing - 0.5f;
    const f32 v = py / spacing - 0.5f;
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const f32 ax = u - fu;
    const f32 ay = v - fv;
    const i32 ix = static_cast<i32>(fu);
    const i32 iy = static_cast<i32>(fv);
    Bilinear b{};
    b.x[0] = clampIndex(ix, countX);
    b.x[1] = clampIndex(ix + 1, countX);
    b.y[0] = clampIndex(iy, countY);
    b.y[1] = clampIndex(iy + 1, countY);
    const f32 bx = 1.f - ax;
    const f32 by = 1.f - ay;
    b.w[0] = bx * by;
    b.w[1] = ax * by;
    b.w[2] = bx * ay;
    b.w[3] = ax * ay;
    return b;
}

// --- probes and rays ----------------------------------------------------------------------------------------
struct Probe {
    bool valid = false;
    u32 pixel = 0;
    F3 o{};
    F3 n{}; ///< camera-facing unit normal of the probe pixel
};

/// Probe (probeX, probeY) of cascade `ci`: its pixel, and the origin on that pixel's surface.
Probe probeAt(const SsrcFrameConstants& c, const SsrcView& v, u32 ci, u32 probeX, u32 probeY) {
    Probe p{};
    const f32 s = c.spacing[ci];
    const f32 gx = (static_cast<f32>(probeX) + 0.5f) * s;
    const f32 gy = (static_cast<f32>(probeY) + 0.5f) * s;
    const u32 ux = std::min(static_cast<u32>(gx), c.width - 1u);
    const u32 uy = std::min(static_cast<u32>(gy), c.height - 1u);
    p.pixel = uy * c.width + ux;
    f32 g[4];
    load4(v.geo, p.pixel, g);
    const f32 z = g[0];
    if (!(z > 0.f)) {
        return p;
    }
    const f32 a = ((static_cast<f32>(ux) + 0.5f) - c.cx) / c.fx;
    const f32 b = ((static_cast<f32>(uy) + 0.5f) - c.cy) / c.fy;
    const F3 pos{a * z, b * z, z};
    F3 n = normalize(F3{g[1], g[2], g[3]});
    if (dot(n, F3{a, b, 1.f}) > 0.f) {
        n = scale(n, -1.f);
    }
    const f32 bias = c.originBias * z;
    p.o = add(pos, scale(n, bias));
    p.n = n;
    p.valid = true;
    return p;
}

struct Ray {
    bool valid = false;
    f32 kO = 0.f;
    f32 kE = 0.f;
    F3 qO{};
    F3 qE{};
    f32 len = 0.f; ///< Euclidean screen length of the projection
};

Ray raySetup(const SsrcFrameConstants& c, const F3& o, const F3& d) {
    Ray r{};
    const f32 nearLimit = c.nearZ * 1.01f;
    if (!(o.z >= nearLimit)) {
        return r;
    }
    f32 len = c.maxDistance;
    if (d.z < 0.f) {
        const f32 toNear = (o.z - nearLimit) / (-d.z);
        len = std::min(len, toNear);
    }
    if (!(len > 1e-4f)) {
        return r;
    }
    const F3 e = add(o, scale(d, len));
    r.kO = 1.f / o.z;
    r.kE = 1.f / e.z;
    r.qO = scale(o, r.kO);
    r.qE = scale(e, r.kE);
    const f32 ox = c.fx * r.qO.x + c.cx;
    const f32 oy = c.fy * r.qO.y + c.cy;
    const f32 ex = c.fx * r.qE.x + c.cx;
    const f32 ey = c.fy * r.qE.y + c.cy;
    const f32 dx = ex - ox;
    const f32 dy = ey - oy;
    r.len = std::sqrt(dx * dx + dy * dy);
    r.valid = true;
    return r;
}

F3 pointAt(const Ray& r, f32 f) {
    const f32 k = r.kO + (r.kE - r.kO) * f;
    const f32 inv = 1.f / k;
    const f32 qx = r.qO.x + (r.qE.x - r.qO.x) * f;
    const f32 qy = r.qO.y + (r.qE.y - r.qO.y) * f;
    const f32 qz = r.qO.z + (r.qE.z - r.qO.z) * f;
    return F3{qx * inv, qy * inv, qz * inv};
}

/// The fractions of cascade ci's interval on the ray; false when the ray ended before the interval.
bool intervalOf(const SsrcFrameConstants& c, const Ray& r, u32 ci, f32& fA, f32& fB, bool& ended) {
    const f32 tS = c.tStart[ci];
    const f32 tE = c.tEnd[ci];
    if (r.len < 1e-3f) {
        if (ci != 0u) {
            return false;
        }
        fA = 0.f;
        fB = 1.f;
        ended = true;
        return true;
    }
    if (tS > 0.f && !(r.len > tS)) {
        return false;
    }
    fA = tS / r.len;
    ended = !(tE < r.len);
    fB = ended ? 1.f : tE / r.len;
    return true;
}

/// Far-field radiance along d from the view-space origin o: sky + ddgiScale x E_ddgi(world(o), world(d)).
void farField(const SsrcFrameConstants& c, const SsrcView& v, const F3& o, const F3& d, f32 (&rgb)[3]) {
    rgb[0] = c.sky[0];
    rgb[1] = c.sky[1];
    rgb[2] = c.sky[2];
    if ((c.flags & kSsrcFlagDdgi) == 0u || v.ddgi == nullptr) {
        return;
    }
    const f32* m = c.viewToWorld;
    // ssfx view (+Y down, +Z forward) -> engine view (+Y up, -Z forward) -> world.
    const F3 eo{o.x, -o.y, -o.z};
    const F3 ed{d.x, -d.y, -d.z};
    const F3 wo{(m[0] * eo.x + m[1] * eo.y + m[2] * eo.z) + c.cameraWorld[0],
                (m[4] * eo.x + m[5] * eo.y + m[6] * eo.z) + c.cameraWorld[1],
                (m[8] * eo.x + m[9] * eo.y + m[10] * eo.z) + c.cameraWorld[2]};
    const F3 wd{m[0] * ed.x + m[1] * ed.y + m[2] * ed.z, m[4] * ed.x + m[5] * ed.y + m[6] * ed.z,
                m[8] * ed.x + m[9] * ed.y + m[10] * ed.z};
    const math::Vec3 e =
        ddgi_kernel::sample_irradiance(*v.ddgi, math::Vec3{wo.x, wo.y, wo.z}, math::Vec3{wd.x, wd.y, wd.z});
    rgb[0] = rgb[0] + e.x * c.ddgiScale;
    rgb[1] = rgb[1] + e.y * c.ddgiScale;
    rgb[2] = rgb[2] + e.z * c.ddgiScale;
}

struct F4 {
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    f32 v = 0.f;
};

F4 decodeRecord(const u32* records, u64 index) {
    const u32 lo = records[index * 2u];
    const u32 hi = records[index * 2u + 1u];
    return F4{f16ToF32(lo & 0xFFFFu), f16ToF32(lo >> 16), f16ToF32(hi & 0xFFFFu), f16ToF32(hi >> 16)};
}

/// Mean of the 4 children (cascade ci + 1) of direction k on upper probe (ux, uy) over the children in the hemisphere
/// of the receiving probe's normal n (all 4, solid-angle weighted, when none is): solid-angle weights, and on cascade 0
/// (whose records only the gather reads) solid angle x cosine, so a bin's gathered contribution is the cosine
/// quadrature over its children.
F4 childAverage(const SsrcFrameConstants& c, const SsrcView& v, u32 ci, const u32* upper, u32 ux, u32 uy, u32 k,
                const F3& n) {
    const u32 res = c.dirRes[ci];
    const u32 res2 = c.dirRes[ci + 1u];
    const u32 ku = k % res;
    const u32 kv = k / res;
    const u32 base = (uy * c.probesX[ci + 1u] + ux) * (res2 * res2);
    f32 sr = 0.f;
    f32 sg = 0.f;
    f32 sb = 0.f;
    f32 sv = 0.f;
    f32 sw = 0.f;
    f32 ar = 0.f;
    f32 ag = 0.f;
    f32 ab = 0.f;
    f32 av = 0.f;
    f32 aw = 0.f;
    for (u32 j = 0; j < 4u; ++j) {
        const u32 ck = (2u * kv + (j >> 1u)) * res2 + 2u * ku + (j & 1u);
        const F4 rec = decodeRecord(upper, base + ck);
        const f32* dt = v.dirs + (static_cast<u64>(c.dirOffset[ci + 1u]) + ck) * 4u;
        const f32 w = dt[3];
        ar = ar + w * rec.r;
        ag = ag + w * rec.g;
        ab = ab + w * rec.b;
        av = av + w * rec.v;
        aw = aw + w;
        const f32 cosine = dot(n, F3{dt[0], dt[1], dt[2]});
        if (cosine > 0.f) {
            const f32 wm = ci == 0u ? w * cosine : w;
            sr = sr + wm * rec.r;
            sg = sg + wm * rec.g;
            sb = sb + wm * rec.b;
            sv = sv + wm * rec.v;
            sw = sw + wm;
        }
    }
    if (sw > 0.f) {
        return F4{sr / sw, sg / sw, sb / sw, sv / sw};
    }
    return F4{ar / aw, ag / aw, ab / aw, av / aw};
}

/// Gather weight of cascade 0 direction k for the unit normal n: the cosine-weighted solid angle of the bin, summed
/// over its 4 cascade-1 children (the bin centre alone with a single cascade).
f32 binWeight(const SsrcFrameConstants& c, const SsrcView& v, u32 k, const F3& n) {
    if (c.cascades < 2u) {
        const f32* dt = v.dirs + static_cast<u64>(k) * 4u;
        return std::max(0.f, dot(n, F3{dt[0], dt[1], dt[2]})) * dt[3];
    }
    const u32 res = c.dirRes[0];
    const u32 res2 = c.dirRes[1];
    const u32 ku = k % res;
    const u32 kv = k / res;
    f32 w = 0.f;
    for (u32 j = 0; j < 4u; ++j) {
        const u32 ck = (2u * kv + (j >> 1u)) * res2 + 2u * ku + (j & 1u);
        const f32* dt = v.dirs + (static_cast<u64>(c.dirOffset[1]) + ck) * 4u;
        w = w + std::max(0.f, dot(n, F3{dt[0], dt[1], dt[2]})) * dt[3];
    }
    return w;
}

/// Depth of pixel h's surfel (the tangent plane through its centre point, depth zc, normal n; nd = n . centre ray) on
/// the view ray through screen point (x, y); the ratio to zc is clamped to [0.5, 2] (grazing planes).
f32 patchDepth(const SsrcFrameConstants& c, f32 zc, f32 nd, const F3& n, f32 x, f32 y) {
    const f32 rx = (x - c.cx) / c.fx;
    const f32 ry = (y - c.cy) / c.fy;
    const f32 nr = dot(n, F3{rx, ry, 1.f});
    if (!(std::fabs(nr) >= 1e-6f)) {
        return zc;
    }
    f32 q = nd / nr;
    if (!(q > 0.5f)) {
        q = 0.5f;
    }
    if (q > 2.f) {
        q = 2.f;
    }
    return zc * q;
}

} // namespace

// --- f16 -----------------------------------------------------------------------------------------------------
u32 f32ToF16(f32 v) {
    f32 m = v;
    if (m > 65504.f) {
        m = 65504.f;
    }
    if (m < -65504.f) {
        m = -65504.f;
    }
    u32 u = std::bit_cast<u32>(m);
    const u32 sign = u & 0x80000000u;
    u ^= sign;
    u32 o = 0;
    if (u >= 0x47800000u) {
        o = u > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (u < 0x38800000u) {
        const f32 t = std::bit_cast<f32>(u) + 0.5f; // 0.5 = the denormal magic ((127 - 15) + (23 - 10) + 1) << 23
        o = std::bit_cast<u32>(t) - 0x3F000000u;
    } else {
        const u32 odd = (u >> 13) & 1u;
        u += 0xC8000FFFu; // ((15 - 127) << 23) + 0xfff
        u += odd;
        o = u >> 13;
    }
    return o | (sign >> 16);
}

f32 f16ToF32(u32 h) {
    const u32 sign = (h & 0x8000u) << 16;
    const u32 e = (h >> 10) & 0x1Fu;
    const u32 m = h & 0x3FFu;
    if (e == 0u) {
        const f32 f = static_cast<f32>(m) * 5.9604644775390625e-8f;
        return sign != 0u ? -f : f;
    }
    if (e == 31u) {
        return std::bit_cast<f32>(sign | 0x7F800000u | (m << 13));
    }
    return std::bit_cast<f32>(sign | ((e + 112u) << 23) | (m << 13));
}

// --- layout --------------------------------------------------------------------------------------------------
bool resolve_layout(const SsrcSettings& s, u32 width, u32 height, SsrcFrameConstants& c, SsrcLayoutInfo* info) {
    if (width == 0u || height == 0u || !isPow2(s.probeSpacing0) || s.probeSpacing0 > 64u || s.dirRes0 == 0u ||
        s.dirRes0 > 64u || !(s.interval0 >= 0.f) || !(s.stride > 0.f) || !(s.maxDistance > 0.f)) {
        return false;
    }
    c.width = width;
    c.height = height;
    c.flags = 0u;
    if (s.bilinearFix) {
        c.flags |= kSsrcFlagBilinearFix;
    }
    if (s.ddgi) {
        c.flags |= kSsrcFlagDdgi;
    }
    if (s.compose) {
        c.flags |= kSsrcFlagCompose;
    }
    c.stride = std::max(0.25f, s.stride);
    c.thickness = std::max(0.f, s.thickness);
    c.thicknessSlope = std::max(0.f, s.thicknessSlope);
    c.maxDistance = std::max(1e-3f, s.maxDistance);
    c.originBias = std::max(0.f, s.originBias);
    c.planeTolerance = std::max(0.f, s.planeTolerance);
    c.intensity = std::max(0.f, s.intensity);
    c.ddgiScale = std::max(0.f, s.ddgiScale);
    for (u32 i = 0; i < 3u; ++i) {
        c.sky[i] = std::max(0.f, s.sky[i]);
    }
    c.sky[3] = 0.f;

    const f32 r0 = s.interval0 > 0.f ? s.interval0 : 2.f * static_cast<f32>(s.probeSpacing0);
    const f64 diagonal = std::sqrt(static_cast<f64>(width) * width + static_cast<f64>(height) * height);
    u32 n = 0;
    if (s.cascadeCount != 0u) {
        n = std::min(s.cascadeCount, kSsrcMaxCascades);
    } else {
        f64 reach = 0.0;
        f64 length = r0;
        while (n < kSsrcMaxCascades && reach < diagonal) {
            reach += length;
            length *= 4.0;
            ++n;
        }
    }
    SsrcLayoutInfo li{};
    u32 spacing = s.probeSpacing0;
    u32 res = s.dirRes0;
    f32 t = 0.f;
    f32 length = r0;
    u32 built = 0;
    for (u32 i = 0; i < kSsrcMaxCascades; ++i) {
        c.probesX[i] = c.probesY[i] = c.dirRes[i] = c.dirOffset[i] = 0u;
        c.spacing[i] = c.tStart[i] = c.tEnd[i] = c.reserved5[i] = 0.f;
    }
    for (u32 i = 0; i < n; ++i) {
        const u32 px = (width + spacing - 1u) / spacing;
        const u32 py = (height + spacing - 1u) / spacing;
        const u64 records = static_cast<u64>(px) * py * res * res;
        if (res > 1024u || records > 0xFFFFFFFFull) {
            break;
        }
        c.probesX[i] = px;
        c.probesY[i] = py;
        c.dirRes[i] = res;
        c.dirOffset[i] = li.dirTotal;
        c.spacing[i] = static_cast<f32>(spacing);
        c.tStart[i] = t;
        c.tEnd[i] = t + length;
        li.records[i] = records;
        li.maxRecords = std::max(li.maxRecords, records);
        li.totalRecords += records;
        li.dirTotal += res * res;
        built = i + 1u;
        t = c.tEnd[i];
        length *= 4.f;
        spacing = std::min(spacing * 2u, 1u << 30);
        res *= 2u;
    }
    if (built == 0u) {
        return false;
    }
    c.cascades = built;
    c.aoCascades = s.aoCascades == 0u ? built : std::min(s.aoCascades, built);
    if (info != nullptr) {
        *info = li;
    }
    return true;
}

bool resolve_constants(const SsrcSettings& settings, const ssfx_gpu::SsfxCameraDesc& camera, const f32 (&ambient)[3],
                       u32 width, u32 height, SsrcFrameConstants& out, SsrcLayoutInfo* info) {
    ssfx::SsfxCamera cam{};
    if (!ssfx_gpu::camera_from_projection(camera.proj, width, height, cam) || !(camera.nearPlane > 0.f)) {
        return false;
    }
    SsrcFrameConstants c{};
    if (!resolve_layout(settings, width, height, c, info)) {
        return false;
    }
    if (camera.reversedZ) {
        c.flags |= kSsrcFlagReversedZ;
    }
    c.fx = cam.fx;
    c.fy = cam.fy;
    c.cx = cam.cx;
    c.cy = cam.cy;
    c.nearZ = cam.near_z;
    c.nearPlane = camera.nearPlane;
    c.farPlane = camera.farPlane;
    const f32* m = camera.view;
    for (u32 col = 0; col < 3u; ++col) {
        for (u32 row = 0; row < 3u; ++row) {
            c.viewRot[col * 4u + row] = m[col * 4u + row];
            // (R^T)[row][col] = R[col][row] = m[row * 4 + col]
            c.viewToWorld[row * 4u + col] = m[row * 4u + col];
        }
    }
    for (u32 r = 0; r < 3u; ++r) {
        const f64 w = static_cast<f64>(m[r * 4u + 0u]) * m[12] + static_cast<f64>(m[r * 4u + 1u]) * m[13] +
                      static_cast<f64>(m[r * 4u + 2u]) * m[14];
        c.cameraWorld[r] = static_cast<f32>(-w);
    }
    c.ambient[0] = ambient[0];
    c.ambient[1] = ambient[1];
    c.ambient[2] = ambient[2];
    out = c;
    return true;
}

void buildDirectionTable(const SsrcFrameConstants& c, std::vector<f32>& out) {
    u32 total = 0;
    for (u32 i = 0; i < c.cascades; ++i) {
        total += c.dirRes[i] * c.dirRes[i];
    }
    out.assign(static_cast<usize>(total) * 4u, 0.f);
    if (c.cascades == 0u) {
        return;
    }
    // Solid angle of each octahedral texel: dOmega = dx dy / |p|^3 for the (unfolded) octahedron point p (the
    // lower-hemisphere fold is area preserving). Midpoint rule at the finest cascade (sub x sub samples per texel),
    // every coarser texel the sum of its 2 x 2 children (so a parent's solid angle is exactly its children's), all
    // renormalised to 4 pi.
    std::vector<std::vector<f64>> omega(c.cascades);
    const u32 top = c.cascades - 1u;
    const u32 fine = c.dirRes[top];
    const u32 sub = std::max(2u, 64u / fine);
    omega[top].assign(static_cast<usize>(fine) * fine, 0.0);
    f64 sum = 0.0;
    for (u32 v = 0; v < fine; ++v) {
        for (u32 u = 0; u < fine; ++u) {
            f64 acc = 0.0;
            for (u32 sv = 0; sv < sub; ++sv) {
                for (u32 su = 0; su < sub; ++su) {
                    const f64 x = ((u + (su + 0.5) / sub) / fine) * 2.0 - 1.0;
                    const f64 y = ((v + (sv + 0.5) / sub) / fine) * 2.0 - 1.0;
                    f64 px = x;
                    f64 py = y;
                    const f64 pz = 1.0 - std::fabs(x) - std::fabs(y);
                    if (pz < 0.0) {
                        px = (1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0);
                        py = (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0);
                    }
                    const f64 l = std::sqrt(px * px + py * py + pz * pz);
                    acc += 1.0 / (l * l * l);
                }
            }
            const f64 cell = (2.0 / fine) / sub;
            omega[top][static_cast<usize>(v) * fine + u] = acc * cell * cell;
            sum += acc * cell * cell;
        }
    }
    for (u32 i = top; i-- > 0u;) {
        const u32 res = c.dirRes[i];
        const u32 res2 = c.dirRes[i + 1u];
        omega[i].assign(static_cast<usize>(res) * res, 0.0);
        for (u32 v = 0; v < res; ++v) {
            for (u32 u = 0; u < res; ++u) {
                f64 acc = 0.0;
                for (u32 j = 0; j < 4u; ++j) {
                    acc += omega[i + 1u][static_cast<usize>(2u * v + (j >> 1u)) * res2 + 2u * u + (j & 1u)];
                }
                omega[i][static_cast<usize>(v) * res + u] = acc;
            }
        }
    }
    const f64 norm = 4.0 * std::numbers::pi / sum;
    for (u32 i = 0; i < c.cascades; ++i) {
        const u32 res = c.dirRes[i];
        for (u32 v = 0; v < res; ++v) {
            for (u32 u = 0; u < res; ++u) {
                const f64 x = ((u + 0.5) / res) * 2.0 - 1.0;
                const f64 y = ((v + 0.5) / res) * 2.0 - 1.0;
                f64 px = x;
                f64 py = y;
                const f64 pz = 1.0 - std::fabs(x) - std::fabs(y);
                if (pz < 0.0) {
                    px = (1.0 - std::fabs(y)) * (x >= 0.0 ? 1.0 : -1.0);
                    py = (1.0 - std::fabs(x)) * (y >= 0.0 ? 1.0 : -1.0);
                }
                const f64 l = std::sqrt(px * px + py * py + pz * pz);
                f32* d = out.data() + (static_cast<usize>(c.dirOffset[i]) + v * res + u) * 4u;
                d[0] = static_cast<f32>(px / l);
                d[1] = static_cast<f32>(py / l);
                d[2] = static_cast<f32>(pz / l);
                d[3] = static_cast<f32>(omega[i][static_cast<usize>(v) * res + u] * norm);
            }
        }
    }
}

// --- kernels -------------------------------------------------------------------------------------------------
SsrcSegment trace_segment(const SsrcFrameConstants& c, const SsrcView& v, const f32 (&a)[3], const f32 (&b)[3],
                          u32 startPixel, SsrcCounters* counters) {
    SsrcSegment seg{};
    const f32 ka = 1.f / a[2];
    const f32 kb = 1.f / b[2];
    const f32 ax = c.fx * (a[0] * ka) + c.cx;
    const f32 ay = c.fy * (a[1] * ka) + c.cy;
    const f32 bx = c.fx * (b[0] * kb) + c.cx;
    const f32 by = c.fy * (b[1] * kb) + c.cy;
    const f32 ddx = bx - ax;
    const f32 ddy = by - ay;
    const f32 len = std::max(std::fabs(ddx), std::fabs(ddy));
    if (counters != nullptr) {
        ++counters->segments;
    }
    if (!(len >= 1e-3f)) {
        return seg;
    }
    const f32 n = std::min(std::ceil(len / c.stride), 1048576.f);
    const u32 steps = static_cast<u32>(n);
    const f32 fStep = c.stride / len;
    const f32 fw = static_cast<f32>(c.width);
    const f32 fh = static_cast<f32>(c.height);
    f32 prevZ = a[2];
    f32 prevX = ax;
    f32 prevY = ay;
    const F3 dir{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    for (u32 i = 1u; i <= steps; ++i) {
        const f32 f = std::min(1.f, static_cast<f32>(i) * fStep);
        const f32 sx = ax + ddx * f;
        const f32 sy = ay + ddy * f;
        if (counters != nullptr) {
            ++counters->samples;
        }
        if (!(sx >= 0.f && sy >= 0.f && sx < fw && sy < fh)) {
            seg.state = kSsrcEscaped;
            seg.f = f;
            return seg;
        }
        const f32 rayZ = 1.f / (ka + (kb - ka) * f);
        const u32 hx = static_cast<u32>(sx);
        const u32 hy = static_cast<u32>(sy);
        const u32 pixel = hy * c.width + hx;
        if (pixel != startPixel) {
            f32 g[4];
            load4(v.geo, pixel, g);
            const f32 zc = g[0];
            if (zc > 0.f) {
                const F3 hn{g[1], g[2], g[3]};
                const F3 rd{((static_cast<f32>(hx) + 0.5f) - c.cx) / c.fx, ((static_cast<f32>(hy) + 0.5f) - c.cy) / c.fy,
                            1.f};
                const f32 nd = dot(hn, rd);
                const f32 d0 = prevZ - patchDepth(c, zc, nd, hn, prevX, prevY);
                const f32 d1 = rayZ - patchDepth(c, zc, nd, hn, sx, sy);
                const f32 th = c.thickness + c.thicknessSlope * zc;
                if ((d1 >= 0.f && d1 <= th) || (d0 < 0.f && d1 > th) || (d0 > th && d1 < 0.f)) {
                    seg.state = kSsrcHit;
                    seg.f = f;
                    const f32 alongRay = dot(hn, dir);
                    const bool front = nd > 0.f ? alongRay > 0.f : alongRay < 0.f;
                    if (front) {
                        f32 l[4];
                        load4(v.lit, pixel, l);
                        seg.r = l[0];
                        seg.g = l[1];
                        seg.b = l[2];
                    }
                    return seg;
                }
            }
        }
        prevZ = rayZ;
        prevX = sx;
        prevY = sy;
    }
    return seg;
}

void ssrc_cascade_record(const SsrcFrameConstants& c, const SsrcView& v, u32 ci, const u32* upper, u32* dst, u32 index,
                         SsrcCounters* counters) {
    const u32 res = c.dirRes[ci];
    const u32 count = res * res;
    const u32 k = index % count;
    const u32 probe = index / count;
    const u32 probeX = probe % c.probesX[ci];
    const u32 probeY = probe / c.probesX[ci];
    const f32* dt = v.dirs + (static_cast<u64>(c.dirOffset[ci]) + k) * 4u;
    const F3 d{dt[0], dt[1], dt[2]};
    const bool hasUpper = ci + 1u < c.cascades;
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    f32 vis = 1.f;
    f32 farRgb[3];
    const Probe p = probeAt(c, v, ci, probeX, probeY);
    f32 fA = 0.f;
    f32 fB = 1.f;
    bool ended = false;
    Ray ray{};
    if (p.valid) {
        ray = raySetup(c, p.o, d);
    }
    if (!p.valid || !ray.valid || !intervalOf(c, ray, ci, fA, fB, ended)) {
        farField(c, v, p.valid ? p.o : F3{}, d, farRgb);
        r = farRgb[0];
        g = farRgb[1];
        b = farRgb[2];
        vis = 1.f;
    } else {
        const F3 A = pointAt(ray, fA);
        const F3 B = pointAt(ray, fB);
        const f32 a3[3] = {A.x, A.y, A.z};
        const f32 b3[3] = {B.x, B.y, B.z};
        bool fallback = !hasUpper;
        // Cascade 0 bins centred below the probe's horizon are gathered only through their children above it: merge
        // those (hemisphere-masked) without tracing the centre direction into the surface.
        const bool below = ci == 0u && !(dot(p.n, d) > 0.f);
        if (hasUpper) {
            const f32 gx = (static_cast<f32>(probeX) + 0.5f) * c.spacing[ci];
            const f32 gy = (static_cast<f32>(probeY) + 0.5f) * c.spacing[ci];
            const Bilinear bl = bilinear(gx, gy, c.spacing[ci + 1u], c.probesX[ci + 1u], c.probesY[ci + 1u]);
            // Parents: valid (not sky) and, plane-aware, near the child's tangent plane (all valid ones when none is).
            Probe qs[4];
            bool use[4] = {false, false, false, false};
            bool anyPlane = false;
            for (u32 j = 0; j < 4u; ++j) {
                if (!(bl.w[j] > 0.f)) {
                    continue;
                }
                qs[j] = probeAt(c, v, ci + 1u, bl.x[j & 1u], bl.y[j >> 1u]);
                if (!qs[j].valid) {
                    continue;
                }
                const f32 dist = std::fabs(dot(qs[j].n, sub(p.o, qs[j].o)));
                use[j] = !(c.planeTolerance > 0.f) || dist <= c.planeTolerance * p.o.z;
                anyPlane = anyPlane || use[j];
            }
            if (!anyPlane) {
                for (u32 j = 0; j < 4u; ++j) {
                    use[j] = bl.w[j] > 0.f && qs[j].valid;
                }
            }
            f32 sr = 0.f;
            f32 sg = 0.f;
            f32 sb = 0.f;
            f32 sv = 0.f;
            f32 sw = 0.f;
            if ((c.flags & kSsrcFlagBilinearFix) != 0u && !below) {
                for (u32 j = 0; j < 4u; ++j) {
                    if (!use[j]) {
                        continue;
                    }
                    const f32 w = bl.w[j];
                    const u32 ux = bl.x[j & 1u];
                    const u32 uy = bl.y[j >> 1u];
                    const Probe& q = qs[j];
                    f32 t3[3] = {B.x, B.y, B.z};
                    bool toParent = false;
                    const Ray qr = raySetup(c, q.o, d);
                    if (qr.valid && qr.len >= 1e-3f && c.tEnd[ci] < qr.len) {
                        const F3 T = pointAt(qr, c.tEnd[ci] / qr.len);
                        t3[0] = T.x;
                        t3[1] = T.y;
                        t3[2] = T.z;
                        toParent = true;
                    }
                    const SsrcSegment s = trace_segment(c, v, a3, t3, p.pixel, counters);
                    F4 val{};
                    if (s.state == kSsrcHit) {
                        val = F4{s.r, s.g, s.b, 0.f};
                    } else if (s.state == kSsrcEscaped || (!toParent && ended)) {
                        farField(c, v, p.o, d, farRgb);
                        val = F4{farRgb[0], farRgb[1], farRgb[2], 1.f};
                    } else {
                        val = childAverage(c, v, ci, upper, ux, uy, k, p.n);
                    }
                    sr = sr + w * val.r;
                    sg = sg + w * val.g;
                    sb = sb + w * val.b;
                    sv = sv + w * val.v;
                    sw = sw + w;
                }
                if (sw > 0.f) {
                    r = sr / sw;
                    g = sg / sw;
                    b = sb / sw;
                    vis = sv / sw;
                } else {
                    fallback = true;
                }
            } else {
                const SsrcSegment s = below ? SsrcSegment{} : trace_segment(c, v, a3, b3, p.pixel, counters);
                if (s.state == kSsrcHit) {
                    r = s.r;
                    g = s.g;
                    b = s.b;
                    vis = 0.f;
                } else if (s.state == kSsrcEscaped || (ended && !below)) {
                    farField(c, v, p.o, d, farRgb);
                    r = farRgb[0];
                    g = farRgb[1];
                    b = farRgb[2];
                    vis = 1.f;
                } else {
                    for (u32 j = 0; j < 4u; ++j) {
                        if (!use[j]) {
                            continue;
                        }
                        const f32 w = bl.w[j];
                        const u32 ux = bl.x[j & 1u];
                        const u32 uy = bl.y[j >> 1u];
                        const F4 val = childAverage(c, v, ci, upper, ux, uy, k, p.n);
                        sr = sr + w * val.r;
                        sg = sg + w * val.g;
                        sb = sb + w * val.b;
                        sv = sv + w * val.v;
                        sw = sw + w;
                    }
                    if (sw > 0.f) {
                        r = sr / sw;
                        g = sg / sw;
                        b = sb / sw;
                        vis = sv / sw;
                    } else {
                        farField(c, v, p.o, d, farRgb);
                        r = farRgb[0];
                        g = farRgb[1];
                        b = farRgb[2];
                        vis = 1.f;
                    }
                }
            }
        }
        if (fallback) {
            const SsrcSegment s = trace_segment(c, v, a3, b3, p.pixel, counters);
            if (s.state == kSsrcHit) {
                r = s.r;
                g = s.g;
                b = s.b;
                vis = 0.f;
            } else {
                farField(c, v, p.o, d, farRgb);
                r = farRgb[0];
                g = farRgb[1];
                b = farRgb[2];
                vis = 1.f;
            }
        }
    }
    if (ci >= c.aoCascades) {
        vis = 1.f;
    }
    dst[static_cast<u64>(index) * 2u] = f32ToF16(r) | (f32ToF16(g) << 16);
    dst[static_cast<u64>(index) * 2u + 1u] = f32ToF16(b) | (f32ToF16(vis) << 16);
}

void ssrc_gather_pixel(const SsrcFrameConstants& c, const SsrcView& v, const u32* records0, f32* indirect, f32* image,
                       u32 index) {
    const u32 x = index % c.width;
    const u32 y = index / c.width;
    f32 g[4];
    load4(v.geo, index, g);
    f32 l[4];
    load4(v.lit, index, l);
    f32 ir = 0.f;
    f32 ig = 0.f;
    f32 ib = 0.f;
    f32 ao = 1.f;
    f32 cr = l[0];
    f32 cg = l[1];
    f32 cb = l[2];
    if (g[0] > 0.f) {
        const f32 a = ((static_cast<f32>(x) + 0.5f) - c.cx) / c.fx;
        const f32 b = ((static_cast<f32>(y) + 0.5f) - c.cy) / c.fy;
        F3 n = normalize(F3{g[1], g[2], g[3]});
        if (dot(n, F3{a, b, 1.f}) > 0.f) {
            n = scale(n, -1.f);
        }
        const u32 count = c.dirRes[0] * c.dirRes[0];
        f32 cw = 0.f;
        for (u32 k = 0; k < count; ++k) {
            cw = cw + binWeight(c, v, k, n);
        }
        const Bilinear bl = bilinear(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, c.spacing[0],
                                     c.probesX[0], c.probesY[0]);
        f32 sr = 0.f;
        f32 sg = 0.f;
        f32 sb = 0.f;
        f32 sv = 0.f;
        f32 sw = 0.f;
        for (u32 j = 0; j < 4u; ++j) {
            const f32 w = bl.w[j];
            if (!(w > 0.f)) {
                continue;
            }
            const u32 ux = bl.x[j & 1u];
            const u32 uy = bl.y[j >> 1u];
            const Probe q = probeAt(c, v, 0u, ux, uy);
            if (!q.valid) {
                continue;
            }
            const u32 base = (uy * c.probesX[0] + ux) * count;
            f32 pr = 0.f;
            f32 pg = 0.f;
            f32 pb = 0.f;
            f32 pv = 0.f;
            for (u32 k = 0; k < count; ++k) {
                const f32 wk = binWeight(c, v, k, n);
                const F4 rec = decodeRecord(records0, base + k);
                pr = pr + wk * rec.r;
                pg = pg + wk * rec.g;
                pb = pb + wk * rec.b;
                pv = pv + wk * rec.v;
            }
            sr = sr + w * pr;
            sg = sg + w * pg;
            sb = sb + w * pb;
            sv = sv + w * pv;
            sw = sw + w;
        }
        if (sw > 0.f && cw > 0.f) {
            const f32 den = sw * cw;
            f32 dif[4];
            load4(v.diffuse, index, dif);
            ir = dif[0] * (sr / den) * c.intensity;
            ig = dif[1] * (sg / den) * c.intensity;
            ib = dif[2] * (sb / den) * c.intensity;
            ao = sv / den;
            f32 alb[4];
            load4(v.albedo, index, alb);
            const f32 k = dif[3] * (ao - 1.f);
            cr = (l[0] + ir) + c.ambient[0] * alb[0] * k;
            cg = (l[1] + ig) + c.ambient[1] * alb[1] * k;
            cb = (l[2] + ib) + c.ambient[2] * alb[2] * k;
        }
    }
    f32* o = indirect + static_cast<u64>(index) * 4u;
    o[0] = ir;
    o[1] = ig;
    o[2] = ib;
    o[3] = ao;
    f32* im = image + static_cast<u64>(index) * 4u;
    if ((c.flags & kSsrcFlagCompose) != 0u) {
        im[0] = std::max(cr, 0.f);
        im[1] = std::max(cg, 0.f);
        im[2] = std::max(cb, 0.f);
        im[3] = l[3];
    } else {
        im[0] = ir;
        im[1] = ig;
        im[2] = ib;
        im[3] = ao;
    }
}

// --- solver --------------------------------------------------------------------------------------------------
void inputsFromPrepared(const ssfx_gpu::SsfxPreparedFrame& in, SsrcInputs& out) {
    const usize n = static_cast<usize>(in.width) * in.height;
    out.width = in.width;
    out.height = in.height;
    out.geo.resize(n * 4u);
    out.lit.resize(n * 4u);
    out.diffuse.resize(n * 4u);
    out.albedo.resize(n * 4u);
    for (usize i = 0; i < n; ++i) {
        f32* g = out.geo.data() + i * 4u;
        g[0] = in.prepared[i].x;
        g[1] = in.normal[i].x;
        g[2] = in.normal[i].y;
        g[3] = in.normal[i].z;
        f32* l = out.lit.data() + i * 4u;
        l[0] = in.radiance[i].x;
        l[1] = in.radiance[i].y;
        l[2] = in.radiance[i].z;
        l[3] = in.litAlpha[i];
        f32* d = out.diffuse.data() + i * 4u;
        d[0] = in.diffuse[i].x;
        d[1] = in.diffuse[i].y;
        d[2] = in.diffuse[i].z;
        d[3] = in.prepared[i].w;
        f32* a = out.albedo.data() + i * 4u;
        a[0] = in.albedo[i].x;
        a[1] = in.albedo[i].y;
        a[2] = in.albedo[i].z;
        a[3] = 0.f;
    }
}

bool SsrcCpuSolver::solve(const SsrcFrameConstants& c, const SsrcInputs& in, const ddgi_kernel::VolumeView* ddgi,
                          std::vector<f32>& indirect, std::vector<f32>& image, SsrcStats* stats) {
    const usize n = static_cast<usize>(in.width) * in.height;
    if (n == 0u || in.width != c.width || in.height != c.height || c.cascades == 0u || in.geo.size() != n * 4u ||
        in.lit.size() != n * 4u || in.diffuse.size() != n * 4u || in.albedo.size() != n * 4u) {
        return false;
    }
    buildDirectionTable(c, m_dirs);
    u64 maxRecords = 0;
    for (u32 i = 0; i < c.cascades; ++i) {
        maxRecords = std::max(maxRecords, static_cast<u64>(c.probesX[i]) * c.probesY[i] * c.dirRes[i] * c.dirRes[i]);
    }
    for (std::vector<u32>& r : m_records) {
        if (r.size() < maxRecords * 2u) {
            r.resize(static_cast<usize>(maxRecords) * 2u);
        }
    }
    SsrcView view{};
    view.geo = in.geo.data();
    view.lit = in.lit.data();
    view.diffuse = in.diffuse.data();
    view.albedo = in.albedo.data();
    view.dirs = m_dirs.data();
    view.ddgi = ddgi;
    if (stats != nullptr) {
        *stats = SsrcStats{};
    }
    const u32* upper = nullptr;
    for (u32 ci = c.cascades; ci-- > 0u;) {
        const u32 count = c.probesX[ci] * c.probesY[ci] * c.dirRes[ci] * c.dirRes[ci];
        u32* dst = m_records[ci & 1u].data();
        SsrcCounters counters{};
        for (u32 i = 0; i < count; ++i) {
            ssrc_cascade_record(c, view, ci, upper, dst, i, stats != nullptr ? &counters : nullptr);
        }
        if (stats != nullptr) {
            stats->samplesPerCascade[ci] = counters.samples;
            stats->samples += counters.samples;
            stats->segments += counters.segments;
            stats->records += count;
        }
        upper = dst;
    }
    indirect.resize(n * 4u);
    image.resize(n * 4u);
    for (u32 i = 0; i < static_cast<u32>(n); ++i) {
        ssrc_gather_pixel(c, view, upper, indirect.data(), image.data(), i);
    }
    return true;
}

void bruteForce(const SsrcFrameConstants& c, const SsrcInputs& in, const ddgi_kernel::VolumeView* ddgi, u32 raysSqrt,
                std::vector<f32>& indirect, SsrcStats* stats) {
    const usize n = static_cast<usize>(in.width) * in.height;
    indirect.assign(n * 4u, 0.f);
    SsrcView view{};
    view.geo = in.geo.data();
    view.lit = in.lit.data();
    view.diffuse = in.diffuse.data();
    view.albedo = in.albedo.data();
    view.ddgi = ddgi;
    const u32 k = std::max(1u, raysSqrt);
    const f32 aoRadius = c.aoCascades > 0u ? c.tEnd[c.aoCascades - 1u] : 0.f;
    SsrcCounters counters{};
    for (u32 y = 0; y < in.height; ++y) {
        for (u32 x = 0; x < in.width; ++x) {
            const u32 index = y * in.width + x;
            f32* o = indirect.data() + static_cast<usize>(index) * 4u;
            o[3] = 1.f;
            const f32* g = in.geo.data() + static_cast<usize>(index) * 4u;
            const f32 z = g[0];
            if (!(z > 0.f)) {
                continue;
            }
            const f32 a = ((static_cast<f32>(x) + 0.5f) - c.cx) / c.fx;
            const f32 b = ((static_cast<f32>(y) + 0.5f) - c.cy) / c.fy;
            F3 nrm = normalize(F3{g[1], g[2], g[3]});
            if (dot(nrm, F3{a, b, 1.f}) > 0.f) {
                nrm = scale(nrm, -1.f);
            }
            const F3 origin = add(F3{a * z, b * z, z}, scale(nrm, c.originBias * z));
            f64 sr = 0.0;
            f64 sg = 0.0;
            f64 sb = 0.0;
            f64 sv = 0.0;
            for (u32 i = 0; i < k; ++i) {
                for (u32 j = 0; j < k; ++j) {
                    const math::Vec3 s =
                        ssfx::ssgi_kernel::sample_direction(math::Vec3{nrm.x, nrm.y, nrm.z}, x, y, i, j, k);
                    const F3 d = normalize(F3{s.x, s.y, s.z});
                    f32 farRgb[3];
                    const Ray ray = raySetup(c, origin, d);
                    SsrcSegment seg{};
                    seg.state = kSsrcEscaped;
                    if (ray.valid) {
                        const F3 A = pointAt(ray, 0.f);
                        const F3 B = pointAt(ray, 1.f);
                        const f32 a3[3] = {A.x, A.y, A.z};
                        const f32 b3[3] = {B.x, B.y, B.z};
                        seg = trace_segment(c, view, a3, b3, index, &counters);
                    }
                    if (seg.state == kSsrcHit) {
                        sr += seg.r;
                        sg += seg.g;
                        sb += seg.b;
                        sv += (seg.f * ray.len < aoRadius) ? 0.0 : 1.0;
                    } else {
                        farField(c, view, origin, d, farRgb);
                        sr += farRgb[0];
                        sg += farRgb[1];
                        sb += farRgb[2];
                        sv += 1.0;
                    }
                }
            }
            const f64 inv = 1.0 / (static_cast<f64>(k) * k);
            const f32* dif = in.diffuse.data() + static_cast<usize>(index) * 4u;
            o[0] = static_cast<f32>(dif[0] * sr * inv * c.intensity);
            o[1] = static_cast<f32>(dif[1] * sg * inv * c.intensity);
            o[2] = static_cast<f32>(dif[2] * sb * inv * c.intensity);
            o[3] = static_cast<f32>(sv * inv);
        }
    }
    if (stats != nullptr) {
        *stats = SsrcStats{};
        stats->samples = counters.samples;
        stats->segments = counters.segments;
    }
}

// --- metrics -------------------------------------------------------------------------------------------------
SsrcErrorMetrics compareIndirect(const std::vector<f32>& test, const std::vector<f32>& ref, const std::vector<u8>& mask) {
    SsrcErrorMetrics m{};
    const usize n = std::min(test.size(), ref.size()) / 4u;
    f64 sumAbs = 0.0;
    f64 sumRef = 0.0;
    f64 sumSq = 0.0;
    f64 meanRef = 0.0;
    f64 meanTest = 0.0;
    f64 aoAbs = 0.0;
    u64 count = 0;
    for (usize i = 0; i < n; ++i) {
        if (!mask.empty() && mask[i] == 0u) {
            continue;
        }
        for (u32 ch = 0; ch < 3u; ++ch) {
            const f64 a = test[i * 4u + ch];
            const f64 b = ref[i * 4u + ch];
            const f64 e = std::fabs(a - b);
            sumAbs += e;
            sumRef += std::fabs(b);
            sumSq += e * e;
            m.maxAbs = std::max(m.maxAbs, e);
        }
        aoAbs += std::fabs(static_cast<f64>(test[i * 4u + 3u]) - ref[i * 4u + 3u]);
        meanRef += (static_cast<f64>(ref[i * 4u]) + ref[i * 4u + 1u] + ref[i * 4u + 2u]) / 3.0;
        meanTest += (static_cast<f64>(test[i * 4u]) + test[i * 4u + 1u] + test[i * 4u + 2u]) / 3.0;
        ++count;
    }
    if (count == 0u) {
        return m;
    }
    m.pixels = static_cast<u32>(count);
    m.meanRef = meanRef / static_cast<f64>(count);
    m.meanTest = meanTest / static_cast<f64>(count);
    m.aoMeanAbs = aoAbs / static_cast<f64>(count);
    m.relL1 = sumRef > 0.0 ? sumAbs / sumRef : sumAbs;
    const f64 meanAbsRef = sumRef / (3.0 * static_cast<f64>(count));
    const f64 rmse = std::sqrt(sumSq / (3.0 * static_cast<f64>(count)));
    m.relRmse = meanAbsRef > 0.0 ? rmse / meanAbsRef : rmse;
    return m;
}

std::vector<u8> geometryMask(const SsrcInputs& in) {
    const usize n = static_cast<usize>(in.width) * in.height;
    std::vector<u8> mask(n, 0u);
    for (usize i = 0; i < n && i * 4u < in.geo.size(); ++i) {
        mask[i] = in.geo[i * 4u] > 0.f ? 1u : 0u;
    }
    return mask;
}

} // namespace fuse::renderer::ssrc
