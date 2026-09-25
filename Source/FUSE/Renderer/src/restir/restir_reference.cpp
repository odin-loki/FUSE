// WP-7.2 ReSTIR DI and GI: CPU runner and f64 reference (include/fuse/renderer/restir/restir_reference.hpp).
#include <fuse/renderer/restir/restir_reference.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::restir {

namespace {
/// Work section of a stage (the GPU's ping-pong mapping, RestirBufferLayout without keepIntermediates).
u32 sectionOf(u32 stage) { return stage == 0u ? 0u : (stage == 1u ? 1u : ((stage & 1u) == 0u ? 2u : 1u)); }
} // namespace

// --- RestirCpu ---------------------------------------------------------------------------------------------
struct RestirCpu::Env {
    const RestirSurfaceF* cur = nullptr;
    const RestirSurfaceF* prev = nullptr;
    const RestirDiReservoir* diSrc = nullptr;
    const RestirDiReservoir* diHist = nullptr;
    const RestirGiReservoir* giSrc = nullptr;
    const RestirGiReservoir* giHist = nullptr;
    const f32* motionData = nullptr;
    light_tree::LightTreeView treeView{};
    const RestirLight* lights = nullptr;
    const RestirCpuScene* scene = nullptr;
    RestirCpuStats* stats = nullptr;

    RestirSurfaceF surface(u32 slot, u32 pixel) const { return slot == 0u ? cur[pixel] : prev[pixel]; }
    RestirDiReservoir diSource(u32 pixel) const { return diSrc[pixel]; }
    RestirDiReservoir diHistory(u32 pixel) const { return diHist[pixel]; }
    RestirGiReservoir giSource(u32 pixel) const { return giSrc[pixel]; }
    RestirGiReservoir giHistory(u32 pixel) const { return giHist[pixel]; }
    void motion(u32 pixel, f32& mx, f32& my) const {
        mx = motionData != nullptr ? motionData[pixel * 2u] : 0.f;
        my = motionData != nullptr ? motionData[pixel * 2u + 1u] : 0.f;
    }
    bool occluded(const RV3& o, const RV3& d, f32 tMax) const {
        ++stats->shadowRays;
        return scene->occluded(o, d, tMax);
    }
    bool traceHit(const RV3& o, const RV3& d, f32 tMin, f32 tMax, RestirHitF& hit) const {
        ++stats->giRays;
        return scene->traceHit(o, d, tMin, tMax, hit);
    }
    const light_tree::LightTreeView& tree() const { return treeView; }
    const RestirLight& light(u32 index) const { return lights[index]; }
};

void RestirCpu::reserve(u32 width, u32 height) {
    const usize pixels = static_cast<usize>(width) * height;
    for (u32 k = 0; k < 2u; ++k) {
        m_surfaces[k].resize(pixels);
        m_diHistory[k].resize(pixels);
        m_giHistory[k].resize(pixels);
    }
    for (u32 k = 0; k < 3u; ++k) {
        m_diStage[k].resize(pixels);
        m_giStage[k].resize(pixels);
    }
    m_diSignal.resize(pixels * 4u);
    m_giSignal.resize(pixels * 4u);
    m_depth.resize(pixels);
    if (width != m_width || height != m_height) {
        m_history = false;
    }
    m_width = width;
    m_height = height;
}

