#pragma once
// WP-4.1 gates: analytic motion reference (f64), camera math and the synthetic TAAU input sequence shared by
// test_rp_temporal_cpu.cpp and test_rp_temporal.cpp.
#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/taa/taau_kernel.hpp>
#include <fuse/renderer/temporal/temporal_types.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>

#include <cmath>
#include <vector>

namespace tm_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::renderer::gpu_scene::GpuTransform;

// --- camera math (column-major, Vulkan clip space with NDC y down, forward depth) --------------------------
struct Mat4 {
    f32 m[16] = {};
};

inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

inline Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

/// World -> view (right-handed, looks down -Z, y up).
inline Mat4 lookAt(const f32 eye[3], const f32 at[3]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& v : s) {
        v /= sl;
    }
    const f32 u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -f[0];
    v.m[6] = -f[1];
    v.m[10] = -f[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    v.m[15] = 1.f;
    return v;
}

inline fuse::math::Mat4 toMath(const Mat4& a) {
    fuse::math::Mat4 r{};
    for (u32 i = 0; i < 16u; ++i) {
        r.data[i] = a.m[i];
    }
    return r;
}

/// Object -> world transform: rotation about y (yaw) then x (pitch), non-uniform scale, translation.
inline GpuTransform place(f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz, f32 yaw, f32 pitch = 0.f) {
    const f32 cy = std::cos(yaw), syw = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // R = Ry(yaw) * Rx(pitch)
    const f32 r[3][3] = {{cy, syw * sp, syw * cp}, {0.f, cp, -sp}, {-syw, cy * sp, cy * cp}};
    GpuTransform t{};
    for (u32 i = 0; i < 3u; ++i) {
        t.rows[i][0] = r[i][0] * sx;
        t.rows[i][1] = r[i][1] * sy;
        t.rows[i][2] = r[i][2] * sz;
    }
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

// --- analytic motion (f64) -----------------------------------------------------------------------------------
struct D4 {
    f64 x = 0, y = 0, z = 0, w = 0;
};

inline void worldOf(const GpuTransform& t, const f64 p[3], f64 out[3]) {
    for (u32 r = 0; r < 3u; ++r) {
        out[r] = static_cast<f64>(t.rows[r][0]) * p[0] + static_cast<f64>(t.rows[r][1]) * p[1] +
                 static_cast<f64>(t.rows[r][2]) * p[2] + static_cast<f64>(t.rows[r][3]);
    }
}

inline D4 clipOf(const f32 m[16], const f64 w[3]) {
    D4 c;
    c.x = m[0] * w[0] + m[4] * w[1] + m[8] * w[2] + static_cast<f64>(m[12]);
    c.y = m[1] * w[0] + m[5] * w[1] + m[9] * w[2] + static_cast<f64>(m[13]);
    c.z = m[2] * w[0] + m[6] * w[1] + m[10] * w[2] + static_cast<f64>(m[14]);
    c.w = m[3] * w[0] + m[7] * w[1] + m[11] * w[2] + static_cast<f64>(m[15]);
    return c;
}

struct Analytic {
    bool ok = false;
    f64 motionPx[2] = {0, 0}; ///< (uv_cur - uv_prev) * size
    f64 curPx[2] = {0, 0};    ///< unjittered current position of the seen point, pixels
    f64 prevDrawPx[2] = {0, 0}; ///< its position under the previous frame's DRAW (jittered) matrix, pixels
    f64 depth = 0;
};

/// The exact point of triangle `v` (object space, decoded f32 vertices) that the pixel centre (x, y) sees under
/// `draw` (the rasterised matrix), and its motion under the unjittered matrices, all in f64.
inline Analytic analyticMotion(const f32 draw[16], const f32 vp[16], const f32 prevVp[16], const f32 prevDraw[16],
                               const GpuTransform& cur, const GpuTransform& prev, const f32 v[3][3], u32 x, u32 y, u32 w,
                               u32 h) {
    Analytic a;
    D4 c[3];
    f64 obj[3][3];
    for (u32 k = 0; k < 3u; ++k) {
        for (u32 i = 0; i < 3u; ++i) {
            obj[k][i] = v[k][i];
        }
        f64 wp[3];
        worldOf(cur, obj[k], wp);
        c[k] = clipOf(draw, wp);
    }
    const f64 nx = (x + 0.5) * 2.0 / w - 1.0;
    const f64 ny = (y + 0.5) * 2.0 / h - 1.0;
    f64 ux[3], uy[3];
    for (u32 k = 0; k < 3u; ++k) {
        ux[k] = c[k].x - nx * c[k].w;
        uy[k] = c[k].y - ny * c[k].w;
    }
    const f64 e0 = ux[1] * uy[2] - uy[1] * ux[2];
    const f64 e1 = ux[2] * uy[0] - uy[2] * ux[0];
    const f64 e2 = ux[0] * uy[1] - uy[0] * ux[1];
    const f64 s = e0 + e1 + e2;
    if (s == 0.0) {
        return a;
    }
    const f64 b[3] = {e0 / s, e1 / s, e2 / s};
    f64 p[3];
    for (u32 i = 0; i < 3u; ++i) {
        p[i] = b[0] * obj[0][i] + b[1] * obj[1][i] + b[2] * obj[2][i];
    }
    f64 wc[3], wpv[3];
    worldOf(cur, p, wc);
    worldOf(prev, p, wpv);
    const D4 cc = clipOf(vp, wc);
    const D4 pc = clipOf(prevVp, wpv);
    const D4 pd = clipOf(prevDraw, wpv);
    if (!(cc.w > 0.0) || !(pc.w > 0.0)) {
        return a;
    }
    a.curPx[0] = (cc.x / cc.w * 0.5 + 0.5) * w;
    a.curPx[1] = (cc.y / cc.w * 0.5 + 0.5) * h;
    const f64 prevPx[2] = {(pc.x / pc.w * 0.5 + 0.5) * w, (pc.y / pc.w * 0.5 + 0.5) * h};
    a.prevDrawPx[0] = (pd.x / pd.w * 0.5 + 0.5) * w;
    a.prevDrawPx[1] = (pd.y / pd.w * 0.5 + 0.5) * h;
    a.motionPx[0] = a.curPx[0] - prevPx[0];
    a.motionPx[1] = a.curPx[1] - prevPx[1];
    a.depth = cc.w;
    a.ok = true;
    return a;
}

/// Sky motion in pixels of the sample (x + 0.5 + jx, y + 0.5 + jy): direction d = A_cur^-1 (ndc, 1) (f64), then
/// A_prev d (A = rows x, y, w and columns 0..2 of the view-projection).
inline bool analyticSky(const f32 vp[16], const f32 prevVp[16], f32 jx, f32 jy, u32 x, u32 y, u32 w, u32 h, f64 out[2]) {
    const u32 rows[3] = {0u, 1u, 3u};
    f64 a[3][3];
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 3u; ++c) {
            a[r][c] = vp[c * 4u + rows[r]];
        }
    }
    const f64 sx = x + 0.5 + static_cast<f64>(jx);
    const f64 sy = y + 0.5 + static_cast<f64>(jy);
    const f64 rhs[3] = {sx * 2.0 / w - 1.0, sy * 2.0 / h - 1.0, 1.0};
    // Cramer's rule.
    auto det3 = [](const f64 m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const f64 det = det3(a);
    if (det == 0.0) {
        return false;
    }
    f64 d[3];
    for (u32 k = 0; k < 3u; ++k) {
        f64 m[3][3];
        for (u32 r = 0; r < 3u; ++r) {
            for (u32 c = 0; c < 3u; ++c) {
                m[r][c] = c == k ? rhs[r] : a[r][c];
            }
        }
        d[k] = det3(m) / det;
    }
    f64 q[3] = {0, 0, 0};
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 3u; ++c) {
            q[r] += static_cast<f64>(prevVp[c * 4u + rows[r]]) * d[c];
        }
    }
    if (!(q[2] > 0.0)) {
        return false;
    }
    out[0] = sx - (q[0] / q[2] * 0.5 + 0.5) * w;
    out[1] = sy - (q[1] / q[2] * 0.5 + 0.5) * h;
    return true;
}

