// WP-7.3 path-tracing mode: the CPU reference path tracer. See include/fuse/renderer/pathtrace/pt_reference.hpp.
#include <fuse/renderer/pathtrace/pt_reference.hpp>

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/rt/rt_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace fuse::renderer::pathtrace {

namespace {

constexpr f64 kPi = 3.14159265358979323846;

PtV3d v3(f64 x, f64 y, f64 z) { return PtV3d{x, y, z}; }
PtV3d load(const f32 (&a)[3]) { return PtV3d{a[0], a[1], a[2]}; }
f64 maxc(const PtV3d& a) { return ptMaxComponent(a); }

u64 splitmix(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
f64 rand01(u64& state) { return static_cast<f64>(splitmix(state) >> 11) * (1.0 / 9007199254740992.0); }

PtV3d transformPoint(const f64 (&m)[3][4], const f32* p) {
    const f64 x = p[0];
    const f64 y = p[1];
    const f64 z = p[2];
    return v3(m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3], m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
              m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]);
}

PtV3d unproject(const f64 (&inv)[16], f64 x, f64 y, f64 z) {
    const f64 w = inv[3] * x + inv[7] * y + inv[11] * z + inv[15];
    return v3((inv[0] * x + inv[4] * y + inv[8] * z + inv[12]) / w, (inv[1] * x + inv[5] * y + inv[9] * z + inv[13]) / w,
              (inv[2] * x + inv[6] * y + inv[10] * z + inv[14]) / w);
}

bool slab(const PtV3d& o, const PtV3d& d, const PtV3d& lo, const PtV3d& hi, f64 tMin, f64 tMax) {
    const f64 os[3] = {o.x, o.y, o.z};
    const f64 ds[3] = {d.x, d.y, d.z};
    const f64 ls[3] = {lo.x, lo.y, lo.z};
    const f64 hs[3] = {hi.x, hi.y, hi.z};
    for (u32 a = 0; a < 3u; ++a) {
        if (ds[a] == 0.0) {
            if (os[a] < ls[a] || os[a] > hs[a]) {
                return false;
            }
            continue;
        }
        const f64 inv = 1.0 / ds[a];
        f64 t0 = (ls[a] - os[a]) * inv;
        f64 t1 = (hs[a] - os[a]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }
    return true;
}

} // namespace

// --- scene ------------------------------------------------------------------------------------------------------

bool PtCpuScene::build(const gpu_scene::GpuScene& scene, const u16* const* vpos, u32 meshCount) {
    m_instances.clear();
    m_triangles.clear();
    const gpu_scene::TableBytes materials = scene.tableBytes(gpu_scene::GpuSceneTable::Materials);
    std::vector<f32> decoded;
    for (u32 slot = 0; slot < scene.instanceHighWater(); ++slot) {
        const gpu_scene::GpuInstance& gi = scene.instance(slot);
        const u32 mask = rt::rtInstanceMask(gi.flags, 1u);
        if (mask == 0u) {
            continue;
        }
        if (gi.mesh >= meshCount || gi.mesh >= scene.meshCount() || vpos[gi.mesh] == nullptr) {
            return false;
        }
        const gpu_scene::GpuMesh& mesh = scene.mesh(gi.mesh);
        if (mesh.indexCount == 0u || static_cast<u64>(mesh.firstIndex) + mesh.indexCount > scene.indexCount()) {
            return false;
        }
        const gpu_scene::GpuTransform& xf = scene.transform(slot);
        f64 m[3][4];
        for (u32 r = 0; r < 3u; ++r) {
            for (u32 c = 0; c < 4u; ++c) {
                m[r][c] = xf.rows[r][c];
            }
        }
        decoded.assign(static_cast<size_t>(mesh.vertexCount) * 3u, 0.f);
        for (u32 v = 0; v < mesh.vertexCount; ++v) {
            rt::rtDecodePosition(mesh, vpos[gi.mesh], v, &decoded[static_cast<size_t>(v) * 3u]);
        }
        Instance inst{};
        inst.slot = slot;
        inst.mask = mask;
        inst.firstTriangle = static_cast<u32>(m_triangles.size());
        inst.lo = v3(1e300, 1e300, 1e300);
        inst.hi = v3(-1e300, -1e300, -1e300);
        const u32* idx = scene.indexData() + mesh.firstIndex;
        for (u32 t = 0; t * 3u < mesh.indexCount; ++t) {
            PtV3d w[3];
            for (u32 k = 0; k < 3u; ++k) {
                w[k] = transformPoint(m, &decoded[static_cast<size_t>(idx[t * 3u + k]) * 3u]);
                inst.lo = v3(std::min(inst.lo.x, w[k].x), std::min(inst.lo.y, w[k].y), std::min(inst.lo.z, w[k].z));
                inst.hi = v3(std::max(inst.hi.x, w[k].x), std::max(inst.hi.y, w[k].y), std::max(inst.hi.z, w[k].z));
            }
            Triangle tri{};
            tri.p0 = w[0];
            tri.e1 = w[1] - w[0];
            tri.e2 = w[2] - w[0];
            tri.normal = ptNormalize(ptCross(tri.e1, tri.e2));
            tri.primitive = t;
            m_triangles.push_back(tri);
        }
        inst.triangleCount = static_cast<u32>(m_triangles.size()) - inst.firstTriangle;
        const f64 pad = 1e-9 * (1.0 + maxc(v3(std::fabs(inst.hi.x), std::fabs(inst.hi.y), std::fabs(inst.hi.z))));
        inst.lo = inst.lo - v3(pad, pad, pad);
        inst.hi = inst.hi + v3(pad, pad, pad);
        if (gi.material < materials.count && materials.data != nullptr) {
            Material::GPUMaterial row{};
            std::memcpy(&row, materials.data + static_cast<size_t>(gi.material) * materials.stride, sizeof(row));
            inst.material.albedo = v3(std::max(row.baseColor.x, 0.f), std::max(row.baseColor.y, 0.f), std::max(row.baseColor.z, 0.f));
            inst.material.metallic = std::clamp(static_cast<f64>(row.baseColor.w), 0.0, 1.0);
            inst.material.roughness = std::clamp(static_cast<f64>(row.roughnessEmissive.x), 0.0, 1.0);
            const f64 k = std::max(static_cast<f64>(row.emissiveIntensity), 0.0);
            inst.emission = v3(std::max(row.roughnessEmissive.y, 0.f) * k, std::max(row.roughnessEmissive.z, 0.f) * k,
                               std::max(row.roughnessEmissive.w, 0.f) * k);
        }
        m_instances.push_back(inst);
    }
    return true;
}

void PtCpuScene::setLights(const light_tree::LightTreeEmitter* emitters, const restir::RestirLight* table, u32 count, const u32* emitterMap,
                           u32 mapWords, u32 mapSlots) {
    m_lights.assign(count, PtRefLight{});
    m_pmf.assign(count, 0.0);
    m_cdf.assign(count, 0.0);
    f64 total = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const light_tree::LightTreeEmitter& e = emitters[i];
        PtRefLight& l = m_lights[i];
        l.kind = e.kind;
        l.p0 = load(e.p0);
        l.e1 = load(e.e1);
        l.e2 = load(e.e2);
        l.normal = ptNormalize(load(e.normal));
        l.area = e.area;
        l.twoSided = (e.flags & light_tree::kLtFlagTwoSided) != 0u;
        l.radiance = v3(table[i].radiance[0], table[i].radiance[1], table[i].radiance[2]);
        l.cosInner = table[i].cosInner;
        l.cosOuter = table[i].cosOuter;
        const bool area = l.kind == light_tree::kLtKindTriangle || l.kind == light_tree::kLtKindRect || l.kind == light_tree::kLtKindDisk;
        m_pmf[i] = ptLuminance(l.radiance) * (area ? l.area : 1.0);
        total += m_pmf[i];
    }
    // Floor every light at 1e-3 of the mean so none has probability 0 (the estimator stays unbiased).
    const f64 floor = count > 0u ? 1e-3 * (total > 0.0 ? total / count : 1.0) : 0.0;
    total = 0.0;
    for (u32 i = 0; i < count; ++i) {
        m_pmf[i] = std::max(m_pmf[i], floor);
        total += m_pmf[i];
    }
    f64 run = 0.0;
    for (u32 i = 0; i < count; ++i) {
        m_pmf[i] /= total;
        run += m_pmf[i];
        m_cdf[i] = run;
    }
    if (count > 0u) {
        m_cdf[count - 1u] = 1.0;
    }
    m_map.assign(emitterMap != nullptr ? emitterMap : nullptr, emitterMap != nullptr ? emitterMap + mapWords : nullptr);
    m_mapSlots = emitterMap != nullptr ? mapSlots : 0u;
}

