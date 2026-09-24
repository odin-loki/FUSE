// FUSE Relight RL-4.2: raster remaster lighting (deferred and forward): the RL-4.3 Relight BSDF (bsdf_core.h through
// shaders/material/bsdf.glsl) evaluated for the frame's lights read from the WP-1.1 GPU scene: the directional list,
// the pixel's cluster (renderer WP-2.1 cluster grid built on the CPU by its oracle; windowed inverse-square falloff and
// spot cone as clustered_gpu_kernel.hpp) and the fallback light; the T1 directional shadow map (3 x 3 PCF); D3D fog.
// Needs -I <Relight>/kernels -I <Relight>/shaders/material.
#ifndef FUSE_RELIGHT_RASTER_LIGHTING_GLSL
#define FUSE_RELIGHT_RASTER_LIGHTING_GLSL

#include "raster_common.glsl"

RasterFloatsRef g_rasterLut;
#define FUSE_BSDF_LUT_READ(i) g_rasterLut.v[(i)]
#include "bsdf.glsl"

float rasterPointFalloff(float d, float range) {
    if (range <= 0.0 || d >= range) {
        return 0.0;
    }
    const float ratio2 = (d * d) / (range * range);
    const float window = clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    return (window * window) / max(d * d, 1e-4);
}

float rasterSpotCone(float cosAngle, float cosInner, float cosOuter) {
    const float t = clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
    return t * t;
}

// Normal-offset lookup (1.5 shadow texels along the normal, shadowParams.z = world size of a texel) + a constant
// depth bias: no acne on surfaces grazing the light.
float rasterShadow(RasterFrame f, vec3 world, vec3 n) {
    if (f.shadowParams.w == 0.0 || (f.counts.w & RASTER_FEATURE_SHADOWS) == 0u) {
        return 1.0;
    }
    const vec4 clip = f.shadowMatrix * vec4(world + n * (1.5 * f.shadowParams.z), 1.0);
    const vec2 uv = clip.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || clip.z > 1.0) {
        return 1.0;
    }
    const ivec2 size = textureSize(sampler2D(fuse_textures_2d[fuse_handle_index(f.handles.y)],
                                             fuse_samplers[fuse_handle_index(f.handles.z)]), 0);
    const ivec2 texel = ivec2(uv * vec2(size));
    const float ref = clip.z - f.shadowParams.y;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const ivec2 t = clamp(texel + ivec2(x, y), ivec2(0), size - 1);
            const float depth = fuse_fetch_2d(f.handles.y, f.handles.z, t, 0).r;
            lit += ref <= depth ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

vec3 rasterToLocal(vec3 v, vec3 t, vec3 b, vec3 n) { return vec3(dot(v, t), dot(v, b), dot(v, n)); }

BsdfMaterial rasterBsdf(vec3 albedo, float roughness, float metallic) {
    BsdfMaterial m;
    m.model = kBsdfModelOpaque;
    m.flags = 0u;
    m.diffuseModel = kBsdfDiffuseLambert;
    m.opacity = 1.0;
    m.albedo = albedo;
    m.roughness = roughness;
    m.anisotropy = 0.0;
    m.metallic = metallic;
    m.ior = 1.5;
    m.thinFilmThickness = 0.0;
    m.transmittance = vec3(1.0);
    m.mediumDistance = 1.0;
    m.layerColor = vec3(0.0);
    m.layerOpacity = 0.0;
    m.sssTransmittance = vec3(1.0);
    m.sssMeasurementDistance = 0.0;
    m.sssSingleScatterAlbedo = vec3(0.0);
    m.sssAnisotropy = 0.0;
    m.sssRadius = vec3(0.0);
    m.hairBetaM = 0.0;
    m.hairBetaN = 0.0;
    m.hairAlpha = 0.0;
    m.hairH = 0.0;
    m.hairSigmaA = vec3(0.0);
    m.emission = vec3(0.0);
    return m;
}

// The cluster grid is the main camera's over the whole target: the pixel is the world position through the main
// camera's VIEW x PROJECTION (draws in a smaller D3D viewport land elsewhere on screen than their grid cell).
uint rasterCluster(RasterFrame f, vec2 fragPixel, vec3 world) {
    const uint tx = uint(f.cluster.x), ty = uint(f.cluster.y), tz = uint(f.cluster.z);
    vec2 pixel = fragPixel;
    const vec4 clip = f.viewProj * vec4(world, 1.0);
    if (clip.w > 1e-6) {
        pixel = vec2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) * f.screen.xy + 0.5;
    }
    const uint x = min(uint(pixel.x * f.screen.z * float(tx)), tx - 1u);
    const uint y = min(uint(pixel.y * f.screen.w * float(ty)), ty - 1u);
    uint z = 0u;
    const float zNear = f.forward.w, zFar = f.cluster.w;
    const float depth = dot(world - f.eye.xyz, f.forward.xyz);
    if (tz > 1u && zNear > 0.0 && zFar > zNear) {
        const float c = clamp(depth, zNear, zFar);
        z = min(uint(log(c / zNear) / log(zFar / zNear) * float(tz)), tz - 1u);
    }
    return (y * tx + x) * tz + z;
}

