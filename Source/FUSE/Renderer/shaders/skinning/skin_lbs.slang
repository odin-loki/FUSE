// GPU skinning in the GPU scene ("skin.lbs"). Slang twin of skin_lbs.comp (see there) and of
// fuse::animation::skinning_kernel::skin_vertex. Compile with -fp-mode precise (Slang does not honour `precise` per
// variable), so no multiply-add is contracted.
#include "../common/exact_math.slang"

static const uint kFuseSkinJobNormals = 1u;

struct SkinJob {
    uint64_t rest;
    uint64_t palette;
    uint64_t prevPalette;
    uint64_t out_;
    uint vertexCount;
    uint boneCount;
    uint flags;
    uint pad;
};

struct SkinPush {
    uint64_t jobs;
    uint jobCount;
    uint pad;
};

[[vk::push_constant]] ConstantBuffer<SkinPush> pc;

struct SkinResult {
    float3 position;
    float3 normal;
};

SkinResult fuseSkin(float* pal, uint boneCount, float3 rp, float3 rn, uint4 bones, float4 weights) {
    float px = 0.0;
    float py = 0.0;
    float pz = 0.0;
    float nx = 0.0;
    float ny = 0.0;
    float nz = 0.0;
    for (uint k = 0u; k < 4u; ++k) {
        const float weight = weights[k];
        if (weight <= 0.0) {
            continue;
        }
        const uint bone = bones[k];
        if (bone >= boneCount) {
            continue;
        }
        const uint o = bone * 16u;
        const float tx = pal[o + 0u] * rp.x + pal[o + 4u] * rp.y + pal[o + 8u] * rp.z + pal[o + 12u];
        const float ty = pal[o + 1u] * rp.x + pal[o + 5u] * rp.y + pal[o + 9u] * rp.z + pal[o + 13u];
        const float tz = pal[o + 2u] * rp.x + pal[o + 6u] * rp.y + pal[o + 10u] * rp.z + pal[o + 14u];
        const float ux = pal[o + 0u] * rn.x + pal[o + 4u] * rn.y + pal[o + 8u] * rn.z;
        const float uy = pal[o + 1u] * rn.x + pal[o + 5u] * rn.y + pal[o + 9u] * rn.z;
        const float uz = pal[o + 2u] * rn.x + pal[o + 6u] * rn.y + pal[o + 10u] * rn.z;
        px += tx * weight;
        py += ty * weight;
        pz += tz * weight;
        nx += ux * weight;
        ny += uy * weight;
        nz += uz * weight;
    }
    SkinResult r;
    r.position = float3(px, py, pz);
    r.normal = float3(nx, ny, nz);
    return r;
}

[numthreads(128, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint3 group: SV_GroupID) {
    const uint job = group.y;
    if (job >= pc.jobCount) {
        return;
    }
    const SkinJob j = ((SkinJob*)pc.jobs)[job];
    const uint v = tid.x;
    if (v >= j.vertexCount) {
        return;
    }
    const uint n = j.vertexCount;
    const bool hasNormals = (j.flags & kFuseSkinJobNormals) != 0u;
    float4* rest = (float4*)j.rest;
    const float4 rp = rest[v];
    const float3 rn = hasNormals ? rest[n + v].xyz : float3(0.0, 0.0, 0.0);
    uint4* w = (uint4*)(j.rest + uint64_t(n) * 32ull);
    const uint4 bones = w[v * 2u];
    const float4 weights = asfloat(w[v * 2u + 1u]);
    float4* outp = (float4*)j.out_;

    const SkinResult cur = fuseSkin((float*)j.palette, j.boneCount, rp.xyz, rn, bones, weights);
    outp[v] = float4(cur.position, rp.w);
    if (hasNormals) {
        float nx = cur.normal.x;
        float ny = cur.normal.y;
        float nz = cur.normal.z;
        const float len = fuseSqrtRn(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8) {
            nx = fuseDivRn(nx, len);
            ny = fuseDivRn(ny, len);
            nz = fuseDivRn(nz, len);
        }
        outp[2u * n + v] = float4(nx, ny, nz, 0.0);
    }
    const SkinResult prev = fuseSkin((float*)j.prevPalette, j.boneCount, rp.xyz, float3(0.0, 0.0, 0.0), bones, weights);
    outp[n + v] = float4(prev.position, rp.w);
}
