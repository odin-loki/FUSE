#pragma once

// Single-source renderer of the upscaler reference scenes — kernel "upscale_ref_scene" (docs/compute-kernels.md).
//
// One item per output pixel. The static set is an SDF scene traced with the reference SDF ray-march kernel's
// `ray_march_kernel::scene_eval` / `scene_normal` (fuse_compute — the same code as "sdf_ray_march"), combined with
// an analytic ground plane and two dynamic SDF capsules (a fast thin rod and a rotating articulated arm whose
// motion varies per surface point, like a skinned limb). Alpha-blended soft particles are composited on top and
// produce the reactive / transparency masks. The kernel writes, per pixel: colour (box-filtered over
// `samples_per_axis`^2 stratified sub-samples, or one sample at the jittered position), linear view depth, UV motion
// (current - previous, unjittered; sky = camera rotation only), reactive, transparency, object id and — for
// ground truth — an exact disocclusion flag (the surface point was not visible from the previous camera).
//
// Textures (ground checker + fine stripes for moire, triplanar checker on "tile" objects) are analytically
// box-filtered over a footprint of `footprint_scale` output pixels, so the render-resolution frame applies the
// upscaler mip bias (footprint 2^mip_bias render pixels) and the ground truth integrates its sub-sample footprint.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::refscene_kernel {

inline constexpr const char* kName = "upscale_ref_scene";
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};
inline constexpr u32 kMaxParticles = 8u;
inline constexpr u32 kMaxSamplesPerAxis = 8u;

/// Object ids written to the id buffer.
inline constexpr u32 kIdSky = 0u;
inline constexpr u32 kIdGround = 1u;
inline constexpr u32 kIdStaticBase = 16u; ///< + SdfObject::material_id
inline constexpr u32 kIdThin = 100u;
inline constexpr u32 kIdArm = 101u;

/// Pinhole camera (basis matches math::lookAt: right = forward x up, up = right x forward).
struct Camera {
    math::Vec3 position{};
    math::Vec3 right{1.f, 0.f, 0.f};
    math::Vec3 up{0.f, 1.f, 0.f};
    math::Vec3 forward{0.f, 0.f, -1.f};
    f32 tan_half_y = 0.5f;
    f32 aspect = 1.f;
};

struct Particle {
    math::Vec3 position{};
    math::Vec3 color{};
    f32 radius = 0.2f;
    f32 alpha = 0.5f;
};

/// Everything the scene needs for one frame (POD; resolved on the host).
struct SceneState {
    Camera camera{};
    Camera prev_camera{};
    compute::RayMarchParams sdf{}; ///< Static SDF objects (+ max_steps / min_dist / max_dist) for scene_eval.
    math::Vec3 bounds_min{};       ///< AABB of static + dynamic objects (ray clip).
    math::Vec3 bounds_max{};
    u32 has_thin = 0;
    math::Vec3 thin_pos{};
    math::Vec3 thin_prev_pos{};
    f32 thin_radius = 0.03f;
    f32 thin_half_length = 0.8f;
    u32 has_arm = 0;
    math::Vec3 arm_pivot{};
    f32 arm_angle = 0.f;
    f32 arm_prev_angle = 0.f;
    f32 arm_length = 1.2f;
    f32 arm_radius = 0.09f;
    u32 particle_count = 0;
    Particle particles[kMaxParticles] = {};
    math::Vec3 particle_velocity{}; ///< World units per frame (shared by all particles).
    math::Vec3 sun_dir{0.4f, 0.8f, 0.3f};
    f32 checker_frequency = 2.f; ///< Ground checker cells per metre.
    f32 stripe_frequency = 7.f;  ///< Ground stripe cycles per metre (moire source).
    f32 stripe_amount = 0.35f;
};

struct Params {
    SceneState scene{};
    u32 width = 0;
    u32 height = 0;
    math::Vec2 jitter_px{};       ///< Used when samples_per_axis == 1.
    u32 samples_per_axis = 1;     ///< > 1: stratified box supersampling of the pixel (jitter ignored).
    f32 footprint_scale = 1.f;    ///< Texture filter width in output pixels.
    kernel::Span<math::Vec3> color{};
    kernel::Span<f32> depth{};
    kernel::Span<math::Vec2> motion{};
    kernel::Span<f32> reactive{};
    kernel::Span<f32> transparency{};
    kernel::Span<u32> object_id{};
    kernel::Span<u8> disoccluded{}; ///< Optional: 1 when the centre-sample surface was hidden last frame.
};

// ---------------------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline f32 sat(f32 v) { return std::min(std::max(v, 0.f), 1.f); }