u32 PtCpuScene::pickLight(f64 u) const {
    const auto it = std::upper_bound(m_cdf.begin(), m_cdf.end(), u);
    const u32 i = static_cast<u32>(it - m_cdf.begin());
    return i < m_cdf.size() ? i : static_cast<u32>(m_cdf.size()) - 1u;
}

bool PtCpuScene::hitTriangles(const Instance& inst, const PtV3d& o, const PtV3d& d, f64 tMin, f64& tBest, u32& best, f64& bu,
                              f64& bv) const {
    bool any = false;
    for (u32 i = 0; i < inst.triangleCount; ++i) {
        const Triangle& tri = m_triangles[inst.firstTriangle + i];
        const PtV3d p = ptCross(d, tri.e2);
        const f64 det = ptDot(tri.e1, p);
        if (det == 0.0) {
            continue;
        }
        const f64 inv = 1.0 / det;
        const PtV3d s = o - tri.p0;
        const f64 u = ptDot(s, p) * inv;
        if (u < 0.0 || u > 1.0) {
            continue;
        }
        const PtV3d q = ptCross(s, tri.e1);
        const f64 v = ptDot(d, q) * inv;
        if (v < 0.0 || u + v > 1.0) {
            continue;
        }
        const f64 t = ptDot(tri.e2, q) * inv;
        if (t > tMin && t < tBest) {
            tBest = t;
            best = inst.firstTriangle + i;
            bu = u;
            bv = v;
            any = true;
        }
    }
    return any;
}

