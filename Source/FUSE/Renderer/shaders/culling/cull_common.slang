// FUSE instance culling (WP-1.3): Slang side of include/fuse/renderer/culling/cull_types.hpp and
// twin of cull_common.glsl / instance_cull_kernel.hpp (same operations in the same order, `precise`
// so nothing is contracted into an FMA).
//
// #include after gpu_scene.slang. Declares the bindless sampled-image (binding 1) and sampler
// (binding 2) arrays; storage buffers / images are declared by each kernel (their qualifiers differ).
#pragma once

[[vk::binding(1, 0)]] Texture2D<float> fuse_textures_2d[];

static const uint kFuseCullFrustum = 1u;
static const uint kFuseCullOcclusion = 2u;
static const uint kFuseCullHistoryValid = 4u;

static const uint kFuseCullResultNone = 0u;
static const uint kFuseCullResultFrustumCulled = 1u;
static const uint kFuseCullResultPhase1Drawn = 2u;
static const uint kFuseCullResultCandidate = 3u;
static const uint kFuseCullResultPhase2Drawn = 4u;
static const uint kFuseCullResultOccluded = 5u;

static const uint kFuseCullCountPhase1Draws = 0u;
static const uint kFuseCullCountPhase2Draws = 1u;
static const uint kFuseCullCountCandidates = 2u;
static const uint kFuseCullCountHizCounter = 3u;
static const uint kFuseCullCountDispatchX = 4u;

static const float kFuseCullMinW = 1.0e-6;

struct CullConstants {
    float viewProj[16];
    float prevViewProj[16];
    float4 planes[6];
    uint hizMips[16];
    uint instanceCount;
    uint flags;
    uint hizDim;
    uint hizMipCount;
    float hizScale[2];
    uint maxDraws;
    uint phase2DrawBase;
    uint argsBuffer;
    uint countsBuffer;
    uint candidatesBuffer;
    uint resultsBuffer;
    uint hizTexture;
    uint pointSampler;
    uint reserved[2];
};

uint fuseHandleIndex(uint handle) { return handle & 0xFFFFFu; }

struct CullSphere {
    float c[3];
    float r;
};

CullSphere fuseCullWorldSphere(GpuTransform t, GpuMesh m, float radiusScale) {
    CullSphere s;
    for (uint r = 0u; r < 3u; ++r) {
        precise float c = t.rows[r].x * m.boundsCenter[0] + t.rows[r].y * m.boundsCenter[1] +
                          t.rows[r].z * m.boundsCenter[2] + t.rows[r].w;
        s.c[r] = c;
    }
    precise float g00 = t.rows[0].x * t.rows[0].x + t.rows[1].x * t.rows[1].x + t.rows[2].x * t.rows[2].x;
    precise float g11 = t.rows[0].y * t.rows[0].y + t.rows[1].y * t.rows[1].y + t.rows[2].y * t.rows[2].y;
    precise float g22 = t.rows[0].z * t.rows[0].z + t.rows[1].z * t.rows[1].z + t.rows[2].z * t.rows[2].z;
    precise float d01 = t.rows[0].x * t.rows[0].y + t.rows[1].x * t.rows[1].y + t.rows[2].x * t.rows[2].y;
    precise float d02 = t.rows[0].x * t.rows[0].z + t.rows[1].x * t.rows[1].z + t.rows[2].x * t.rows[2].z;
    precise float d12 = t.rows[0].y * t.rows[0].z + t.rows[1].y * t.rows[1].z + t.rows[2].y * t.rows[2].z;
    const float g01 = d01 < 0.0 ? -d01 : d01;
    const float g02 = d02 < 0.0 ? -d02 : d02;
    const float g12 = d12 < 0.0 ? -d12 : d12;
    precise float b0 = g00 + g01 + g02;
    precise float b1 = g11 + g01 + g12;
    precise float b2 = g22 + g02 + g12;
    const float b12 = b1 > b2 ? b1 : b2;
    const float s2 = b0 > b12 ? b0 : b12;
    precise float r = m.boundsRadius * sqrt(s2) * radiusScale;
    s.r = r;
    return s;
}