bool RestirCpu::runFrame(const RestirSettings& settingsIn, const RestirCamera& camera, u32 width, u32 height, u32 frameIndex,
                         u32 seed, const RestirSurfaceF* surfaces, const f32* motion, const light_tree::LightTreeView& tree,
                         const RestirLight* lights, u32 lightCount, const RestirCpuScene& scene) {
    if (width == 0u || height == 0u || surfaces == nullptr) {
        return false;
    }
    const usize pixels = static_cast<usize>(width) * height;
    if (width != m_width || height != m_height || m_surfaces[0].size() != pixels) {
        reserve(width, height);
        m_history = false;
    }
    const RestirSettings settings = restirSanitize(settingsIn);
    RestirFrameParams params{};
    params.width = width;
    params.height = height;
    params.frameIndex = frameIndex;
    params.seed = seed;
    params.lightCount = lightCount;
    params.history = m_history;
    params.motion = motion != nullptr;
    if (!buildRestirFrameConstants(settings, camera, params, m_constants)) {
        return false;
    }
    const RestirFrameConstants& c = m_constants;
    m_slot ^= 1u;
    const u32 slot = m_slot;
    std::copy(surfaces, surfaces + pixels, m_surfaces[slot].begin());

    Env env{};
    env.cur = m_surfaces[slot].data();
    env.prev = m_surfaces[slot ^ 1u].data();
    env.diHist = m_diHistory[slot ^ 1u].data();
    env.giHist = m_giHistory[slot ^ 1u].data();
    env.motionData = motion;
    env.treeView = tree;
    env.lights = lights;
    env.scene = &scene;
    env.stats = &m_stats;

    const RestirDiReservoir* diFinal = nullptr;
    if (settings.di) {
        std::vector<RestirDiReservoir>& initial = m_diStage[sectionOf(0u)];
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                initial[y * width + x] = rsDiInitial(env, c, x, y);
            }
        }
        u32 last = 0u;
        auto pass = [&](u32 stage, u32 mode, u32 iteration) {
            env.diSrc = m_diStage[sectionOf(last)].data();
            std::vector<RestirDiReservoir>& dst = m_diStage[sectionOf(stage)];
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    dst[y * width + x] = rsDiReuse(env, c, x, y, mode, iteration);
                }
            }
            last = stage;
        };
        if (settings.diTemporal) {
            pass(1u, kRestirModeTemporal, 0u);
        }
        for (u32 i = 0; i < settings.diSpatialIterations; ++i) {
            pass(2u + i, kRestirModeSpatial, i);
        }
        diFinal = m_diStage[sectionOf(last)].data();
    }
    const RestirGiReservoir* giFinal = nullptr;
    if (settings.gi) {
        std::vector<RestirGiReservoir>& initial = m_giStage[sectionOf(0u)];
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                initial[y * width + x] = rsGiInitial(env, c, x, y, nullptr);
            }
        }
        u32 last = 0u;
        auto pass = [&](u32 stage, u32 mode, u32 iteration) {
            env.giSrc = m_giStage[sectionOf(last)].data();
            std::vector<RestirGiReservoir>& dst = m_giStage[sectionOf(stage)];
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    dst[y * width + x] = rsGiReuse(env, c, x, y, mode, iteration);
                }
            }
            last = stage;
        };
        if (settings.giTemporal) {
            pass(1u, kRestirModeTemporal, 0u);
        }
        for (u32 i = 0; i < settings.giSpatialIterations; ++i) {
            pass(2u + i, kRestirModeSpatial, i);
        }
        giFinal = m_giStage[sectionOf(last)].data();
    }
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 p = y * width + x;
            rsShade(env, c, x, y, diFinal != nullptr ? diFinal + p : nullptr, giFinal != nullptr ? giFinal + p : nullptr,
                    &m_diSignal[static_cast<usize>(p) * 4u], &m_giSignal[static_cast<usize>(p) * 4u], m_depth[p]);
            m_diHistory[slot][p] = diFinal != nullptr ? diFinal[p] : RestirDiReservoir{};
            m_giHistory[slot][p] = giFinal != nullptr ? giFinal[p] : RestirGiReservoir{};
        }
    }
    m_history = true;
    ++m_stats.frames;
    return true;
}