PtRefHit PtCpuScene::trace(const PtV3d& o, const PtV3d& d, f64 tMin, f64 tMax, u32 cullMask) const {
    PtRefHit h{};
    f64 tBest = tMax;
    u32 best = kPtInvalid;
    u32 bestInstance = kPtInvalid;
    f64 bu = 0.0;
    f64 bv = 0.0;
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const Instance& inst = m_instances[i];
        if ((inst.mask & cullMask) == 0u || !slab(o, d, inst.lo, inst.hi, tMin, tBest)) {
            continue;
        }
        if (hitTriangles(inst, o, d, tMin, tBest, best, bu, bv)) {
            bestInstance = i;
        }
    }
    if (bestInstance == kPtInvalid) {
        return h;
    }
    const Instance& inst = m_instances[bestInstance];
    const Triangle& tri = m_triangles[best];
    h.hit = true;
    h.t = tBest;
    h.position = tri.p0 + tri.e1 * bu + tri.e2 * bv;
    h.normal = tri.normal;
    h.instance = inst.slot;
    h.primitive = tri.primitive;
    h.material = inst.material;
    h.emission = inst.emission;
    h.emitter = ptEmitterLookup(m_map.data(), static_cast<u32>(m_map.size()), m_mapSlots, inst.slot, tri.primitive);
    if (h.emitter != kPtInvalid && h.emitter >= m_lights.size()) {
        h.emitter = kPtInvalid;
    }
    return h;
}

bool PtCpuScene::occluded(const PtV3d& o, const PtV3d& d, f64 tMax, u32 cullMask) const {
    for (const Instance& inst : m_instances) {
        if ((inst.mask & cullMask) == 0u || !slab(o, d, inst.lo, inst.hi, 0.0, tMax)) {
            continue;
        }
        f64 t = tMax;
        u32 best = kPtInvalid;
        f64 bu = 0.0;
        f64 bv = 0.0;
        if (hitTriangles(inst, o, d, 0.0, t, best, bu, bv)) {
            return true;
        }
    }
    return false;
}

// --- reference path tracer ----------------------------------------------------------------------------------------

bool PtReference::setup(const PtSettings& settings, const PtCamera& camera, u32 width, u32 height) {
    m_ready = false;
    if (width == 0u || height == 0u || !ptInvert(camera.viewProj, m_inv)) {
        return false;
    }
    m_settings = ptSanitize(settings);
    m_camera = v3(camera.position[0], camera.position[1], camera.position[2]);
    m_width = width;
    m_height = height;
    m_ready = true;
    return true;
}

