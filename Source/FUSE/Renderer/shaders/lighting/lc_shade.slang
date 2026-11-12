// WP-2.1 "light.shade". Slang twin of lc_shade.comp (see there, including the tiled light loop).
#include "lc_includes.slang"
#include "../ddgi/ddgi_sample.slang" // WP-6.1 DDGI indirect diffuse

[[vk::binding(1, 0)]] Texture2D<float4> fuse_lc_textures[];
[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_lc_images[];

float4 fuseLcFetch(uint handle, int2 p) { return fuse_lc_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

groupshared uint s_cluster;
groupshared uint s_box[6]; // order-preserving keys of min xyz, max xyz
groupshared uint s_scan[64];
groupshared uint s_slots[64];
groupshared uint s_vsm[64]; // fuseLcVsmCode of each kept entry

uint fuseLcKey(float f) {
    const uint k = asuint(f);
    return (k & 0x80000000u) != 0u ? ~k : (k | 0x80000000u);
}
float fuseLcUnkey(uint k) { return asfloat((k & 0x80000000u) != 0u ? (k & 0x7FFFFFFFu) : ~k); }

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint t: SV_GroupIndex) {
    const LcFrame F = fuseLcFrame();
    const uint2 p = tid.xy;
    const bool inside = p.x < F.width && p.y < F.height;
    const int2 ip = int2(p);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    bool shaded = false;
    uint cluster = 0u;
    LcSurface s = {};
    float3 v = float3(0.0, 0.0, 0.0);
    float3 radiance = float3(0.0, 0.0, 0.0);
    LcShadeState st = {};
    if (inside) {
        const float sx = (float(p.x) + 0.5) * F.invWidth;
        const float sy = (float(p.y) + 0.5) * F.invHeight;
        const float depth = fuseLcFetch(F.gbufferDepth, ip).x;
        const float viewDepth = fuseLcViewDepth(depth, F.nearPlane, F.farPlane, (F.flags & kFuseLcFlagReversedZ) != 0u);
        if (viewDepth > 0.0 && !isinf(viewDepth) && fuseLcMapCluster(sx, sy, viewDepth, F, cluster)) {
            shaded = true;
            const float4 rt0 = fuseLcFetch(F.gbufferNormalAo, ip);
            const float4 rt1 = fuseLcFetch(F.gbufferAlbedo, ip);
            const float4 rt2 = fuseLcFetch(F.gbufferRoughMetal, ip);
            const float4 rt5 = fuseLcFetch(F.gbufferEmissive, ip);
            s.position = fuseLcViewToWorld(fuseLcViewPosition(sx, sy, viewDepth, F), F);
            s.normal = fuseLcOctDecode(rt0.x, rt0.y);
            s.ao = rt0.w;
            s.albedo = rt1.xyz;
            s.roughness = rt2.x;
            s.metallic = rt2.y;
            s.emissive = rt5.xyz;
            v = fuseLcSafeNormalize(fuseLcVec3(F.cameraPosition) - s.position, s.normal);
            // The light loop (WP-2.2: shared with the forward transparency pass, lc_ltc.slang).
            radiance = fuseLcShadeBegin(F, s, v, p, st); // WP-6.2: RT visibility
        }
    }

    // The cluster lights, tiled (workgroup-uniform control flow).
    GpuLight* lights = fuseGpuSceneLights(fuseGpuScene(F.scene));
    uint* grid = (uint*)F.grid;
    uint* list = (uint*)F.lightList;
    const bool skipArea = (F.flags & kFuseLcFlagSkipAreaLights) != 0u;
    bool pending = shaded;
    for (;;) {
        if (t == 0u) {
            s_cluster = 0xFFFFFFFFu;
            s_box[0] = 0xFFFFFFFFu;
            s_box[1] = 0xFFFFFFFFu;
            s_box[2] = 0xFFFFFFFFu;
            s_box[3] = 0u;
            s_box[4] = 0u;
            s_box[5] = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (pending) {
            InterlockedMin(s_cluster, cluster);
        }
        GroupMemoryBarrierWithGroupSync();
        const uint current = s_cluster;
        if (current == 0xFFFFFFFFu) {
            break;
        }
        const bool mine = pending && cluster == current;
        if (mine) {
            InterlockedMin(s_box[0], fuseLcKey(s.position.x));
            InterlockedMin(s_box[1], fuseLcKey(s.position.y));
            InterlockedMin(s_box[2], fuseLcKey(s.position.z));
            InterlockedMax(s_box[3], fuseLcKey(s.position.x));
            InterlockedMax(s_box[4], fuseLcKey(s.position.y));
            InterlockedMax(s_box[5], fuseLcKey(s.position.z));
        }
        GroupMemoryBarrierWithGroupSync();
        const float3 boxMin = float3(fuseLcUnkey(s_box[0]), fuseLcUnkey(s_box[1]), fuseLcUnkey(s_box[2]));
        const float3 boxMax = float3(fuseLcUnkey(s_box[3]), fuseLcUnkey(s_box[4]), fuseLcUnkey(s_box[5]));
        const uint offset = grid[current * 2u];
        const uint count = grid[current * 2u + 1u];
        for (uint base = 0u; base < count; base += 64u) {
            uint slot = 0xFFFFFFFFu;
            uint flag = 0u;
            if (base + t < count) {
                slot = list[offset + base + t];
                if (slot < F.lightCount) {
                    const uint type = lights[slot].type;
                    if (type == kFuseLightPoint || type == kFuseLightSpot) {
                        const float range = lights[slot].range;
                        if (range > 0.0) {
                            const float3 c = float3(lights[slot].position[0], lights[slot].position[1], lights[slot].position[2]);
                            const float3 d = max(max(boxMin - c, c - boxMax), float3(0.0, 0.0, 0.0));
                            const float reach = range * 1.0001 + 1e-6;
                            flag = (d.x * d.x + d.y * d.y + d.z * d.z) <= reach * reach ? 1u : 0u;
                        }
                    } else {
                        flag = (skipArea && (type == kFuseLightRect || type == kFuseLightDisk)) ? 0u : 1u;
                    }
                }
            }
            s_scan[t] = flag;
            GroupMemoryBarrierWithGroupSync();
            for (uint step = 1u; step < 64u; step <<= 1u) {
                const uint add = t >= step ? s_scan[t - step] : 0u;
                GroupMemoryBarrierWithGroupSync();
                s_scan[t] += add;
                GroupMemoryBarrierWithGroupSync();
            }
            if (flag != 0u) {
                s_slots[s_scan[t] - 1u] = slot;
                s_vsm[s_scan[t] - 1u] = fuseLcVsmCode(uint64_t(F.shadowsLo) | (uint64_t(F.shadowsHi) << 32u), slot);
            }
            const uint kept = s_scan[63];
            GroupMemoryBarrierWithGroupSync();
            if (mine) {
                for (uint j = 0u; j < kept; ++j) {
                    radiance = fuseLcShadeClusterLight(F, st, s, v, s_slots[j], s_vsm[j], radiance);
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (mine) {
            pending = false;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (shaded) {
        // WP-6.1: DDGI irradiance E as indirect diffuse (Lambert: albedo x (1 - metallic) / pi), x AO.
        const uint64_t ddgi = uint64_t(F.ddgiLo) | (uint64_t(F.ddgiHi) << 32u);
        if (ddgi != 0ull) {
            // v (toward the camera) drives the volume's view bias (0 = normal bias only).
            const float3 e = fuseDdgiSampleIrradianceView(ddgi, s.position, s.normal, v);
            const float k = (1.0 - clamp(s.metallic, 0.0, 1.0)) * s.ao * (((DdgiVolume*)ddgi)[0].intensity * (1.0 / kFuseDdgiPi));
            radiance = float3(radiance.x + (s.albedo.x * e.x) * k, radiance.y + (s.albedo.y * e.y) * k,
                              radiance.z + (s.albedo.z * e.z) * k);
        }
        result = float4(radiance, 1.0);
    }
    if (inside) {
        fuse_lc_images[F.output_ & 0xFFFFFu][p] = result;
        if (pc.out_ != 0ull) {
            ((float4*)pc.out_)[p.y * F.width + p.x] = result;
        }
    }
}
