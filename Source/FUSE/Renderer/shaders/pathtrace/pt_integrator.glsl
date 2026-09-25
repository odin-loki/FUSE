// WP-7.3 path-tracing mode: the integrator (GLSL twin of pt_integrator.slang). Shared by the T3 ray-generation
// shader (pt_raygen.rgen: rays through traceRayEXT and the SBT's hit groups) and the T2 ray-query kernel
// (pt_trace.comp: SBT-free, the same surface function called in place). The includer defines, before including:
//
//   bool pt_trace(PtFrame F, vec3 o, vec3 d, float tMin, float tMax, uint originInstance, uint originPrimitive,
//                 out PtHit hit);     // closest hit, skipping the origin triangle (any-hit / candidate loop)
//   bool pt_occluded(PtFrame F, vec3 o, vec3 d, float tMax, uint originInstance, uint originPrimitive);
//
// Estimator (pathtrace.hpp; the CPU reference pt_reference.cpp implements the same integral independently):
//   camera ray -> for each vertex: emission (MIS-weighted when a BSDF ray hit a light-tree emitter), stop at
//   maxBounces, NEE via lt_sample (area lights MIS-weighted against the BSDF pdf), BSDF sample, Russian roulette.
#ifndef FUSE_PT_INTEGRATOR_GLSL
#define FUSE_PT_INTEGRATOR_GLSL

vec3 pt_unproject(PtFrame F, float nx, float ny, float nz) {
    const vec4 c = vec4(nx, ny, nz, 1.0);
    const float x = F.invViewProj[0] * c.x + F.invViewProj[4] * c.y + F.invViewProj[8] * c.z + F.invViewProj[12];
    const float y = F.invViewProj[1] * c.x + F.invViewProj[5] * c.y + F.invViewProj[9] * c.z + F.invViewProj[13];
    const float z = F.invViewProj[2] * c.x + F.invViewProj[6] * c.y + F.invViewProj[10] * c.z + F.invViewProj[14];
    const float w = F.invViewProj[3] * c.x + F.invViewProj[7] * c.y + F.invViewProj[11] * c.z + F.invViewProj[15];
    return vec3(x, y, z) / w;
}

struct PtPrimary {
    bool hit;
    vec3 position;
    vec3 normal;
    vec3 albedo;
};

// Next-event estimate at a vertex (f cos Le / pdf x MIS weight, visibility tested); zero when nothing is sampled.
vec3 pt_nee(PtFrame F, PtHit h, vec3 p, vec3 n, vec3 origin, vec3 wo, vec3 t, vec3 b, PtMaterial m, inout uint rng) {
    const float u0 = pt_rand(rng);
    const float u1 = pt_rand(rng);
    const float u2 = pt_rand(rng);
    const LtSampleResult s = lt_sample(F.lightTree, p, n, u0, u1, u2);
    if (s.light == LT_INVALID || s.light >= F.lightCount || !(s.pmf > 0.0)) {
        return vec3(0.0);
    }
    const PtLight L = PtLightsRef(F.lights).v[s.light];
    const LtEmitter em = LtEmittersRef(LtHeaderRef(F.lightTree).h.emitters).v[s.light];
    const vec3 radiance = pt_load(L.radiance);
    const vec3 y = vec3(s.position[0], s.position[1], s.position[2]);
    const vec3 woL = vec3(dot(wo, t), dot(wo, b), dot(wo, n));
    vec3 wi;
    vec3 le;
    float pdfLight;
    float tMax;
    bool area = false;
    if (s.kind == LT_KIND_DIRECTIONAL) {
        wi = -y;
        le = radiance;
        pdfLight = s.pmf;
        tMax = F.farDistance;
    } else {
        const vec3 d = y - p;
        const float dist2 = dot(d, d);
        if (!(dist2 > 1e-12)) {
            return vec3(0.0);
        }
        wi = d / sqrt(dist2);
        if (s.kind == LT_KIND_TRIANGLE || s.kind == LT_KIND_RECT || s.kind == LT_KIND_DISK) {
            float cosL = -dot(pt_load(em.normal), wi);
            if ((em.flags & LT_FLAG_TWO_SIDED) != 0u) {
                cosL = abs(cosL);
            }
            if (!(cosL > 0.0) || !(s.pdfArea > 0.0)) {
                return vec3(0.0);
            }
            le = radiance;
            pdfLight = s.pmf * s.pdfArea * dist2 / cosL;
            area = true;
        } else {
            float spot = 1.0;
            if (s.kind == LT_KIND_SPOT) {
                const float cosA = -dot(pt_load(em.normal), wi);
                if (L.cosInner > L.cosOuter) {
                    const float x = clamp((cosA - L.cosOuter) / (L.cosInner - L.cosOuter), 0.0, 1.0);
                    spot = x * x * (3.0 - 2.0 * x);
                } else {
                    spot = cosA >= L.cosOuter ? 1.0 : 0.0;
                }
            }
            le = radiance * (spot / dist2);
            pdfLight = s.pmf;
        }
        const vec3 toLight = y - origin;
        tMax = length(toLight) * PT_SHADOW_SHORTEN;
    }
    const float cosX = dot(n, wi);
    if (!(cosX > 0.0)) {
        return vec3(0.0);
    }
    const vec3 wiL = vec3(dot(wi, t), dot(wi, b), cosX);
    const vec3 f = pt_bsdf_eval(m, F.minRoughness, woL, wiL);
    if (!(pt_max3(f) > 0.0) || !(pt_max3(le) > 0.0)) {
        return vec3(0.0);
    }
    float w = 1.0;
    if (area && (F.flags & PT_FLAG_EMITTER_HITS) != 0u) {
        w = pt_power_heuristic(pdfLight, pt_bsdf_pdf(m, F.minRoughness, woL, wiL));
    }
    vec3 dir = wi;
    if (s.kind != LT_KIND_DIRECTIONAL) {
        dir = normalize(y - origin);
    }
    if (pt_occluded(F, origin, dir, tMax, h.instance, h.primitive)) {
        return vec3(0.0);
    }
    return f * le * (cosX * w / pdfLight);
}