FUSE_HOST_DEVICE inline math::Vec3 camera_ray(const Camera& c, f32 ndcX, f32 ndcY) {
    return (c.forward + c.right * (ndcX * c.tan_half_y * c.aspect) + c.up * (ndcY * c.tan_half_y)).normalized();
}

/// UV of a point (w = 1) or a direction (w = 0) under a camera; false when behind it.
FUSE_HOST_DEVICE inline bool project_uv(const Camera& c, const math::Vec3& p, bool isDirection, math::Vec2& uv) {
    const math::Vec3 v = isDirection ? p : p - c.position;
    const f32 z = v.dot(c.forward);
    if (!(z > 1e-6f)) {
        return false;
    }
    uv = {0.5f + 0.5f * v.dot(c.right) / (z * c.tan_half_y * c.aspect), 0.5f - 0.5f * v.dot(c.up) / (z * c.tan_half_y)};
    return true;
}

FUSE_HOST_DEVICE inline math::Vec3 rotate_z(const math::Vec3& p, f32 angle) {
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    return {c * p.x - s * p.y, s * p.x + c * p.y, p.z};
}

FUSE_HOST_DEVICE inline f32 thin_sdf(const SceneState& s, const math::Vec3& p, math::Vec3* n) {
    const math::Vec3 l = p - s.thin_pos;
    const math::Vec3 v{l.x, l.y - std::min(std::max(l.y, -s.thin_half_length), s.thin_half_length), l.z};
    if (n != nullptr) {
        *n = v.normalized();
    }
    return v.length() - s.thin_radius;
}

FUSE_HOST_DEVICE inline f32 arm_sdf(const SceneState& s, const math::Vec3& p, math::Vec3* n) {
    const math::Vec3 local = rotate_z(p - s.arm_pivot, -s.arm_angle); // arm along local +X
    const math::Vec3 v{local.x - std::min(std::max(local.x, 0.f), s.arm_length), local.y, local.z};
    if (n != nullptr) {
        *n = rotate_z(v.normalized(), s.arm_angle);
    }
    return v.length() - s.arm_radius;
}

/// Opaque scene distance (static SDF via the ray-march kernel + dynamic capsules), no ground plane.
FUSE_HOST_DEVICE inline f32 opaque_sdf(const SceneState& s, const math::Vec3& p) {
    f32 d = s.sdf.object_count > 0u ? compute::ray_march_kernel::scene_eval(s.sdf, p, nullptr) : s.sdf.max_dist;
    if (s.has_thin != 0u) {
        d = std::min(d, thin_sdf(s, p, nullptr));
    }
    if (s.has_arm != 0u) {
        d = std::min(d, arm_sdf(s, p, nullptr));
    }
    return d;
}

struct Hit {
    f32 t = -1.f;
    math::Vec3 position{};
    math::Vec3 normal{0.f, 1.f, 0.f};
    u32 id = kIdSky;
    u32 material = 0;
};

