// WP-6.1 DDGI: volume sampling, Slang twin of ddgi_sample.glsl (see there). Compile with -fp-mode precise.
//   float3 E = fuseDdgiSampleIrradiance(volumeAddress, position, normal);   // 0 when address == 0
//   float3 E = fuseDdgiSampleIrradianceView(volumeAddress, position, normal, view); // + view bias
#pragma once

static const float kFuseDdgiPi = 3.14159265358979323846;

struct DdgiVolume { // DdgiVolumeView, 96 bytes
    float origin[3];
    uint probeCount;
    float spacing[3];
    uint irradianceRes;
    uint dims[3];
    uint depthRes;
    float normalBias;
    float weightCrushThreshold;
    float intensity;
    uint flags;
    uint64_t irradiance;
    uint64_t distance;
    uint64_t probeData;
    float viewBias;
    uint pad;
};

float3 fuseDdgiNormalized(float3 d) {
    const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    const float len = sqrt(dd);
    if (len < 1e-8) {
        return float3(0.0, 0.0, 0.0);
    }
    const float inv = 1.0 / len;
    return float3(d.x * inv, d.y * inv, d.z * inv);
}

float3 fuseDdgiResolveDirection(float3 d) {
    const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!(dd < 1e-8)) {
        return fuseDdgiNormalized(d);
    }
    return float3(0.0, 1.0, 0.0);
}

float3 fuseDdgiResolveDirectionOr(float3 d, float3 fallback) {
    const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!(dd < 1e-8)) {
        return fuseDdgiNormalized(d);
    }
    return fuseDdgiResolveDirection(fallback);
}

float fuseDdgiLength(float3 d) {
    const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    return sqrt(dd);
}