PtV3d PtReference::path(const PtCpuScene& scene, u32 x, u32 y, u64& rng) const {
    const PtSettings& s = m_settings;
    const bool lights = scene.lightCount() > 0u;
    const bool nee = lights && s.strategy != PtStrategy::BsdfOnly;
    const bool emitterHits = lights && s.strategy != PtStrategy::NeeOnly;
    const f64 minRough = s.minRoughness;
    const f64 nx = (x + rand01(rng)) * 2.0 / m_width - 1.0;
    const f64 ny = (y + rand01(rng)) * 2.0 / m_height - 1.0;
    PtV3d o = m_camera;
    PtV3d d = ptNormalize(unproject(m_inv, nx, ny, 0.5) - m_camera);
    PtV3d throughput = v3(1.0, 1.0, 1.0);
    PtV3d radiance{};
    PtV3d prevP{};
    f64 prevPdf = 0.0;
    const PtV3d sky = v3(s.sky[0], s.sky[1], s.sky[2]);
    for (u32 depth = 0; depth <= s.maxBounces; ++depth) {
        const PtRefHit h = scene.trace(o, d, 0.0, s.farDistance, s.cullMask);
        if (!h.hit) {
            radiance = radiance + ptMul(throughput, sky);
            break;
        }
        const bool front = ptDot(h.normal, d) < 0.0;
        const PtV3d n = front ? h.normal : h.normal * -1.0;
        if (h.emitter != kPtInvalid) {
            const PtRefLight& l = scene.light(h.emitter);
            f64 cosL = -ptDot(l.normal, d);
            if (l.twoSided) {
                cosL = std::fabs(cosL);
            }
            if (cosL > 0.0) {
                f64 w = 1.0;
                if (depth > 0u) {
                    if (!emitterHits) {
                        w = 0.0;
                    } else if (nee && l.area > 0.0) {
                        const PtV3d dl = h.position - prevP;
                        const f64 pdfLight = scene.lightPmf(h.emitter) * ptDot(dl, dl) / (cosL * l.area);
                        w = ptPowerHeuristic(prevPdf, pdfLight);
                    }
                }
                radiance = radiance + ptMul(throughput, l.radiance) * w;
            }
        } else if (front) {
            radiance = radiance + ptMul(throughput, h.emission);
        }
        if (depth == s.maxBounces) {
            break;
        }
        PtV3d t;
        PtV3d b;
        ptBasis(n, t, b);
        const PtV3d wo = d * -1.0;
        const PtV3d woL = v3(ptDot(wo, t), ptDot(wo, b), ptDot(wo, n));
        const PtV3d origin = h.position + n * (s.normalBias + s.viewBias * ptLength(h.position - m_camera));
        if (nee) {
            const u32 li = scene.pickLight(rand01(rng));
            const f64 u1 = rand01(rng);
            const f64 u2 = rand01(rng);
            const PtRefLight& l = scene.light(li);
            const f64 pmf = scene.lightPmf(li);
            PtV3d wi{};
            PtV3d le{};
            f64 pdfLight = 0.0;
            f64 tMax = s.farDistance;
            bool area = false;
            bool ok = true;
            PtV3d target{};
            if (l.kind == light_tree::kLtKindDirectional) {
                wi = l.normal * -1.0;
                le = l.radiance;
                pdfLight = pmf;
            } else {
                if (l.kind == light_tree::kLtKindTriangle) {
                    const f64 su = std::sqrt(u1);
                    target = l.p0 + l.e1 * (su * (1.0 - u2)) + l.e2 * (su * u2);
                    area = true;
                } else if (l.kind == light_tree::kLtKindRect) {
                    target = l.p0 + l.e1 * (2.0 * u1 - 1.0) + l.e2 * (2.0 * u2 - 1.0);
                    area = true;
                } else if (l.kind == light_tree::kLtKindDisk) {
                    // Concentric disk mapping (Shirley and Chiu 1997).
                    const f64 a = 2.0 * u1 - 1.0;
                    const f64 c = 2.0 * u2 - 1.0;
                    f64 rx = 0.0;
                    f64 ry = 0.0;
                    if (a != 0.0 || c != 0.0) {
                        if (std::fabs(a) > std::fabs(c)) {
                            rx = a * std::cos(kPi / 4.0 * (c / a));
                            ry = a * std::sin(kPi / 4.0 * (c / a));
                        } else {
                            rx = c * std::sin(kPi / 4.0 * (a / c));
                            ry = c * std::cos(kPi / 4.0 * (a / c));
                        }
                    }
                    target = l.p0 + l.e1 * rx + l.e2 * ry;
                    area = true;
                } else {
                    target = l.p0;
                }
                const PtV3d dv = target - h.position;
                const f64 dist2 = ptDot(dv, dv);
                ok = dist2 > 1e-24;
                if (ok) {
                    wi = dv * (1.0 / std::sqrt(dist2));
                    if (area) {
                        f64 cosL = -ptDot(l.normal, wi);
                        if (l.twoSided) {
                            cosL = std::fabs(cosL);
                        }
                        ok = cosL > 0.0 && l.area > 0.0;
                        le = l.radiance;
                        pdfLight = ok ? pmf * dist2 / (cosL * l.area) : 0.0;
                    } else {
                        f64 spot = 1.0;
                        if (l.kind == light_tree::kLtKindSpot) {
                            const f64 cosA = -ptDot(l.normal, wi);
                            if (l.cosInner > l.cosOuter) {
                                const f64 xs = std::clamp((cosA - l.cosOuter) / (l.cosInner - l.cosOuter), 0.0, 1.0);
                                spot = xs * xs * (3.0 - 2.0 * xs);
                            } else {
                                spot = cosA >= l.cosOuter ? 1.0 : 0.0;
                            }
                        }
                        le = l.radiance * (spot / dist2);
                        pdfLight = pmf;
                    }
                    tMax = ptLength(target - origin) * static_cast<f64>(kPtShadowShorten);
                }
            }
            const f64 cosX = ptDot(n, wi);
            if (ok && cosX > 0.0 && pdfLight > 0.0) {
                const PtV3d wiL = v3(ptDot(wi, t), ptDot(wi, b), cosX);
                const PtV3d f = ptBsdfEval(h.material, minRough, woL, wiL);
                if (maxc(f) > 0.0 && maxc(le) > 0.0) {
                    const f64 w = area && emitterHits ? ptPowerHeuristic(pdfLight, ptBsdfPdf(h.material, minRough, woL, wiL)) : 1.0;
                    const PtV3d dir = l.kind == light_tree::kLtKindDirectional ? wi : ptNormalize(target - origin);
                    if (!scene.occluded(origin, dir, tMax, s.cullMask)) {
                        radiance = radiance + ptMul(throughput, ptMul(f, le)) * (cosX * w / pdfLight);
                    }
                }
            }
        }
        const f64 uLobe = rand01(rng);
        const f64 u1 = rand01(rng);
        const f64 u2 = rand01(rng);
        const PtBsdfSample<f64> bs = ptBsdfSample(h.material, minRough, woL, uLobe, u1, u2);
        if (!bs.valid) {
            break;
        }
        throughput = ptMul(throughput, bs.weight);
        prevP = h.position;
        prevPdf = bs.pdf;
        o = origin;
        d = ptNormalize(t * bs.wi.x + b * bs.wi.y + n * bs.wi.z);
        if (s.russianRoulette && depth + 1u >= s.rrStartBounce) {
            const f64 q = std::min(maxc(throughput), 0.95);
            if (!(rand01(rng) < q)) {
                break;
            }
            throughput = throughput * (1.0 / q);
        }
    }
    if (s.clampRadiance > 0.f) {
        const f64 l = ptLuminance(radiance);
        if (l > s.clampRadiance) {
            radiance = radiance * (s.clampRadiance / l);
        }
    }
    return radiance;
}

