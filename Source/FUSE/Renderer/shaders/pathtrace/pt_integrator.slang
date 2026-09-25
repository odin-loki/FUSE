// WP-7.3 path-tracing mode: the integrator, Slang twin of pt_integrator.glsl (see there). The includer defines
//   bool ptTrace(PtFrame F, float3 o, float3 d, float tMin, float tMax, uint originInstance, uint originPrimitive,
//                out PtHit hit);
//   bool ptOccluded(PtFrame F, float3 o, float3 d, float tMax, uint originInstance, uint originPrimitive);
#pragma once

float3 ptUnproject(PtFrame F, float nx, float ny, float nz) {
    const float4 c = float4(nx, ny, nz, 1.0);
    const float x = F.invViewProj[0] * c.x + F.invViewProj[4] * c.y + F.invViewProj[8] * c.z + F.invViewProj[12];
    const float y = F.invViewProj[1] * c.x + F.invViewProj[5] * c.y + F.invViewProj[9] * c.z + F.invViewProj[13];
    const float z = F.invViewProj[2] * c.x + F.invViewProj[6] * c.y + F.invViewProj[10] * c.z + F.invViewProj[14];
    const float w = F.invViewProj[3] * c.x + F.invViewProj[7] * c.y + F.invViewProj[11] * c.z + F.invViewProj[15];
    return float3(x, y, z) / w;
}

struct PtPrimary {
    bool hit;
    float3 position;
    float3 normal;
    float3 albedo;
};