float2 fuseDdgiEncodeDirection(float3 direction) {
    float3 n = fuseDdgiResolveDirection(direction);
    const float sum = abs(n.x) + abs(n.y) + abs(n.z);
    if (sum > 1e-8) {
        const float inv = 1.0 / sum;
        n = float3(n.x * inv, n.y * inv, n.z * inv);
    }
    float2 o;
    if (n.z >= 0.0) {
        o = float2(n.x, n.y);
    } else {
        o.x = (1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        o.y = (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
    }
    return float2(o.x * 0.5 + 0.5, o.y * 0.5 + 0.5);
}

float3 fuseDdgiSampleTile(uint64_t atlas, uint base, uint comps, uint res, float3 direction) {
    float* t = (float*)atlas;
    const uint stride = res + 2u;
    const float2 uv = fuseDdgiEncodeDirection(direction);
    const float px = 1.0 + uv.x * float(res) - 0.5;
    const float py = 1.0 + uv.y * float(res) - 0.5;
    const float fx = clamp(px, 0.0, float(res));
    const float fy = clamp(py, 0.0, float(res));
    const uint x0 = min(uint(fx), res);
    const uint y0 = min(uint(fy), res);
    const uint x1 = x0 + 1u;
    const uint y1 = y0 + 1u;
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);
    float3 result = float3(0.0, 0.0, 0.0);
    for (uint c = 0u; c < comps; ++c) {
        const float t00 = t[base + (y0 * stride + x0) * comps + c];
        const float t10 = t[base + (y0 * stride + x1) * comps + c];
        const float t01 = t[base + (y1 * stride + x0) * comps + c];
        const float t11 = t[base + (y1 * stride + x1) * comps + c];
        const float a = t00 * (1.0 - tx) + t10 * tx;
        const float b = t01 * (1.0 - tx) + t11 * tx;
        result[c] = a * (1.0 - ty) + b * ty;
    }
    return result;
}

float3 fuseDdgiProbeIrradiance(DdgiVolume V, uint probe, float3 direction) {
    if (probe >= V.probeCount) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 dir = fuseDdgiResolveDirection(direction);
    const uint tile = (V.irradianceRes + 2u) * (V.irradianceRes + 2u);
    const float3 s = fuseDdgiSampleTile(V.irradiance, probe * tile * 3u, 3u, V.irradianceRes, dir);
    return float3(s.x * kFuseDdgiPi, s.y * kFuseDdgiPi, s.z * kFuseDdgiPi);
}

float2 fuseDdgiProbeDistance(DdgiVolume V, uint probe, float3 direction) {
    if (probe >= V.probeCount) {
        return float2(0.0, 0.0);
    }
    const float3 dir = fuseDdgiResolveDirection(direction);
    const uint tile = (V.depthRes + 2u) * (V.depthRes + 2u);
    return fuseDdgiSampleTile(V.distance, probe * tile * 2u, 2u, V.depthRes, dir).xy;
}

float3 fuseDdgiProbePosition(DdgiVolume V, uint probe) {
    const uint slice = V.dims[0] * V.dims[1];
    const uint z = probe / slice;
    const uint rem = probe % slice;
    const uint y = rem / V.dims[0];
    const uint x = rem % V.dims[0];
    return float3(V.origin[0] + V.spacing[0] * float(x), V.origin[1] + V.spacing[1] * float(y),
                  V.origin[2] + V.spacing[2] * float(z));
}

float3 fuseDdgiProbePositionRelocated(DdgiVolume V, uint probe) {
    const float3 p = fuseDdgiProbePosition(V, probe);
    if (V.probeData == 0 || probe >= V.probeCount) {
        return p;
    }
    const float4 d = ((float4*)V.probeData)[probe];
    return float3(p.x + d.x, p.y + d.y, p.z + d.z);
}

bool fuseDdgiProbeActive(DdgiVolume V, uint probe) {
    return V.probeData == 0 || probe >= V.probeCount || ((float4*)V.probeData)[probe].w == 0.0;
}

float3 fuseDdgiSampleVolumeBiased(DdgiVolume V, float3 position, float3 n, float3 biased) {
    float3 grid = float3(0.0, 0.0, 0.0);
    if (V.spacing[0] > 0.0 && V.spacing[1] > 0.0 && V.spacing[2] > 0.0) {
        grid = float3((biased.x - V.origin[0]) / V.spacing[0], (biased.y - V.origin[1]) / V.spacing[1],
                      (biased.z - V.origin[2]) / V.spacing[2]);
    }
    uint base[3];
    float alpha[3];
    for (uint a = 0u; a < 3u; ++a) {
        const float g = grid[a];
        const float maxBase = float(V.dims[a] - 1u);
        const float fb = clamp(floor(g), 0.0, maxBase);
        base[a] = uint(fb);
        alpha[a] = clamp(g - fb, 0.0, 1.0);
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        float trilinear = 1.0;
        uint c[3];
        for (uint a = 0u; a < 3u; ++a) {
            const uint bit = (corner >> a) & 1u;
            c[a] = min(base[a] + bit, V.dims[a] - 1u);
            trilinear *= bit != 0u ? alpha[a] : (1.0 - alpha[a]);
        }
        if (trilinear <= 0.0) {
            continue;
        }
        const uint probe = c[2] * V.dims[0] * V.dims[1] + c[1] * V.dims[0] + c[0];
        if (!fuseDdgiProbeActive(V, probe)) {
            continue;
        }
        const float3 probePos = fuseDdgiProbePositionRelocated(V, probe);

        float weight = 1.0;
        const float3 toProbe = float3(probePos.x - position.x, probePos.y - position.y, probePos.z - position.z);
        const float toProbeLen = fuseDdgiLength(toProbe);
        if (toProbeLen > 1e-6) {
            const float d = toProbe.x * n.x + toProbe.y * n.y + toProbe.z * n.z;
            const float wrap = (d / toProbeLen + 1.0) * 0.5;
            weight *= wrap * wrap + 0.2;
        }

        const float3 probeToPoint = float3(biased.x - probePos.x, biased.y - probePos.y, biased.z - probePos.z);
        const float dist = fuseDdgiLength(probeToPoint);
        if (dist > 1e-6) {
            const float inv = 1.0 / dist;
            const float3 dir = float3(probeToPoint.x * inv, probeToPoint.y * inv, probeToPoint.z * inv);
            const float2 moments = fuseDdgiProbeDistance(V, probe, dir);
            const float mean = moments.x;
            if (dist > mean) {
                const float variance = abs(moments.y - mean * mean);
                const float delta = dist - mean;
                float chebyshev = variance / max(variance + delta * delta, 1e-12);
                chebyshev = max(chebyshev * chebyshev * chebyshev, 0.0);
                weight *= max(chebyshev, 0.05);
            }
        }
        weight = max(weight, 1e-6);
        const float crush = V.weightCrushThreshold;
        if (crush > 0.0 && weight < crush) {
            weight *= (weight * weight) / (crush * crush);
        }
        weight *= trilinear;

        const float3 e = fuseDdgiProbeIrradiance(V, probe, n);
        sum = float3(sum.x + e.x * weight, sum.y + e.y * weight, sum.z + e.z * weight);
        weightSum += weight;
    }
    if (weightSum <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float invSum = 1.0 / weightSum;
    return float3(sum.x * invSum, sum.y * invSum, sum.z * invSum);
}

float3 fuseDdgiSampleVolume(DdgiVolume V, float3 position, float3 normal) {
    const float3 n = fuseDdgiResolveDirection(normal);
    const float3 biased = float3(position.x + n.x * V.normalBias, position.y + n.y * V.normalBias,
                                 position.z + n.z * V.normalBias);
    return fuseDdgiSampleVolumeBiased(V, position, n, biased);
}

float3 fuseDdgiSampleVolumeView(DdgiVolume V, float3 position, float3 normal, float3 view) {
    const float3 n = fuseDdgiResolveDirection(normal);
    if (V.viewBias == 0.0) {
        const float3 biased = float3(position.x + n.x * V.normalBias, position.y + n.y * V.normalBias,
                                     position.z + n.z * V.normalBias);
        return fuseDdgiSampleVolumeBiased(V, position, n, biased);
    }
    const float3 w = fuseDdgiResolveDirectionOr(view, n);
    const float3 biased = float3((position.x + n.x * V.normalBias) + w.x * V.viewBias,
                                 (position.y + n.y * V.normalBias) + w.y * V.viewBias,
                                 (position.z + n.z * V.normalBias) + w.z * V.viewBias);
    return fuseDdgiSampleVolumeBiased(V, position, n, biased);
}

float3 fuseDdgiSampleIrradiance(uint64_t volumeAddress, float3 position, float3 normal) {
    if (volumeAddress == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const DdgiVolume V = ((DdgiVolume*)volumeAddress)[0];
    if (V.probeCount == 0u || V.irradiance == 0 || V.distance == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    return fuseDdgiSampleVolume(V, position, normal);
}

float3 fuseDdgiSampleIrradianceView(uint64_t volumeAddress, float3 position, float3 normal, float3 view) {
    if (volumeAddress == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const DdgiVolume V = ((DdgiVolume*)volumeAddress)[0];
    if (V.probeCount == 0u || V.irradiance == 0 || V.distance == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    return fuseDdgiSampleVolumeView(V, position, normal, view);
}