// One path through pixel (px, py). Returns the radiance; `primary` receives the camera ray's surface.
vec3 pt_path(PtFrame F, uint px, uint py, inout uint rng, out PtPrimary primary) {
    primary.hit = false;
    primary.position = vec3(0.0);
    primary.normal = vec3(0.0);
    primary.albedo = vec3(0.0);
    const float jx = pt_rand(rng);
    const float jy = pt_rand(rng);
    const float nx = (float(px) + jx) * 2.0 / float(F.width) - 1.0;
    const float ny = (float(py) + jy) * 2.0 / float(F.height) - 1.0;
    const vec3 camera = pt_load(F.cameraPosition);
    vec3 o = camera;
    vec3 d = normalize(pt_unproject(F, nx, ny, 0.5) - camera);
    vec3 throughput = vec3(1.0);
    vec3 radiance = vec3(0.0);
    vec3 prevP = vec3(0.0);
    vec3 prevN = vec3(0.0);
    float prevPdf = 0.0;
    uint originInstance = PT_INVALID;
    uint originPrimitive = PT_INVALID;
    for (uint depth = 0u; depth <= F.maxBounces; ++depth) {
        PtHit h;
        if (!pt_trace(F, o, d, F.rayTMin, F.farDistance, originInstance, originPrimitive, h)) {
            radiance += throughput * pt_load(F.sky);
            break;
        }
        const vec3 p = pt_load(h.position);
        const vec3 ng = pt_load(h.normal);
        const bool front = dot(ng, d) < 0.0;
        const vec3 n = front ? ng : -ng;
        if (depth == 0u) {
            primary.hit = true;
            primary.position = p;
            primary.normal = n;
            primary.albedo = pt_load(h.albedo);
        }
        // Emission.
        if (h.emitter != PT_INVALID && h.emitter < F.lightCount) {
            const LtEmitter em = LtEmittersRef(LtHeaderRef(F.lightTree).h.emitters).v[h.emitter];
            float cosL = -dot(pt_load(em.normal), d);
            if ((em.flags & LT_FLAG_TWO_SIDED) != 0u) {
                cosL = abs(cosL);
            }
            if (cosL > 0.0) {
                float w = 1.0;
                if (depth > 0u) {
                    if ((F.flags & PT_FLAG_EMITTER_HITS) == 0u) {
                        w = 0.0;
                    } else if ((F.flags & PT_FLAG_NEE) != 0u && em.area > 0.0) {
                        const vec3 dl = p - prevP;
                        const float pdfLight = lt_pmf(F.lightTree, prevP, prevN, h.emitter) * (dot(dl, dl) / (cosL * em.area));
                        w = pt_power_heuristic(prevPdf, pdfLight);
                    }
                }
                radiance += throughput * pt_load(PtLightsRef(F.lights).v[h.emitter].radiance) * w;
            }
        } else if (front) {
            radiance += throughput * pt_load(h.emission);
        }
        if (depth == F.maxBounces) {
            break;
        }
        const vec3 wo = -d;
        vec3 t;
        vec3 b;
        pt_basis(n, t, b);
        PtMaterial m;
        m.albedo = pt_load(h.albedo);
        m.metallic = h.metallic;
        m.roughness = h.roughness;
        const vec3 origin = p + n * (F.normalBias + F.viewBias * length(p - camera));
        if ((F.flags & PT_FLAG_NEE) != 0u) {
            radiance += throughput * pt_nee(F, h, p, n, origin, wo, t, b, m, rng);
        } else {
            pt_rand(rng);
            pt_rand(rng);
            pt_rand(rng);
        }
        const vec3 woL = vec3(dot(wo, t), dot(wo, b), dot(wo, n));
        const float uLobe = pt_rand(rng);
        const float u1 = pt_rand(rng);
        const float u2 = pt_rand(rng);
        vec3 wiL;
        float pdf;
        vec3 weight;
        if (!pt_bsdf_sample(m, F.minRoughness, woL, uLobe, u1, u2, wiL, pdf, weight)) {
            break;
        }
        throughput *= weight;
        prevP = p;
        prevN = n;
        prevPdf = pdf;
        o = origin;
        d = normalize(t * wiL.x + b * wiL.y + n * wiL.z);
        originInstance = h.instance;
        originPrimitive = h.primitive;
        const uint bounces = depth + 1u;
        if ((F.flags & PT_FLAG_RUSSIAN_ROULETTE) != 0u && bounces >= F.rrStartBounce) {
            const float q = min(pt_max3(throughput), 0.95);
            if (!(pt_rand(rng) < q)) {
                break;
            }
            throughput /= q;
        }
    }
    if ((F.flags & PT_FLAG_CLAMP) != 0u) {
        const float l = pt_luminance(radiance);
        if (l > F.clampRadiance) {
            radiance *= F.clampRadiance / l;
        }
    }
    return radiance;
}