// --- synthetic TAAU sequence ------------------------------------------------------------------------------------
/// Owning render-resolution inputs of one frame (CPU UpscaleInputs views them).
struct TaauFrame {
    fuse::renderer::UpscaleResolution resolution{};
    std::vector<fuse::math::Vec3> color;
    std::vector<f32> depth;
    std::vector<fuse::math::Vec2> motion;
    std::vector<f32> reactive;
    std::vector<f32> transparency;
    fuse::math::Vec2 jitter{};
    fuse::renderer::UpscaleCamera camera{};
    fuse::renderer::UpscaleCamera previousCamera{};
    bool reset = false;
    bool masks = true;

    fuse::renderer::UpscaleInputs inputs() const {
        fuse::renderer::UpscaleInputs in{};
        in.resolution = resolution;
        in.color = color.data();
        in.depth = depth.data();
        in.motion = motion.data();
        in.reactive = masks ? reactive.data() : nullptr;
        in.transparency_composition = masks ? transparency.data() : nullptr;
        in.jitter_px = jitter;
        in.exposure = 0.7f;
        in.reset_history = reset;
        in.camera = camera;
        in.previous_camera = previousCamera;
        return in;
    }
};

inline fuse::renderer::UpscaleCamera sequenceCamera(u32 frame, bool withCamera, f32 aspect) {
    fuse::renderer::UpscaleCamera c{};
    const f32 t = static_cast<f32>(frame);
    const f32 eye[3] = {0.12f * t, 0.3f, 4.f};
    const f32 at[3] = {0.12f * t + 0.05f * std::sin(0.4f * t), 0.2f, -6.f};
    c.view = toMath(lookAt(eye, at));
    c.vertical_fov_rad = withCamera ? 0.9f : 0.f;
    c.aspect = aspect;
    return c;
}