bool fuseCullFrustumVisible(CullConstants* C, CullSphere s) {
    for (uint i = 0u; i < 6u; ++i) {
        const float4 p = C.planes[i];
        precise float d = p.x * s.c[0] + p.y * s.c[1] + p.z * s.c[2] + p.w;
        if (d < -s.r) {
            return false;
        }
    }
    return true;
}

float fuseCullClamp01(float v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

float fuseCullMax4(float a, float b, float c, float d) {
    const float ab = a > b ? a : b;
    const float cd = c > d ? c : d;
    return ab > cd ? ab : cd;
}

float fuseCullHizFetch(CullConstants* C, uint mip, uint x, uint y) {
    return fuse_textures_2d[fuseHandleIndex(C.hizTexture)].Load(int3(int(x), int(y), int(mip)));
}

// which: 0 = this frame's viewProj, 1 = last frame's (prevViewProj). True = possibly visible.
bool fuseCullHizVisible(CullConstants* C, uint which, CullSphere s) {
    float vp[16];
    for (uint i = 0u; i < 16u; ++i) {
        vp[i] = which == 0u ? C.viewProj[i] : C.prevViewProj[i];
    }
    float minX = 1.0, maxX = -1.0, minY = 1.0, maxY = -1.0, minZ = 1.0;
    for (uint k = 0u; k < 8u; ++k) {
        precise float x = (k & 1u) != 0u ? s.c[0] + s.r : s.c[0] - s.r;
        precise float y = (k & 2u) != 0u ? s.c[1] + s.r : s.c[1] - s.r;
        precise float z = (k & 4u) != 0u ? s.c[2] + s.r : s.c[2] - s.r;
        precise float cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
        if (!(cw > kFuseCullMinW)) {
            return true;
        }
        precise float cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
        precise float cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
        precise float cz = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
        precise float nx = cx / cw;
        precise float ny = cy / cw;
        precise float nz = cz / cw;
        if (k == 0u) {
            minX = nx;
            maxX = nx;
            minY = ny;
            maxY = ny;
            minZ = nz;
        } else {
            minX = nx < minX ? nx : minX;
            maxX = nx > maxX ? nx : maxX;
            minY = ny < minY ? ny : minY;
            maxY = ny > maxY ? ny : maxY;
            minZ = nz < minZ ? nz : minZ;
        }
    }
    if (minZ < 0.0) {
        return true;
    }
    const uint last = C.hizDim - 1u;
    precise float ux0 = minX * 0.5 + 0.5;
    precise float ux1 = maxX * 0.5 + 0.5;
    precise float uy0 = minY * 0.5 + 0.5;
    precise float uy1 = maxY * 0.5 + 0.5;
    precise float fx0 = fuseCullClamp01(ux0) * C.hizScale[0];
    precise float fx1 = fuseCullClamp01(ux1) * C.hizScale[0];
    precise float fy0 = fuseCullClamp01(uy0) * C.hizScale[1];
    precise float fy1 = fuseCullClamp01(uy1) * C.hizScale[1];
    const uint x0 = min(uint(fx0), last);
    const uint x1 = min(uint(fx1), last);
    const uint y0 = min(uint(fy0), last);
    const uint y1 = min(uint(fy1), last);
    uint mip = 0u;
    while (mip + 1u < C.hizMipCount && (((x1 >> mip) - (x0 >> mip)) > 1u || ((y1 >> mip) - (y0 >> mip)) > 1u)) {
        ++mip;
    }
    const float farDepth = fuseCullMax4(fuseCullHizFetch(C, mip, x0 >> mip, y0 >> mip), fuseCullHizFetch(C, mip, x1 >> mip, y0 >> mip),
                                        fuseCullHizFetch(C, mip, x0 >> mip, y1 >> mip), fuseCullHizFetch(C, mip, x1 >> mip, y1 >> mip));
    return !(minZ > farDepth);
}