// --- RestirReference -------------------------------------------------------------------------------------------
namespace {
constexpr f64 kPi = 3.14159265358979323846;

f64 lum3(const f64 c[3]) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
f64 dot3(const f64 a[3], const f64 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void offsetOrigin(const f64 p[3], const f64 n[3], const RestirRefParams& params, f64 out[3]) {
    const f64 dx = p[0] - params.camera[0];
    const f64 dy = p[1] - params.camera[1];
    const f64 dz = p[2] - params.camera[2];
    const f64 bias = params.normalBias + params.viewBias * std::sqrt(dx * dx + dy * dy + dz * dz);
    for (u32 k = 0; k < 3u; ++k) {
        out[k] = p[k] + n[k] * bias;
    }
}

void basis(const f64 n[3], f64 t[3], f64 b[3]) {
    const f64 sign = n[2] >= 0.0 ? 1.0 : -1.0;
    const f64 a = -1.0 / (sign + n[2]);
    const f64 c = n[0] * n[1] * a;
    t[0] = 1.0 + sign * n[0] * n[0] * a;
    t[1] = sign * c;
    t[2] = -sign * n[0];
    b[0] = c;
    b[1] = sign + n[1] * n[1] * a;
    b[2] = -n[1];
}
} // namespace

f64 RestirReference::Rng::next() {
    // splitmix64 -> 53-bit uniform in [0, 1)
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<f64>(z >> 11) * (1.0 / 9007199254740992.0);
}

bool RestirReference::build(const light_tree::LightTreeView& tree, const RestirLight* lights, u32 count) {
    m_emitters.assign(tree.emitters, tree.emitters + std::min(count, tree.emitterCount));
    m_lights.assign(lights, lights + m_emitters.size());
    m_cdf.resize(m_emitters.size());
    m_prob.resize(m_emitters.size());
    f64 total = 0.0;
    for (usize i = 0; i < m_emitters.size(); ++i) {
        const light_tree::LightTreeEmitter& e = m_emitters[i];
        const f64 rad[3] = {m_lights[i].radiance[0], m_lights[i].radiance[1], m_lights[i].radiance[2]};
        f64 power = lum3(rad);
        if (rsAreaKind(e.kind)) {
            power *= e.area;
        }
        m_prob[i] = power > 0.0 ? power : 0.0;
        total += m_prob[i];
        m_cdf[i] = total;
    }
    if (!(total > 0.0)) {
        return false;
    }
    for (usize i = 0; i < m_cdf.size(); ++i) {
        m_cdf[i] /= total;
        m_prob[i] /= total;
    }
    return true;
}

void RestirReference::sampleDirect(const f64 p[3], const f64 n[3], const RestirReferenceScene& scene,
                                   const RestirRefParams& params, Rng& rng, f64 out[3]) const {
    out[0] = out[1] = out[2] = 0.0;
    const f64 u = rng.next();
    const f64 u1 = rng.next();
    const f64 u2 = rng.next();
    const usize k = static_cast<usize>(std::lower_bound(m_cdf.begin(), m_cdf.end(), u) - m_cdf.begin());
    if (k >= m_emitters.size() || !(m_prob[k] > 0.0)) {
        return;
    }
    const light_tree::LightTreeEmitter& e = m_emitters[k];
    const RestirLight& L = m_lights[k];
    f64 y[3] = {e.p0[0], e.p0[1], e.p0[2]};
    f64 a = 0.0;
    f64 b = 0.0;
    bool area = true;
    if (e.kind == light_tree::kLtKindTriangle) {
        const f64 su = std::sqrt(u1);
        a = su * (1.0 - u2);
        b = su * u2;
    } else if (e.kind == light_tree::kLtKindRect) {
        a = 2.0 * u1 - 1.0;
        b = 2.0 * u2 - 1.0;
    } else if (e.kind == light_tree::kLtKindDisk) {
        const f64 r = std::sqrt(u1);
        const f64 phi = 2.0 * kPi * u2;
        a = r * std::cos(phi);
        b = r * std::sin(phi);
    } else {
        area = false;
    }
    if (area) {
        for (u32 c = 0; c < 3u; ++c) {
            y[c] += static_cast<f64>(e.e1[c]) * a + static_cast<f64>(e.e2[c]) * b;
        }
    }
    const f64 en[3] = {e.normal[0], e.normal[1], e.normal[2]};
    f64 origin[3];
    offsetOrigin(p, n, params, origin);
    f64 g = 0.0;
    f64 dir[3];
    f64 tMax = 0.0;
    if (e.kind == light_tree::kLtKindDirectional) {
        const f64 wi[3] = {-en[0], -en[1], -en[2]};
        const f64 cosX = dot3(n, wi);
        if (!(cosX > 0.0)) {
            return;
        }
        g = cosX;
        for (u32 c = 0; c < 3u; ++c) {
            dir[c] = wi[c];
        }
        tMax = params.farDistance;
    } else {
        const f64 d[3] = {y[0] - p[0], y[1] - p[1], y[2] - p[2]};
        const f64 dist2 = dot3(d, d);
        if (!(dist2 > 1e-24)) {
            return;
        }
        const f64 dist = std::sqrt(dist2);
        const f64 wi[3] = {d[0] / dist, d[1] / dist, d[2] / dist};
        const f64 cosX = dot3(n, wi);
        if (!(cosX > 0.0)) {
            return;
        }
        if (area) {
            f64 cosL = -dot3(en, wi);
            if ((e.flags & light_tree::kLtFlagTwoSided) != 0u) {
                cosL = std::fabs(cosL);
            }
            if (!(cosL > 0.0)) {
                return;
            }
            g = cosX * cosL / dist2;
        } else {
            g = cosX / dist2;
            if (e.kind == light_tree::kLtKindSpot) {
                const f64 cosA = -dot3(en, wi);
                f64 spot = 0.0;
                if (L.cosInner > L.cosOuter) {
                    f64 t = (cosA - L.cosOuter) / (static_cast<f64>(L.cosInner) - L.cosOuter);
                    t = std::clamp(t, 0.0, 1.0);
                    spot = t * t * (3.0 - 2.0 * t);
                } else {
                    spot = cosA >= L.cosOuter ? 1.0 : 0.0;
                }
                g *= spot;
            }
        }
        const f64 to[3] = {y[0] - origin[0], y[1] - origin[1], y[2] - origin[2]};
        const f64 dl = std::sqrt(dot3(to, to));
        if (dl > 1e-9) {
            for (u32 c = 0; c < 3u; ++c) {
                dir[c] = to[c] / dl;
            }
            tMax = dl * static_cast<f64>(kRestirShadowShorten);
        }
    }
    if (!(g > 0.0)) {
        return;
    }
    if (tMax > 0.0 && scene.occluded(origin, dir, tMax)) {
        return;
    }
    const f64 pdf = area ? m_prob[k] / static_cast<f64>(e.area) : m_prob[k];
    for (u32 c = 0; c < 3u; ++c) {
        out[c] = static_cast<f64>(L.radiance[c]) * g / (kPi * pdf);
    }
}

RestirRefEstimate RestirReference::direct(const RestirRefSurface& s, const RestirReferenceScene& scene,
                                          const RestirRefParams& params, u32 samples, u64 seed) const {
    RestirRefEstimate est{};
    if (!s.valid || samples == 0u || m_cdf.empty()) {
        return est;
    }
    Rng rng{seed * 0x2545F4914F6CDD1Dull + 0x1234567ull};
    f64 sum[3] = {0.0, 0.0, 0.0};
    f64 l1 = 0.0;
    f64 l2 = 0.0;
    for (u32 i = 0; i < samples; ++i) {
        f64 v[3];
        sampleDirect(s.p, s.n, scene, params, rng, v);
        for (u32 c = 0; c < 3u; ++c) {
            sum[c] += v[c];
        }
        const f64 l = lum3(v);
        l1 += l;
        l2 += l * l;
    }
    const f64 inv = 1.0 / samples;
    for (u32 c = 0; c < 3u; ++c) {
        est.mean[c] = sum[c] * inv;
    }
    est.luminance = l1 * inv;
    const f64 var = std::max(0.0, l2 * inv - est.luminance * est.luminance);
    est.stdError = std::sqrt(var * inv);
    est.samples = samples;
    return est;
}

RestirRefEstimate RestirReference::indirect(const RestirRefSurface& s, const RestirReferenceScene& scene,
                                            const RestirRefParams& params, u32 samples, u64 seed) const {
    RestirRefEstimate est{};
    if (!s.valid || samples == 0u || m_cdf.empty()) {
        return est;
    }
    Rng rng{seed * 0x9E3779B97F4A7C15ull + 0x7654321ull};
    f64 t[3];
    f64 b[3];
    basis(s.n, t, b);
    f64 sum[3] = {0.0, 0.0, 0.0};
    f64 l1 = 0.0;
    f64 l2 = 0.0;
    for (u32 i = 0; i < samples; ++i) {
        const f64 r = std::sqrt(rng.next());
        const f64 phi = 2.0 * kPi * rng.next();
        const f64 x = r * std::cos(phi);
        const f64 y = r * std::sin(phi);
        const f64 z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
        f64 dir[3];
        for (u32 c = 0; c < 3u; ++c) {
            dir[c] = t[c] * x + b[c] * y + s.n[c] * z;
        }
        const f64 dl = std::sqrt(dot3(dir, dir));
        for (u32 c = 0; c < 3u; ++c) {
            dir[c] /= dl;
        }
        f64 v[3] = {0.0, 0.0, 0.0};
        f64 th = 0.0;
        f64 hn[3];
        f64 albedo[3];
        if (scene.traceHit(s.p, dir, params.giRayTMin, params.farDistance, th, hn, albedo)) {
            f64 xs[3];
            for (u32 c = 0; c < 3u; ++c) {
                xs[c] = s.p[c] + dir[c] * th;
            }
            const f64 nl = std::sqrt(dot3(hn, hn));
            f64 ns[3] = {hn[0] / nl, hn[1] / nl, hn[2] / nl};
            if (dot3(ns, dir) > 0.0) {
                for (f64& c : ns) {
                    c = -c;
                }
            }
            f64 d[3];
            sampleDirect(xs, ns, scene, params, rng, d);
            for (u32 c = 0; c < 3u; ++c) {
                v[c] = albedo[c] * d[c];
            }
        } else {
            rng.next(); // keep the stream aligned (3 numbers per next-event sample)
            rng.next();
            rng.next();
        }
        for (u32 c = 0; c < 3u; ++c) {
            sum[c] += v[c];
        }
        const f64 l = lum3(v);
        l1 += l;
        l2 += l * l;
    }
    const f64 inv = 1.0 / samples;
    for (u32 c = 0; c < 3u; ++c) {
        est.mean[c] = sum[c] * inv;
    }
    est.luminance = l1 * inv;
    const f64 var = std::max(0.0, l2 * inv - est.luminance * est.luminance);
    est.stdError = std::sqrt(var * inv);
    est.samples = samples;
    return est;
}

} // namespace fuse::renderer::restir