/// Frame `frame` of a procedural sequence at `res`: a tilted textured background panning at `pan` render px /
/// frame (camera motion), a nearer bright striped rectangle moving the other way (dilation, disocclusion,
/// velocity rejection), sky rows at the top (depth 0), HDR sparkles, a reactive disc and a transparency band.
/// The colour is evaluated at the jittered sample position so the jitter matters.
inline TaauFrame makeTaauFrame(const fuse::renderer::UpscaleResolution& res, u32 frame, bool withCamera, bool masks) {
    TaauFrame f{};
    f.resolution = res;
    f.masks = masks;
    const u32 rw = res.render_width;
    const u32 rh = res.render_height;
    const u32 n = rw * rh;
    f.color.resize(n);
    f.depth.resize(n);
    f.motion.resize(n);
    f.reactive.resize(n);
    f.transparency.resize(n);
    f.jitter = fuse::renderer::upscaleJitterOffset(frame, fuse::renderer::upscaleJitterPhaseCount(res));
    const f32 aspect = static_cast<f32>(res.display_width) / static_cast<f32>(res.display_height);
    f.camera = sequenceCamera(frame, withCamera, aspect);
    f.previousCamera = sequenceCamera(frame > 0u ? frame - 1u : 0u, withCamera, aspect);
    const f32 t = static_cast<f32>(frame);
    const f32 pan = 0.37f;   // render px / frame (background)
    const f32 objVel = -0.9f; // render px / frame (object, x)
    const f32 objX0 = 0.55f * rw - objVel * t;
    const f32 objX1 = objX0 + 0.22f * rw;
    const f32 objY0 = 0.35f * rh;
    const f32 objY1 = 0.62f * rh;
    const u32 skyRows = rh / 8u;
    for (u32 j = 0; j < rh; ++j) {
        for (u32 i = 0; i < rw; ++i) {
            const u32 k = j * rw + i;
            const f32 sx = static_cast<f32>(i) + 0.5f + f.jitter.x;
            const f32 sy = static_cast<f32>(j) + 0.5f + f.jitter.y;
            fuse::math::Vec3 c{};
            f32 d = 0.f;
            fuse::math::Vec2 m{};
            if (j < skyRows) {
                c = {0.3f, 0.5f, 0.9f + 0.1f * std::sin(0.05f * (sx + 0.5f * pan * t))};
                d = 0.f;
                m = {0.5f * pan / static_cast<f32>(rw), 0.f};
            } else if (sx >= objX0 && sx < objX1 && sy >= objY0 && sy < objY1) {
                const f32 u = sx - objX0;
                const f32 stripe = std::fmod(u * 0.7f, 2.f) < 1.f ? 1.f : 0.f;
                c = {2.5f * stripe + 0.1f, 0.4f, 1.5f * (1.f - stripe) + 0.05f};
                d = 2.f + 0.01f * sy;
                m = {objVel / static_cast<f32>(rw), 0.f};
            } else {
                const f32 wx = sx + pan * t;
                const f32 checker = (static_cast<int>(std::floor(wx / 3.f)) + static_cast<int>(std::floor(sy / 3.f))) % 2 == 0 ? 1.f : 0.f;
                c = {0.2f + 0.6f * checker, 0.25f + 0.2f * std::sin(0.3f * wx), 0.15f + 0.1f * std::cos(0.2f * sy)};
                if ((i * 7u + j * 13u + frame * 3u) % 97u == 0u) {
                    c = {12.f, 11.f, 9.f}; // HDR sparkle
                }
                d = 5.f + 3.f * sy / static_cast<f32>(rh);
                m = {pan / static_cast<f32>(rw), 0.f};
            }
            f.color[k] = c;
            f.depth[k] = d;
            f.motion[k] = m;
            const f32 dx = sx - 0.25f * rw;
            const f32 dy = sy - 0.75f * rh;
            f.reactive[k] = dx * dx + dy * dy < 0.01f * rw * rw ? 0.7f : 0.f;
            f.transparency[k] = (sy > 0.8f * rh && sy < 0.88f * rh) ? 0.5f : 0.f;
        }
    }
    return f;
}

} // namespace tm_test
