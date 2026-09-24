// WP-6.1 DDGI: volume sampling (twin: ddgi_sample.slang). A line-by-line port of the CPU oracle
// ddgi_kernel::sample_irradiance / probe_irradiance / probe_distance / sample_tile / encode_direction
// (include/fuse/renderer/gi/ddgi_probe_kernel.hpp): the same operations in the same order, `precise`
// (no contraction), so the GPU and the CPU sample agree to rounding of sqrt / division.
//
// Self-contained (buffer references only): the DDGI kernels and the WP-2.1 light.shade include it.
//   vec3 E = fuse_ddgi_sample_irradiance(volumeAddress, position, normal);   // irradiance E, 0 when address == 0
// Layouts: include/fuse/renderer/gi/gpu/ddgi_gpu_types.hpp (DdgiVolumeView, atlas tiles).
#ifndef FUSE_DDGI_SAMPLE_GLSL
#define FUSE_DDGI_SAMPLE_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_DDGI_PI 3.14159265358979323846

struct FuseDdgiVolume { // DdgiVolumeView, 80 bytes
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
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseDdgiVolumeRef { FuseDdgiVolume v; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FuseDdgiFloatsRef { float v[]; };

// ddgi_kernel::is_empty_direction / resolve_direction / Vec3::normalized.
vec3 fuse_ddgi_normalized(vec3 d) {
    precise float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    const float len = sqrt(dd);
    if (len < 1e-8) {
        return vec3(0.0);
    }
    precise float inv = 1.0 / len;
    precise vec3 r = vec3(d.x * inv, d.y * inv, d.z * inv);
    return r;
}

vec3 fuse_ddgi_resolve_direction(vec3 d) {
    precise float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!(dd < 1e-8)) {
        return fuse_ddgi_normalized(d);
    }
    return vec3(0.0, 1.0, 0.0);
}

float fuse_ddgi_length(vec3 d) {
    precise float dd = d.x * d.x + d.y * d.y + d.z * d.z;
    return sqrt(dd);
}

// ddgi_kernel::encode_direction: octahedral UV in [0, 1]^2.
vec2 fuse_ddgi_encode_direction(vec3 direction) {
    vec3 n = fuse_ddgi_resolve_direction(direction);
    precise float sum = abs(n.x) + abs(n.y) + abs(n.z);
    if (sum > 1e-8) {
        precise float inv = 1.0 / sum;
        precise vec3 s = vec3(n.x * inv, n.y * inv, n.z * inv);
        n = s;
    }
    precise vec2 o;
    if (n.z >= 0.0) {
        o = vec2(n.x, n.y);
    } else {
        o.x = (1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        o.y = (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
    }
    precise vec2 uv = vec2(o.x * 0.5 + 0.5, o.y * 0.5 + 0.5);
    return uv;
}

// ddgi_kernel::sample_tile: bordered-tile bilinear fetch. `base` = first float of the tile, `comps` =
// floats per texel (3 irradiance, 2 distance); returns the first `comps` components.
vec3 fuse_ddgi_sample_tile(uint64_t atlas, uint base, uint comps, uint res, vec3 direction) {
    FuseDdgiFloatsRef t = FuseDdgiFloatsRef(atlas);
    const uint stride = res + 2u;
    const vec2 uv = fuse_ddgi_encode_direction(direction);
    precise float px = 1.0 + uv.x * float(res) - 0.5;
    precise float py = 1.0 + uv.y * float(res) - 0.5;
    const float fx = clamp(px, 0.0, float(res));
    const float fy = clamp(py, 0.0, float(res));
    const uint x0 = min(uint(fx), res);
    const uint y0 = min(uint(fy), res);
    const uint x1 = x0 + 1u;
    const uint y1 = y0 + 1u;
    precise float tx = fx - float(x0);
    precise float ty = fy - float(y0);
    precise vec3 result = vec3(0.0);
    for (uint c = 0u; c < comps; ++c) {
        const float t00 = t.v[base + (y0 * stride + x0) * comps + c];
        const float t10 = t.v[base + (y0 * stride + x1) * comps + c];
        const float t01 = t.v[base + (y1 * stride + x0) * comps + c];
        const float t11 = t.v[base + (y1 * stride + x1) * comps + c];
        precise float a = t00 * (1.0 - tx) + t10 * tx;
        precise float b = t01 * (1.0 - tx) + t11 * tx;
        precise float r = a * (1.0 - ty) + b * ty;
        result[c] = r;
    }
    return result;
}

// ddgi_kernel::probe_irradiance (E, texel * pi) / probe_distance (moments).
vec3 fuse_ddgi_probe_irradiance(FuseDdgiVolume V, uint probe, vec3 direction) {
    if (probe >= V.probeCount) {
        return vec3(0.0);
    }
    const vec3 dir = fuse_ddgi_resolve_direction(direction);
    const uint tile = (V.irradianceRes + 2u) * (V.irradianceRes + 2u);
    const vec3 s = fuse_ddgi_sample_tile(V.irradiance, probe * tile * 3u, 3u, V.irradianceRes, dir);
    precise vec3 e = vec3(s.x * float(FUSE_DDGI_PI), s.y * float(FUSE_DDGI_PI), s.z * float(FUSE_DDGI_PI));
    return e;
}

vec2 fuse_ddgi_probe_distance(FuseDdgiVolume V, uint probe, vec3 direction) {
    if (probe >= V.probeCount) {
        return vec2(0.0);
    }
    const vec3 dir = fuse_ddgi_resolve_direction(direction);
    const uint tile = (V.depthRes + 2u) * (V.depthRes + 2u);
    return fuse_ddgi_sample_tile(V.distance, probe * tile * 2u, 2u, V.depthRes, dir).xy;
}

vec3 fuse_ddgi_probe_position(FuseDdgiVolume V, uint probe) {
    const uint slice = V.dims[0] * V.dims[1];
    const uint z = probe / slice;
    const uint rem = probe % slice;
    const uint y = rem / V.dims[0];
    const uint x = rem % V.dims[0];
    precise vec3 p = vec3(V.origin[0] + V.spacing[0] * float(x), V.origin[1] + V.spacing[1] * float(y),
                          V.origin[2] + V.spacing[2] * float(z));
    return p;
}

// ddgi_kernel::sample_irradiance: trilinear over 8 probes with the backface (wrap) and Chebyshev
// visibility weights, weight crush; irradiance E.
vec3 fuse_ddgi_sample_volume(FuseDdgiVolume V, vec3 position, vec3 normal) {
    const vec3 n = fuse_ddgi_resolve_direction(normal);
    precise vec3 biased = vec3(position.x + n.x * V.normalBias, position.y + n.y * V.normalBias,
                               position.z + n.z * V.normalBias);
    precise vec3 grid = vec3(0.0);
    if (V.spacing[0] > 0.0 && V.spacing[1] > 0.0 && V.spacing[2] > 0.0) {
        grid = vec3((biased.x - V.origin[0]) / V.spacing[0], (biased.y - V.origin[1]) / V.spacing[1],
                    (biased.z - V.origin[2]) / V.spacing[2]);
    }
    uint base[3];
    float alpha[3];
    for (uint a = 0u; a < 3u; ++a) {
        const float g = grid[a];
        const float maxBase = float(V.dims[a] - 1u);
        const float fb = clamp(floor(g), 0.0, maxBase);
        base[a] = uint(fb);
        precise float al = g - fb;
        alpha[a] = clamp(al, 0.0, 1.0);
    }
    precise vec3 sum = vec3(0.0);
    precise float weightSum = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner) {
        precise float trilinear = 1.0;
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
        const vec3 probePos = fuse_ddgi_probe_position(V, probe);

        precise float weight = 1.0;
        precise vec3 toProbe = vec3(probePos.x - position.x, probePos.y - position.y, probePos.z - position.z);
        const float toProbeLen = fuse_ddgi_length(toProbe);
        if (toProbeLen > 1e-6) {
            precise float d = toProbe.x * n.x + toProbe.y * n.y + toProbe.z * n.z;
            precise float wrap = (d / toProbeLen + 1.0) * 0.5;
            weight *= wrap * wrap + 0.2;
        }

        precise vec3 probeToPoint = vec3(biased.x - probePos.x, biased.y - probePos.y, biased.z - probePos.z);
        const float dist = fuse_ddgi_length(probeToPoint);
        if (dist > 1e-6) {
            precise float inv = 1.0 / dist;
            precise vec3 dir = vec3(probeToPoint.x * inv, probeToPoint.y * inv, probeToPoint.z * inv);
            const vec2 moments = fuse_ddgi_probe_distance(V, probe, dir);
            const float mean = moments.x;
            if (dist > mean) {
                precise float variance = abs(moments.y - mean * mean);
                precise float delta = dist - mean;
                precise float chebyshev = variance / max(variance + delta * delta, 1e-12);
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

        const vec3 e = fuse_ddgi_probe_irradiance(V, probe, n);
        sum = vec3(sum.x + e.x * weight, sum.y + e.y * weight, sum.z + e.z * weight);
        weightSum += weight;
    }
    if (weightSum <= 0.0) {
        return vec3(0.0);
    }
    precise float invSum = 1.0 / weightSum;
    precise vec3 result = vec3(sum.x * invSum, sum.y * invSum, sum.z * invSum);
    return result;
}

// Irradiance E at a surface point of the volume at `volumeAddress` (a DdgiVolumeView); 0 without one.
vec3 fuse_ddgi_sample_irradiance(uint64_t volumeAddress, vec3 position, vec3 normal) {
    if (volumeAddress == 0ul) {
        return vec3(0.0);
    }
    const FuseDdgiVolume V = FuseDdgiVolumeRef(volumeAddress).v;
    if (V.probeCount == 0u || V.irradiance == 0ul || V.distance == 0ul) {
        return vec3(0.0);
    }
    return fuse_ddgi_sample_volume(V, position, normal);
}

#endif // FUSE_DDGI_SAMPLE_GLSL