void PtReference::renderPixel(const PtCpuScene& scene, u32 x, u32 y, u32 firstSample, u32 samples, u64 seed, PtPixelStats& st) const {
    if (!m_ready) {
        return;
    }
    const u64 pixel = static_cast<u64>(y) * m_width + x;
    for (u32 i = 0; i < samples; ++i) {
        u64 rng = seed ^ (pixel * 0xD1B54A32D192ED03ull) ^ (static_cast<u64>(firstSample + i) * 0x8CB92BA72F3D8DD7ull);
        splitmix(rng);
        const PtV3d l = path(scene, x, y, rng);
        const f64 c[3] = {l.x, l.y, l.z};
        for (u32 k = 0; k < 3u; ++k) {
            st.sum[k] += c[k];
            st.sumSq[k] += c[k] * c[k];
        }
        ++st.samples;
    }
}

bool PtReference::render(const PtCpuScene& scene, u32 samples, u64 seed, u32 threads, PtReferenceImage& out) const {
    if (!m_ready || samples == 0u) {
        return false;
    }
    out.width = m_width;
    out.height = m_height;
    out.samples = samples;
    const size_t pixels = static_cast<size_t>(m_width) * m_height;
    out.mean.assign(pixels * 3u, 0.0);
    out.variance.assign(pixels * 3u, 0.0);
    auto work = [&](u32 worker, u32 workers) {
        for (size_t p = worker; p < pixels; p += workers) {
            PtPixelStats st{};
            renderPixel(scene, static_cast<u32>(p % m_width), static_cast<u32>(p / m_width), 0u, samples, seed, st);
            const f64 n = static_cast<f64>(st.samples);
            for (u32 k = 0; k < 3u; ++k) {
                const f64 mean = st.sum[k] / n;
                const f64 var = n > 1.0 ? std::max(st.sumSq[k] - n * mean * mean, 0.0) / (n - 1.0) : 0.0;
                out.mean[p * 3u + k] = mean;
                out.variance[p * 3u + k] = var / n;
            }
        }
    };
    if (threads <= 1u) {
        work(0u, 1u);
        return true;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (u32 i = 0; i < threads; ++i) {
        pool.emplace_back(work, i, threads);
    }
    for (std::thread& t : pool) {
        t.join();
    }
    return true;
}

} // namespace fuse::renderer::pathtrace