FUSE_HOST_DEVICE inline bool ray_box(const math::Vec3& o, const math::Vec3& d, const math::Vec3& lo, const math::Vec3& hi,
                                     f32& t0, f32& t1) {
    t0 = 0.f;
    t1 = 3.0e38f;
    const f32 ov[3] = {o.x, o.y, o.z};
    const f32 dv[3] = {d.x, d.y, d.z};
    const f32 lv[3] = {lo.x, lo.y, lo.z};
    const f32 hv[3] = {hi.x, hi.y, hi.z};
    for (u32 a = 0; a < 3u; ++a) {
        if (std::fabs(dv[a]) < 1e-9f) {
            if (ov[a] < lv[a] || ov[a] > hv[a]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.f / dv[a];
        f32 ta = (lv[a] - ov[a]) * inv;
        f32 tb = (hv[a] - ov[a]) * inv;
        if (ta > tb) {
            const f32 tmp = ta;
            ta = tb;
            tb = tmp;
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    return t0 <= t1;
}

/// Closest opaque hit along a unit ray (ground plane y = 0, static SDF, dynamic capsules).
FUSE_HOST_DEVICE inline Hit trace(const SceneState& s, const math::Vec3& o, const math::Vec3& d) {
    Hit hit{};
    f32 tMax = s.sdf.max_dist;
    if (d.y < -1e-6f && o.y > 0.f) {
        const f32 tp = -o.y / d.y;
        if (tp < tMax) {
            tMax = tp;
            hit.t = tp;
            hit.id = kIdGround;
            hit.normal = {0.f, 1.f, 0.f};
        }
    }
    f32 t0 = 0.f;
    f32 t1 = 0.f;
    if (ray_box(o, d, s.bounds_min, s.bounds_max, t0, t1)) {
        f32 t = t0;
        const f32 tEnd = std::min(t1, tMax);
        for (u32 step = 0; step < s.sdf.max_steps && t <= tEnd; ++step) {
            const math::Vec3 p = o + d * t;
            const f32 dist = opaque_sdf(s, p);
            if (dist < s.sdf.min_dist * (1.f + t)) {
                hit.t = t;
                hit.position = p;
                // Which surface: the smallest of the three distance terms.
                f32 best = s.sdf.object_count > 0u ? compute::ray_march_kernel::scene_eval(s.sdf, p, nullptr) : 3.0e38f;
                hit.id = kIdStaticBase;
                hit.normal = compute::ray_march_kernel::scene_normal(s.sdf, p);
                // Material of the closest static primitive.
                f32 closestObj = 3.0e38f;
                for (u32 i = 0; i < s.sdf.object_count; ++i) {
                    const compute::SdfObject& obj = s.sdf.objects[i];
                    const f32 od = compute::ray_march_kernel::object_sdf(obj, p - obj.position, s.sdf.max_dist, nullptr);
                    if (od < closestObj) {
                        closestObj = od;
                        hit.material = obj.material_id;
                    }
                }
                hit.id = kIdStaticBase + hit.material;
                if (s.has_thin != 0u) {
                    math::Vec3 n{};
                    const f32 td = thin_sdf(s, p, &n);
                    if (td < best) {
                        best = td;
                        hit.id = kIdThin;
                        hit.normal = n;
                        hit.material = 0u;
                    }
                }
                if (s.has_arm != 0u) {
                    math::Vec3 n{};
                    const f32 ad = arm_sdf(s, p, &n);
                    if (ad < best) {
                        hit.id = kIdArm;
                        hit.normal = n;
                        hit.material = 0u;
                    }
                }
                return hit;
            }
            t += dist;
        }
    }
    if (hit.id == kIdGround) {
        hit.position = o + d * hit.t;
    }
    return hit;
}

/// Box-filtered checker over [x - w/2, x + w/2] (1D integral of the +/-1 square wave), product of two axes.
FUSE_HOST_DEVICE inline f32 filtered_square(f32 x, f32 w) {
    // Integral of square wave s(x) = +1 on [0, 1), -1 on [1, 2) (period 2): I(x) = |frac(x/2) * 2 - 1| based.
    const auto tri = [](f32 v) {
        const f32 f = v * 0.5f - std::floor(v * 0.5f); // [0, 1)
        return f < 0.5f ? 2.f * f : 2.f - 2.f * f;      // triangle 0..1..0 over a period of 2 (integral / 1)
    };
    if (w < 1e-4f) {
        const f32 f = x * 0.5f - std::floor(x * 0.5f);
        return f < 0.5f ? 1.f : -1.f;
    }
    // d/dx tri(x) = +1 on [0, 1), -1 on [1, 2): average of the square wave = (tri(x + w/2) - tri(x - w/2)) / w.
    return (tri(x + 0.5f * w) - tri(x - 0.5f * w)) / w;
}

/// Checker in {0, 1} with cells of size 1, box-filtered with width `w` (cells).
FUSE_HOST_DEVICE inline f32 filtered_checker(f32 x, f32 y, f32 w) {
    return 0.5f - 0.5f * filtered_square(x, w) * filtered_square(y, w);
}

/// 0.5 + 0.5 cos(2 pi x) box-filtered with width `w` (periods): the cosine term scales by sinc(pi w).
FUSE_HOST_DEVICE inline f32 filtered_cos(f32 x, f32 w) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 a = kPi * w;
    const f32 sinc = a < 1e-4f ? 1.f : std::sin(a) / a;
    return 0.5f + 0.5f * std::cos(2.f * kPi * x) * sinc;
}

FUSE_HOST_DEVICE inline math::Vec3 albedo(const SceneState& s, const Hit& h, f32 footprintWorld) {
    if (h.id == kIdGround) {
        const f32 fc = s.checker_frequency;
        const f32 c = filtered_checker(h.position.x * fc, h.position.z * fc, footprintWorld * fc);
        const f32 fs = s.stripe_frequency;
        const f32 stripe = filtered_cos((h.position.x + 0.35f * h.position.z) * fs, footprintWorld * fs * 1.06f);
        const f32 k = (1.f - s.stripe_amount) + s.stripe_amount * stripe;
        return math::Vec3{0.12f + 0.62f * c, 0.13f + 0.6f * c, 0.16f + 0.55f * c} * k;
    }
    if (h.id == kIdThin) {
        return {0.95f, 0.9f, 0.35f};
    }
    if (h.id == kIdArm) {
        return {0.8f, 0.15f, 0.12f};
    }
    switch (h.material) {
    case 1u: { // tiles: triplanar box-filtered checker (4 cells / metre)
        const f32 f = 4.f;
        const f32 w = footprintWorld * f;
        const math::Vec3 n{std::fabs(h.normal.x), std::fabs(h.normal.y), std::fabs(h.normal.z)};
        const math::Vec3 p = h.position * f;
        f32 c = 0.f;
        if (n.x >= n.y && n.x >= n.z) {
            c = filtered_checker(p.y, p.z, w);
        } else if (n.y >= n.z) {
            c = filtered_checker(p.x, p.z, w);
        } else {
            c = filtered_checker(p.x, p.y, w);
        }
        return {0.2f + 0.6f * c, 0.25f + 0.45f * c, 0.3f + 0.25f * c};
    }
    case 2u:
        return {0.2f, 0.55f, 0.25f};
    case 3u:
        return {0.75f, 0.75f, 0.78f};
    default:
        return {0.55f, 0.45f, 0.35f};
    }
}

FUSE_HOST_DEVICE inline math::Vec3 sky(const SceneState& s, const math::Vec3& d) {
    const f32 t = sat(d.y * 1.5f);
    const math::Vec3 horizon{0.78f, 0.82f, 0.88f};
    const math::Vec3 zenith{0.28f, 0.45f, 0.8f};
    const f32 sun = std::pow(std::max(0.f, d.dot(s.sun_dir.normalized())), 48.f);
    return horizon + (zenith - horizon) * t + math::Vec3{1.f, 0.85f, 0.6f} * (0.6f * sun);
}

/// Previous-frame world position of a surface point (object motion); static geometry does not move.
FUSE_HOST_DEVICE inline math::Vec3 previous_position(const SceneState& s, const Hit& h) {
    if (h.id == kIdThin) {
        return h.position - (s.thin_pos - s.thin_prev_pos);
    }
    if (h.id == kIdArm) {
        const math::Vec3 local = rotate_z(h.position - s.arm_pivot, -s.arm_angle);
        return s.arm_pivot + rotate_z(local, s.arm_prev_angle);
    }
    return h.position;
}

struct Shaded {
    math::Vec3 color{};
    f32 coverage = 0.f; ///< Particle coverage (1 - prod(1 - alpha_i)).
};

/// Shades one camera ray: opaque surface + back-to-front particle composite.
FUSE_HOST_DEVICE inline Shaded shade(const SceneState& s, const math::Vec3& o, const math::Vec3& d, const Hit& h,
                                     f32 pixelAngle) {
    Shaded out{};
    math::Vec3 c{};
    if (h.id == kIdSky) {
        c = sky(s, d);
    } else {
        const f32 cosIncidence = std::max(std::fabs(h.normal.dot(d)), 0.15f);
        const f32 footprint = h.t * pixelAngle / cosIncidence;
        const math::Vec3 a = albedo(s, h, footprint);
        const math::Vec3 l = s.sun_dir.normalized();
        const f32 diffuse = std::max(0.f, h.normal.dot(l));
        const f32 ambient = 0.28f + 0.12f * h.normal.y;
        c = a * (ambient + 0.85f * diffuse);
    }
    const f32 tOpaque = h.id == kIdSky ? 3.0e38f : h.t;
    // Particles: soft camera-facing discs, composited far to near (selection by decreasing distance).
    u32 done = 0u;
    for (u32 n = 0; n < s.particle_count; ++n) {
        f32 farthest = -1.f;
        u32 pick = kMaxParticles;
        for (u32 i = 0; i < s.particle_count; ++i) {
            if ((done >> i) & 1u) {
                continue;
            }
            const f32 tc = (s.particles[i].position - o).dot(d);
            if (tc > farthest) {
                farthest = tc;
                pick = i;
            }
        }
        if (pick == kMaxParticles) {
            break;
        }
        done |= 1u << pick;
        const Particle& pt = s.particles[pick];
        const f32 tc = farthest;
        if (tc <= 0.f || tc >= tOpaque) {
            continue;
        }
        const math::Vec3 closest = o + d * tc;
        const f32 r2 = (closest - pt.position).dot(closest - pt.position) / (pt.radius * pt.radius);
        const f32 a = pt.alpha * sat(1.f - r2) * sat(1.f - r2);
        c = c + (pt.color - c) * a;
        out.coverage = 1.f - (1.f - out.coverage) * (1.f - a);
    }
    out.color = c;
    return out;
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const SceneState& s = p.scene;
        const Camera& cam = s.camera;
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 pixel = y * p.width + x;
        const f32 w = static_cast<f32>(p.width);
        const f32 hgt = static_cast<f32>(p.height);
        const u32 spa = std::max(1u, std::min(p.samples_per_axis, kMaxSamplesPerAxis));
        // Angular size of one texture-filter footprint (small-angle, per output pixel * footprint_scale).
        const f32 pixelAngle = 2.f * cam.tan_half_y / hgt * p.footprint_scale;

        math::Vec3 sum{};
        f32 coverage = 0.f;
        Hit h{};
        math::Vec3 d{};
        for (u32 j = 0; j < spa; ++j) {
            for (u32 i = 0; i < spa; ++i) {
                const f32 ox = spa == 1u ? 0.5f + p.jitter_px.x : (static_cast<f32>(i) + 0.5f) / static_cast<f32>(spa);
                const f32 oy = spa == 1u ? 0.5f + p.jitter_px.y : (static_cast<f32>(j) + 0.5f) / static_cast<f32>(spa);
                const f32 ndcX = 2.f * (static_cast<f32>(x) + ox) / w - 1.f;
                const f32 ndcY = 1.f - 2.f * (static_cast<f32>(y) + oy) / hgt;
                const math::Vec3 sd = camera_ray(cam, ndcX, ndcY);
                const Hit sh0 = trace(s, cam.position, sd);
                const Shaded sh = shade(s, cam.position, sd, sh0, pixelAngle);
                if (spa == 1u) {
                    h = sh0;
                    d = sd;
                }
                sum = sum + sh.color;
                coverage += sh.coverage;
            }
        }
        const f32 inv = 1.f / static_cast<f32>(spa * spa);
        if (!p.color.empty()) {
            p.color[pixel] = sum * inv;
        }

        // Auxiliary buffers from the pixel's representative sample (the jittered sample, or the centre for SSAA).
        if (spa != 1u) {
            d = camera_ray(cam, 2.f * (static_cast<f32>(x) + 0.5f) / w - 1.f, 1.f - 2.f * (static_cast<f32>(y) + 0.5f) / hgt);
            h = trace(s, cam.position, d);
        }
        // Previous-frame poses of the dynamic objects (disocclusion visibility test).
        SceneState prevScene = s;
        prevScene.thin_pos = s.thin_prev_pos;
        prevScene.arm_angle = s.arm_prev_angle;
        const f32 cov = coverage * inv;
        if (!p.reactive.empty()) {
            p.reactive[pixel] = cov;
        }
        if (!p.transparency.empty()) {
            p.transparency[pixel] = cov;
        }
        if (!p.object_id.empty()) {
            p.object_id[pixel] = h.id;
        }
        math::Vec2 motion{};
        bool visibleBefore = true;
        if (h.id == kIdSky) {
            math::Vec2 cur{};
            math::Vec2 prev{};
            if (project_uv(cam, d, true, cur) && project_uv(s.prev_camera, d, true, prev)) {
                motion = cur - prev;
            }
            if (!p.disoccluded.empty()) {
                visibleBefore = trace(prevScene, s.prev_camera.position, d).id == kIdSky;
            }
        } else {
            const math::Vec3 prevPos = previous_position(s, h);
            math::Vec2 cur{};
            math::Vec2 prev{};
            if (project_uv(cam, h.position, false, cur) && project_uv(s.prev_camera, prevPos, false, prev)) {
                motion = cur - prev;
                if (!p.disoccluded.empty()) {
                    const math::Vec3 toP = prevPos - s.prev_camera.position;
                    const f32 dist = toP.length();
                    const Hit ph = trace(prevScene, s.prev_camera.position, toP * (1.f / dist));
                    const bool inView = prev.x >= 0.f && prev.y >= 0.f && prev.x < 1.f && prev.y < 1.f;
                    visibleBefore = inView && ph.id == h.id && std::fabs(ph.t - dist) <= 0.01f * dist + 0.02f;
                }
            } else {
                visibleBefore = false;
            }
        }
        if (!p.motion.empty()) {
            p.motion[pixel] = motion;
        }
        if (!p.depth.empty()) {
            p.depth[pixel] = h.id == kIdSky ? 0.f : (h.position - cam.position).dot(cam.forward);
        }
        if (!p.disoccluded.empty()) {
            p.disoccluded[pixel] = visibleBefore ? 0u : 1u;
        }
    }
};

} // namespace fuse::renderer::refscene_kernel