// Next-event estimate at a vertex (f cos Le / pdf x MIS weight, visibility tested); zero when nothing is sampled.
float3 ptNee(PtFrame F, PtHit h, float3 p, float3 n, float3 origin, float3 wo, float3 t, float3 b, PtMaterial m, inout uint rng) {
    const float u0 = ptRand(rng);
    const float u1 = ptRand(rng);
    const float u2 = ptRand(rng);
    const LtSampleResult s = ltSample(F.lightTree, p, n, u0, u1, u2);
    if (s.light == kLtInvalid || s.light >= F.lightCount || !(s.pmf > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const PtLight L = ((PtLight*)F.lights)[s.light];
    const LtEmitter em = ((LtEmitter*)((LtHeader*)F.lightTree)[0].emitters)[s.light];
    const float3 radiance = ptLoad(L.radiance);
    const float3 y = float3(s.position[0], s.position[1], s.position[2]);
    const float3 woL = float3(dot(wo, t), dot(wo, b), dot(wo, n));
    float3 wi;
    float3 le;
    float pdfLight;
    float tMax;
    bool area = false;
    if (s.kind == kLtKindDirectional) {
        wi = -y;
        le = radiance;
        pdfLight = s.pmf;
        tMax = F.farDistance;
    } else {
        const float3 d = y - p;
        const float dist2 = dot(d, d);
        if (!(dist2 > 1e-12)) {
            return float3(0.0, 0.0, 0.0);
        }
        wi = d / sqrt(dist2);
        if (s.kind == kLtKindTriangle || s.kind == kLtKindRect || s.kind == kLtKindDisk) {
            float cosL = -dot(ptLoad(em.normal), wi);
            if ((em.flags & kLtFlagTwoSided) != 0u) {
                cosL = abs(cosL);
            }
            if (!(cosL > 0.0) || !(s.pdfArea > 0.0)) {
                return float3(0.0, 0.0, 0.0);
            }
            le = radiance;
            pdfLight = s.pmf * s.pdfArea * dist2 / cosL;
            area = true;
        } else {
            float spot = 1.0;
            if (s.kind == kLtKindSpot) {
                const float cosA = -dot(ptLoad(em.normal), wi);
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
        const float3 toLight = y - origin;
        tMax = length(toLight) * kPtShadowShorten;
    }
    const float cosX = dot(n, wi);
    if (!(cosX > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 wiL = float3(dot(wi, t), dot(wi, b), cosX);
    const float3 f = ptBsdfEval(m, F.minRoughness, woL, wiL);
    if (!(ptMax3(f) > 0.0) || !(ptMax3(le) > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    float w = 1.0;
    if (area && (F.flags & kPtFlagEmitterHits) != 0u) {
        w = ptPowerHeuristic(pdfLight, ptBsdfPdf(m, F.minRoughness, woL, wiL));
    }
    float3 dir = wi;
    if (s.kind != kLtKindDirectional) {
        dir = normalize(y - origin);
    }
    if (ptOccluded(F, origin, dir, tMax, h.instance, h.primitive)) {
        return float3(0.0, 0.0, 0.0);
    }
    return f * le * (cosX * w / pdfLight);
}

// One path through pixel (px, py). Returns the radiance; `primary` receives the camera ray's surface.
float3 ptPath(PtFrame F, uint px, uint py, inout uint rng, out PtPrimary primary) {
    primary.hit = false;
    primary.position = float3(0.0, 0.0, 0.0);
    primary.normal = float3(0.0, 0.0, 0.0);
    primary.albedo = float3(0.0, 0.0, 0.0);
    const float jx = ptRand(rng);
    const float jy = ptRand(rng);
    const float nx = (float(px) + jx) * 2.0 / float(F.width) - 1.0;
    const float ny = (float(py) + jy) * 2.0 / float(F.height) - 1.0;
    const float3 camera = ptLoad(F.cameraPosition);
    float3 o = camera;
    float3 d = normalize(ptUnproject(F, nx, ny, 0.5) - camera);
    float3 throughput = float3(1.0, 1.0, 1.0);
    float3 radiance = float3(0.0, 0.0, 0.0);
    float3 prevP = float3(0.0, 0.0, 0.0);
    float3 prevN = float3(0.0, 0.0, 0.0);
    float prevPdf = 0.0;
    uint originInstance = kPtInvalid;
    uint originPrimitive = kPtInvalid;
    for (uint depth = 0u; depth <= F.maxBounces; ++depth) {
        PtHit h;
        if (!ptTrace(F, o, d, F.rayTMin, F.farDistance, originInstance, originPrimitive, h)) {
            radiance += throughput * ptLoad(F.sky);
            break;
        }
        const float3 p = ptLoad(h.position);
        const float3 ng = ptLoad(h.normal);
        const bool front = dot(ng, d) < 0.0;
        const float3 n = front ? ng : -ng;
        if (depth == 0u) {
            primary.hit = true;
            primary.position = p;
            primary.normal = n;
            primary.albedo = ptLoad(h.albedo);
        }
        // Emission.
        if (h.emitter != kPtInvalid && h.emitter < F.lightCount) {
            const LtEmitter em = ((LtEmitter*)((LtHeader*)F.lightTree)[0].emitters)[h.emitter];
            float cosL = -dot(ptLoad(em.normal), d);
            if ((em.flags & kLtFlagTwoSided) != 0u) {
                cosL = abs(cosL);
            }
            if (cosL > 0.0) {
                float w = 1.0;
                if (depth > 0u) {
                    if ((F.flags & kPtFlagEmitterHits) == 0u) {
                        w = 0.0;
                    } else if ((F.flags & kPtFlagNee) != 0u && em.area > 0.0) {
                        const float3 dl = p - prevP;
                        const float pdfLight = ltPmf(F.lightTree, prevP, prevN, h.emitter) * (dot(dl, dl) / (cosL * em.area));
                        w = ptPowerHeuristic(prevPdf, pdfLight);
                    }
                }
                radiance += throughput * ptLoad(((PtLight*)F.lights)[h.emitter].radiance) * w;
            }
        } else if (front) {
            radiance += throughput * ptLoad(h.emission);
        }
        if (depth == F.maxBounces) {
            break;
        }
        const float3 wo = -d;
        float3 t;
        float3 b;
        ptBasis(n, t, b);
        PtMaterial m;
        m.albedo = ptLoad(h.albedo);
        m.metallic = h.metallic;
        m.roughness = h.roughness;
        const float3 origin = p + n * (F.normalBias + F.viewBias * length(p - camera));
        if ((F.flags & kPtFlagNee) != 0u) {
            radiance += throughput * ptNee(F, h, p, n, origin, wo, t, b, m, rng);
        } else {
            ptRand(rng);
            ptRand(rng);
            ptRand(rng);
        }
        const float3 woL = float3(dot(wo, t), dot(wo, b), dot(wo, n));
        const float uLobe = ptRand(rng);
        const float u1 = ptRand(rng);
        const float u2 = ptRand(rng);
        float3 wiL;
        float pdf;
        float3 weight;
        if (!ptBsdfSample(m, F.minRoughness, woL, uLobe, u1, u2, wiL, pdf, weight)) {
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
        if ((F.flags & kPtFlagRussianRoulette) != 0u && bounces >= F.rrStartBounce) {
            const float q = min(ptMax3(throughput), 0.95);
            if (!(ptRand(rng) < q)) {
                break;
            }
            throughput /= q;
        }
    }
    if ((F.flags & kPtFlagClamp) != 0u) {
        const float l = ptLuminance(radiance);
        if (l > F.clampRadiance) {
            radiance *= F.clampRadiance / l;
        }
    }
    return radiance;
}

// samplesPerFrame paths of pixel (px, py), accumulated; writes the output mean and the guides.
void ptRenderPixel(PtFrame F, uint px, uint py) {
    const uint pixel = py * F.width + px;
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 sumSq = float3(0.0, 0.0, 0.0);
    PtPrimary first;
    first.hit = false;
    first.position = float3(0.0, 0.0, 0.0);
    first.normal = float3(0.0, 0.0, 0.0);
    first.albedo = float3(0.0, 0.0, 0.0);
    for (uint s = 0u; s < F.samplesPerFrame; ++s) {
        uint rng = ptSeed(pixel, F.sampleBase + s, F.seed);
        PtPrimary primary;
        const float3 l = ptPath(F, px, py, rng, primary);
        sum += l;
        sumSq += l * l;
        if (s == 0u) {
            first = primary;
        }
    }
    const float n = float(F.samplesPerFrame);
    float4 acc = float4(sum, n);
    float4 accSq = float4(sumSq, 0.0);
    if ((F.flags & kPtFlagAccumulate) != 0u && F.sampleBase > 0u) {
        acc += ((float4*)F.accum)[pixel];
        accSq += ((float4*)F.accumSq)[pixel];
    }
    ((float4*)F.accum)[pixel] = acc;
    ((float4*)F.accumSq)[pixel] = accSq;
    ((float4*)F.mean)[pixel] = float4(acc.xyz / acc.w, acc.w);
    if ((F.flags & kPtFlagGuides) != 0u) {
        // Demodulation factor: the primary albedo (floored), 1 for the sky and near-black surfaces (emitters), so
        // signal x albedo guide == the frame's radiance everywhere.
        const float3 mean = sum / n;
        const float3 a = first.hit && ptLuminance(first.albedo) >= 1e-2 ? max(first.albedo, float3(1e-3, 1e-3, 1e-3)) : float3(1.0, 1.0, 1.0);
        ((float4*)F.signal)[pixel] = float4(mean / a, 0.0);
        ((float*)F.depth)[pixel] = first.hit ? dot(first.position - ptLoad(F.cameraPosition), ptLoad(F.cameraForward)) : 0.0;
        ((float4*)F.normal)[pixel] = float4(first.normal, 0.0);
        ((float4*)F.albedo)[pixel] = float4(a, first.hit ? 1.0 : 0.0);
    }
}

