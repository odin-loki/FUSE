// WP-1.3 single-pass Hi-Z build (SPD-style), Slang twin of hiz_build.comp (see the GLSL file for
// the algorithm). Same push layout, same pyramid (bit-exact: only max()).
#include "gpu_scene.slang"
#include "cull_common.slang"

[[vk::binding(0, 0)]] [[vk::image_format("r32f")]] globallycoherent RWTexture2D<float> fuse_hiz_mips[];
[[vk::binding(3, 0)]] globallycoherent RWStructuredBuffer<uint> fuse_hiz_words[];

struct Push {
    uint64_t constants;
    uint depthTexture;
    uint depthSampler;
    uint depthWidth;
    uint depthHeight;
    uint groupsX;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

groupshared float s_tile[256];
groupshared uint s_last;

uint levelDim(CullConstants* C, uint level) { return max(C.hizDim >> level, 1u); }

float depthAt(uint x, uint y) {
    if (x >= pc.depthWidth || y >= pc.depthHeight) {
        return 1.0;
    }
    return fuse_textures_2d[fuseHandleIndex(pc.depthTexture)].Load(int3(int(x), int(y), 0));
}

float mipAt(CullConstants* C, uint level, uint x, uint y) {
    const uint d = levelDim(C, level);
    if (x >= d || y >= d) {
        return 0.0;
    }
    return fuse_hiz_mips[fuseHandleIndex(C.hizMips[level])][uint2(x, y)];
}

float baseValue(CullConstants* C, uint base, uint2 p) {
    if (base == 0u) {
        return fuseCullMax4(depthAt(2u * p.x, 2u * p.y), depthAt(2u * p.x + 1u, 2u * p.y), depthAt(2u * p.x, 2u * p.y + 1u),
                            depthAt(2u * p.x + 1u, 2u * p.y + 1u));
    }
    return fuseCullMax4(mipAt(C, base - 1u, 2u * p.x, 2u * p.y), mipAt(C, base - 1u, 2u * p.x + 1u, 2u * p.y),
                        mipAt(C, base - 1u, 2u * p.x, 2u * p.y + 1u), mipAt(C, base - 1u, 2u * p.x + 1u, 2u * p.y + 1u));
}

void storeLevel(CullConstants* C, uint level, uint2 p, float v) {
    if (level < C.hizMipCount) {
        const uint d = levelDim(C, level);
        if (p.x < d && p.y < d) {
            fuse_hiz_mips[fuseHandleIndex(C.hizMips[level])][p] = v;
        }
    }
}

void reduceTile(CullConstants* C, uint base, uint2 group, uint2 t) {
    float v0[16];
    const uint2 o0 = group * 64u + t * 4u;
    for (uint j = 0u; j < 4u; ++j) {
        for (uint i = 0u; i < 4u; ++i) {
            const float v = baseValue(C, base, o0 + uint2(i, j));
            v0[j * 4u + i] = v;
            storeLevel(C, base, o0 + uint2(i, j), v);
        }
    }
    float v1[4];
    const uint2 o1 = group * 32u + t * 2u;
    for (uint j = 0u; j < 2u; ++j) {
        for (uint i = 0u; i < 2u; ++i) {
            const uint k = (2u * j) * 4u + 2u * i;
            const float v = fuseCullMax4(v0[k], v0[k + 1u], v0[k + 4u], v0[k + 5u]);
            v1[j * 2u + i] = v;
            storeLevel(C, base + 1u, o1 + uint2(i, j), v);
        }
    }
    const float v2 = fuseCullMax4(v1[0], v1[1], v1[2], v1[3]);
    storeLevel(C, base + 2u, group * 16u + t, v2);
    s_tile[t.y * 16u + t.x] = v2;
    GroupMemoryBarrierWithGroupSync();
    for (uint l = 3u; l <= 6u; ++l) {
        const uint n = 16u >> (l - 2u);
        const bool inTile = t.x < n && t.y < n;
        float v = 0.0;
        if (inTile) {
            const uint k = (2u * t.y) * 16u + 2u * t.x;
            v = fuseCullMax4(s_tile[k], s_tile[k + 1u], s_tile[k + 16u], s_tile[k + 17u]);
        }
        GroupMemoryBarrierWithGroupSync();
        if (inTile) {
            s_tile[t.y * 16u + t.x] = v;
            storeLevel(C, base + l, group * n + t, v);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(16, 16, 1)]
void main(uint3 gid: SV_GroupID, uint3 lid: SV_GroupThreadID, uint lindex: SV_GroupIndex) {
    CullConstants* C = (CullConstants*)pc.constants;
    reduceTile(C, 0u, gid.xy, lid.xy);
    if (C.hizMipCount <= 7u) {
        return;
    }
    AllMemoryBarrierWithGroupSync();
    if (lindex == 0u) {
        const uint counter = fuseHandleIndex(C.countsBuffer);
        uint done;
        InterlockedAdd(fuse_hiz_words[counter][kFuseCullCountHizCounter], 1u, done);
        const bool last = done == pc.groupsX * pc.groupsX - 1u;
        if (last) {
            uint previous;
            InterlockedExchange(fuse_hiz_words[counter][kFuseCullCountHizCounter], 0u, previous);
        }
        s_last = last ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (s_last == 0u) {
        return;
    }
    DeviceMemoryBarrier();
    reduceTile(C, 7u, uint2(0u, 0u), lid.xy);
}
