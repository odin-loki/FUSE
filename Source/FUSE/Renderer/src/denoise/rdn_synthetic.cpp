// RL-5.5 radiance denoiser: the analytic test scene (include/fuse/renderer/denoise/rdn_synthetic.hpp).
#include <fuse/renderer/denoise/rdn_synthetic.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::denoise {

namespace {

struct V3 {
    f64 x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
f64 dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }

u32 hash(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}
f64 rnd(u32 seed, u32 t, u32 pixel, u32 dim) {
    const u32 h = hash(seed ^ hash(t * 0x9E3779B9u ^ hash(pixel ^ hash(dim + 0x632BE5ABu))));
    return (static_cast<f64>(h >> 8) + 0.5) * (1.0 / 16777216.0);
}

constexpr f64 kWallZ = -10.0;
constexpr f64 kOccZ = 1.0;
constexpr f64 kOccW = 1.2;
constexpr f64 kOccY0 = 0.3;
constexpr f64 kOccY1 = 1.5;
constexpr f64 kTanHalf = 0.6;

struct Cam {
    V3 o, r, u, f; ///< r / u scaled by tan(fov / 2) (x aspect)
};

Cam cameraAt(const RdnSyntheticDesc& d, s32 t) {
    Cam c{};
    c.o = V3{d.camSpeed * static_cast<f64>(t), 1.2, 6.0};
    c.f = norm(V3{0.0, -0.15, -1.0});
    const V3 right = norm(cross(c.f, V3{0.0, 1.0, 0.0}));
    const V3 up = cross(right, c.f);
    const f64 aspect = static_cast<f64>(d.width) / static_cast<f64>(d.height);
    c.r = right * (kTanHalf * aspect);
    c.u = up * kTanHalf;
    return c;
}

RdnCamera toRdn(const Cam& c) {
    RdnCamera r{};
    const V3* v[4] = {&c.o, &c.r, &c.u, &c.f};
    f32* dst[4] = {r.origin, r.right, r.up, r.forward};
    for (u32 k = 0; k < 4u; ++k) {
        dst[k][0] = static_cast<f32>(v[k]->x);
        dst[k][1] = static_cast<f32>(v[k]->y);
        dst[k][2] = static_cast<f32>(v[k]->z);
    }
    return r;
}

struct State {
    f64 occX = 0.0; ///< occluder left edge
    f64 light = 1.0;
    bool occluder = true;
};

State stateAt(const RdnSyntheticDesc& d, s32 t) {
    State s{};
    s.occX = d.occluderStart + d.occluderSpeed * static_cast<f64>(t);
    s.light = t >= 0 && static_cast<u32>(t) >= d.lightStepFrame ? d.lightStepScale : 1.0;
    s.occluder = d.occluder;
    return s;
}

struct Hit {
    u8 surface = kRdnSky;
    f64 t = 0.0;
    V3 p{};
};

Hit trace(const State& s, V3 o, V3 dir, bool floor) {
    Hit h{};
    h.t = 1e30;
    if (s.occluder && dir.z != 0.0) {
        const f64 t = (kOccZ - o.z) / dir.z;
        const V3 p = o + dir * t;
        if (t > 1e-4 && t < h.t && p.x >= s.occX && p.x <= s.occX + kOccW && p.y >= kOccY0 && p.y <= kOccY1) {
            h = Hit{kRdnOccluder, t, p};
        }
    }
    if (dir.z < 0.0) {
        const f64 t = (kWallZ - o.z) / dir.z;
        const V3 p = o + dir * t;
        if (t > 1e-4 && t < h.t && p.y >= 0.0) {
            h = Hit{kRdnWall, t, p};
        }
    }
    if (floor && dir.y < 0.0) {
        const f64 t = -o.y / dir.y;
        const V3 p = o + dir * t;
        if (t > 1e-4 && t < h.t && p.z >= kWallZ && p.z <= 8.0) {
            h = Hit{kRdnFloor, t, p};
        }
    }
    if (h.t >= 1e30) {
        h = Hit{};
    }
    return h;
}

f64 smooth(f64 a, f64 b, f64 v) {
    const f64 t = std::fmin(std::fmax((v - a) / (b - a), 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Soft shadow of the occluder on the floor (light from above, slightly behind: the shadow lies at z in [-0.5, 1]).
f64 floorShadow(const State& s, V3 p) {
    if (!s.occluder) {
        return 1.0;
    }
    const f64 e = 0.15;
    const f64 inX = smooth(s.occX - e, s.occX + e, p.x) * (1.0 - smooth(s.occX + kOccW - e, s.occX + kOccW + e, p.x));
    const f64 inZ = smooth(-0.5 - e, -0.5 + e, p.z) * (1.0 - smooth(1.0 - e, 1.0 + e, p.z));
    return 1.0 - 0.65 * inX * inZ;
}

V3 diffuseTruth(const State& s, const Hit& h) {
    switch (h.surface) {
    case kRdnWall:
        return V3{0.9, 0.85, 0.8} * (s.light * (1.0 + 0.5 * std::sin(0.7 * h.p.x)));
    case kRdnFloor:
        return V3{0.8, 0.8, 0.85} * (s.light * (0.6 + 0.3 * std::cos(0.5 * h.p.x + 0.3 * h.p.z)) * floorShadow(s, h.p));
    case kRdnOccluder:
        return V3{0.7, 0.7, 0.7} * s.light;
    default:
        return V3{};
    }
}

/// Outgoing radiance of a hit (what a reflection sees): albedo x irradiance.
V3 outgoing(const State& s, const Hit& h) {
    if (h.surface == kRdnSky) {
        return V3{0.3, 0.4, 0.6} * s.light;
    }
    const V3 e = diffuseTruth(s, h);
    if (h.surface == kRdnWall) {
        const f64 fr = h.p.x - std::floor(h.p.x);
        const V3 a = fr < 0.5 ? V3{0.9, 0.3, 0.2} : V3{0.2, 0.5, 0.9};
        return V3{e.x * a.x, e.y * a.y, e.z * a.z};
    }
    if (h.surface == kRdnOccluder) {
        return V3{e.x * 0.8, e.y * 0.8, e.z * 0.2};
    }
    return e * 0.5;
}

struct Shade {
    Hit hit{};
    V3 diffuse{};
    V3 specular{};
    f64 hitDist = 0.0;
    V3 n{};
    f64 rough = 1.0;
};

Shade shade(const RdnSyntheticDesc& d, const State& s, const Cam& c, f64 fx, f64 fy) {
    const f64 x = (fx / d.width) * 2.0 - 1.0;
    const f64 y = 1.0 - (fy / d.height) * 2.0;
    const V3 dir = norm(c.f + c.r * x + c.u * y);
    Shade sh{};
    sh.hit = trace(s, c.o, dir, true);
    const Hit& h = sh.hit;
    if (h.surface == kRdnSky) {
        return sh;
    }
    sh.diffuse = diffuseTruth(s, h);
    if (h.surface == kRdnFloor) {
        sh.n = V3{0.0, 1.0, 0.0};
        sh.rough = d.floorRoughness;
        const V3 r{dir.x, -dir.y, dir.z};
        const Hit rh = trace(s, h.p, r, false);
        sh.specular = outgoing(s, rh);
        sh.hitDist = rh.surface == kRdnSky ? 0.0 : rh.t;
    } else {
        sh.n = V3{0.0, 0.0, 1.0};
        sh.rough = 1.0;
        sh.specular = V3{0.1, 0.1, 0.1} * s.light;
        sh.hitDist = 5.0;
    }
    return sh;
}

bool project(const Cam& c, V3 p, f64& u, f64& v) {
    const V3 q = p - c.o;
    const f64 z = dot(q, c.f);
    if (!(z > 1e-6)) {
        return false;
    }
    u = dot(q, c.r) / (z * dot(c.r, c.r)) * 0.5 + 0.5;
    v = 0.5 - dot(q, c.u) / (z * dot(c.u, c.u)) * 0.5;
    return true;
}

f64 lum(V3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

f64 noiseD(u32 seed, u32 t, u32 i) { return -std::log(1.0 - rnd(seed, t, i, 0u)); }
f64 noiseS(u32 seed, u32 t, u32 i, f64 rough) {
    const f64 r = rnd(seed, t, i, 1u);
    return rough <= 0.1 ? 0.5 + r : 2.0 * r;
}

rdnk::float4 f4(V3 v, f64 w) {
    return rdnk::float4(static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z), static_cast<f32>(w));
}

} // namespace

RdnReferenceInputs RdnSyntheticFrame::inputs(bool gradients, bool instances) const {
    RdnReferenceInputs in{};
    in.diffuse = diffuse.data();
    in.specular = specular.data();
    in.normal = normal.data();
    in.depth = depth.data();
    in.motion = motion.data();
    in.instance = instances ? instance.data() : nullptr;
    in.gradient = gradients ? gradient.data() : nullptr;
    return in;
}

void rdn_synthetic_frame(const RdnSyntheticDesc& d, u32 t, u32 seed, RdnSyntheticFrame& out) {
    const u32 w = d.width;
    const u32 h = d.height;
    const usize n = static_cast<usize>(w) * h;
    const u32 sw = (w + kRdnStratum - 1u) / kRdnStratum;
    const u32 shh = (h + kRdnStratum - 1u) / kRdnStratum;
    out.width = w;
    out.height = h;
    out.diffuse.resize(n);
    out.specular.resize(n);
    out.truthD.resize(n);
    out.truthS.resize(n);
    out.normal.resize(n);
    out.depth.resize(n);
    out.motion.resize(n);
    out.instance.resize(n);
    out.surface.resize(n);
    out.gradient.resize(static_cast<usize>(sw) * shh);
    const s32 ti = static_cast<s32>(t);
    const Cam cam = cameraAt(d, ti);
    const Cam prev = cameraAt(d, ti - 1);
    const State st = stateAt(d, ti);
    const State stPrev = stateAt(d, ti - 1);
    out.camera = toRdn(cam);
    out.prevCamera = toRdn(prev);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            const Shade sh = shade(d, st, cam, x + 0.5, y + 0.5);
            out.surface[i] = sh.hit.surface;
            if (sh.hit.surface == kRdnSky) {
                out.diffuse[i] = out.specular[i] = out.truthD[i] = out.truthS[i] = rdnk::float4{};
                out.normal[i] = rdnk::float4(0.f, 0.f, 0.f, 1.f);
                out.depth[i] = 0.f;
                out.motion[i] = rdnk::float2{};
                out.instance[i] = ~0u;
                continue;
            }
            const f64 nd = noiseD(seed, t, i);
            const f64 ns = noiseS(seed, t, i, sh.rough);
            f64 fire = 1.0;
            if (d.firefliesPerMille > 0u && rnd(seed, t, i, 7u) * 1000.0 < d.firefliesPerMille) {
                fire = d.fireflyScale;
            }
            out.truthD[i] = f4(sh.diffuse, 0.0);
            out.truthS[i] = f4(sh.specular, 0.0);
            out.diffuse[i] = f4(sh.diffuse * (nd * fire), sh.hit.surface == kRdnFloor ? 3.0 : 2.0);
            out.specular[i] = f4(sh.specular * ns, sh.hitDist);
            out.normal[i] = f4(sh.n, sh.rough);
            out.depth[i] = static_cast<f32>(dot(sh.hit.p - cam.o, cam.f));
            out.instance[i] = sh.hit.surface;
            // Motion: where the surface point was in the previous frame (the occluder moved with it).
            V3 pp = sh.hit.p;
            if (sh.hit.surface == kRdnOccluder) {
                pp.x -= d.occluderSpeed;
            }
            f64 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            project(cam, sh.hit.p, u0, v0);
            if (project(prev, pp, u1, v1)) {
                out.motion[i] = rdnk::float2(static_cast<f32>(u1 - u0), static_cast<f32>(v1 - v0));
            } else {
                out.motion[i] = rdnk::float2{};
            }
        }
    }
    // A-SVGF samples: the previous camera's ray through q, re-shaded with q's previous random numbers.
    for (u32 sy = 0; sy < shh; ++sy) {
        for (u32 sx = 0; sx < sw; ++sx) {
            const u32 s = sy * sw + sx;
            if (t == 0u) {
                out.gradient[s] = rdnk::float4(0.f, -1.f, 0.f, -1.f);
                continue;
            }
            const u32 k = hash(seed ^ hash(t ^ hash(s))) % 9u;
            const u32 qx = std::min(sx * kRdnStratum + k % 3u, w - 1u);
            const u32 qy = std::min(sy * kRdnStratum + k / 3u, h - 1u);
            const u32 q = qy * w + qx;
            const Shade a = shade(d, stPrev, prev, qx + 0.5, qy + 0.5);
            const Shade b = shade(d, st, prev, qx + 0.5, qy + 0.5);
            if (a.hit.surface == kRdnSky) {
                out.gradient[s] = rdnk::float4(0.f, -1.f, 0.f, -1.f);
                continue;
            }
            const f64 nd = noiseD(seed, t - 1u, q);
            const f64 nsa = noiseS(seed, t - 1u, q, a.rough);
            const f64 nsb = noiseS(seed, t - 1u, q, b.rough);
            out.gradient[s] =
                rdnk::float4(static_cast<f32>(lum(b.diffuse) * nd), static_cast<f32>(lum(a.diffuse) * nd),
                             static_cast<f32>(lum(b.specular) * nsb), static_cast<f32>(lum(a.specular) * nsa));
        }
    }
}

} // namespace fuse::renderer::denoise