// samplesPerFrame paths of pixel (px, py), accumulated; writes the output mean and the guides.
void pt_render_pixel(PtFrame F, uint px, uint py) {
    const uint pixel = py * F.width + px;
    vec3 sum = vec3(0.0);
    vec3 sumSq = vec3(0.0);
    PtPrimary first;
    first.hit = false;
    first.position = vec3(0.0);
    first.normal = vec3(0.0);
    first.albedo = vec3(0.0);
    for (uint s = 0u; s < F.samplesPerFrame; ++s) {
        uint rng = pt_seed(pixel, F.sampleBase + s, F.seed);
        PtPrimary primary;
        const vec3 l = pt_path(F, px, py, rng, primary);
        sum += l;
        sumSq += l * l;
        if (s == 0u) {
            first = primary;
        }
    }
    const float n = float(F.samplesPerFrame);
    vec4 acc = vec4(sum, n);
    vec4 accSq = vec4(sumSq, 0.0);
    if ((F.flags & PT_FLAG_ACCUMULATE) != 0u && F.sampleBase > 0u) {
        acc += PtVec4Ref(F.accum).v[pixel];
        accSq += PtVec4Ref(F.accumSq).v[pixel];
    }
    PtVec4Ref(F.accum).v[pixel] = acc;
    PtVec4Ref(F.accumSq).v[pixel] = accSq;
    PtVec4Ref(F.mean).v[pixel] = vec4(acc.xyz / acc.w, acc.w);
    if ((F.flags & PT_FLAG_GUIDES) != 0u) {
        // Demodulation factor: the primary albedo (floored), 1 for the sky and near-black surfaces (emitters), so
        // signal x albedo guide == the frame's radiance everywhere.
        const vec3 mean = sum / n;
        const vec3 a = first.hit && pt_luminance(first.albedo) >= 1e-2 ? max(first.albedo, vec3(1e-3)) : vec3(1.0);
        PtVec4Ref(F.signal).v[pixel] = vec4(mean / a, 0.0);
        PtFloatRef(F.depth).v[pixel] = first.hit ? dot(first.position - pt_load(F.cameraPosition), pt_load(F.cameraForward)) : 0.0;
        PtVec4Ref(F.normal).v[pixel] = vec4(first.normal, 0.0);
        PtVec4Ref(F.albedo).v[pixel] = vec4(a, first.hit ? 1.0 : 0.0);
    }
}

#endif