// Outgoing linear radiance of a lit surface.
vec3 rasterShade(RasterFrame f, vec3 albedoDisplay, float roughness, float metallic, vec3 n, vec3 world,
                 vec3 emissiveDisplay, vec2 pixel) {
    g_rasterLut = RasterFloatsRef(f.lut);
    const vec3 albedo = rasterToLinear(albedoDisplay);
    vec3 v = f.eye.xyz - world;
    v = dot(v, v) > 1e-12 ? normalize(v) : n;
    const vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 t = normalize(cross(up, n));
    const vec3 b = cross(n, t);
    const BsdfMaterial m = rasterBsdf(albedo, roughness, metallic);
    const vec3 wo = rasterToLocal(v, t, b, n);
    vec3 lo = rasterToLinear(emissiveDisplay) + f.ambient.rgb * albedo;
    if (wo.z <= 0.0) {
        return lo;
    }
    FuseGpuLightsRef lights = fuse_gpu_scene_lights(fuse_gpu_scene(f.handles.w));
    const float shadow = rasterShadow(f, world, n);
    const float exposure = f.ambient.w;
    RasterUintsRef directional = RasterUintsRef(f.directional);
    for (uint i = 0u; i < f.counts.x; ++i) {
        const uint slot = directional.v[i];
        const FuseGpuLight l = lights.v[slot];
        const vec3 wi = -normalize(vec3(l.direction[0], l.direction[1], l.direction[2]));
        const vec3 e =
            vec3(l.color[0], l.color[1], l.color[2]) * (l.intensity * exposure * (slot == f.counts.z ? shadow : 1.0));
        lo += bsdfEval(m, wo, rasterToLocal(wi, t, b, n)) * e;
    }
    if (f.fallbackDir.w > 0.0) {
        const vec3 wi = -f.fallbackDir.xyz;
        const vec3 e = f.fallbackColor.rgb * (exposure * (f.counts.z == 0xffffffffu ? shadow : 1.0));
        lo += bsdfEval(m, wo, rasterToLocal(wi, t, b, n)) * e;
    }
    if (f.counts.y > 0u) {
        const uint cluster = rasterCluster(f, pixel, world);
        RasterUintsRef grid = RasterUintsRef(f.grid);
        RasterUintsRef list = RasterUintsRef(f.lightList);
        const uint offset = grid.v[2u * cluster], count = grid.v[2u * cluster + 1u];
        for (uint k = 0u; k < count; ++k) {
            const FuseGpuLight l = lights.v[list.v[offset + k]];
            const vec3 d = vec3(l.position[0], l.position[1], l.position[2]) - world;
            const float dist = length(d);
            float atten = rasterPointFalloff(dist, l.range);
            if (atten == 0.0) {
                continue;
            }
            const vec3 wi = d / max(dist, 1e-6);
            if (l.type == FUSE_LIGHT_SPOT) {
                const vec3 axis = normalize(vec3(l.direction[0], l.direction[1], l.direction[2]));
                atten *= rasterSpotCone(dot(-wi, axis), l.cosInner, l.cosOuter);
            }
            lo += bsdfEval(m, wo, rasterToLocal(wi, t, b, n)) * vec3(l.color[0], l.color[1], l.color[2]) *
                  (l.intensity * atten * exposure);
        }
    }
    return lo;
}

// D3D fog in display space: factor from the view depth (linear / exp / exp2), then colour = mix(fog, colour, f).
vec3 rasterFog(RasterFrame f, vec3 display, vec3 world) {
    if (f.fogParams.w == 0.0 || (f.counts.w & RASTER_FEATURE_FOG) == 0u) {
        return display;
    }
    const float z = max(dot(world - f.eye.xyz, f.forward.xyz), 0.0);
    const uint mode = uint(f.fogColor.w);
    float factor = 1.0;
    if (mode == 3u) {
        factor = (f.fogParams.y - z) * f.fogParams.x;
    } else if (mode == 1u) {
        factor = exp(-f.fogParams.z * z);
    } else if (mode == 2u) {
        const float dz = f.fogParams.z * z;
        factor = exp(-dz * dz);
    }
    return mix(f.fogColor.rgb, display, clamp(factor, 0.0, 1.0));
}

#endif // FUSE_RELIGHT_RASTER_LIGHTING_GLSL
