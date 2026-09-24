// FUSE compute software rasteriser (WP-5.4): Slang twin of swraster_common.glsl (see there) and of
// include/fuse/renderer/swraster/swraster_kernel.hpp. #include after gpu_scene.slang, vis_format.slang
// and vis_common.slang; compile with -fp-mode precise (the vertex transform must not be contracted).
#pragma once

static const int kFuseSwSubpixelBits = 8;
static const int kFuseSwSubpixelOne = 256;
static const int kFuseSwSubpixelHalf = 128;
static const uint kFuseSwGroupSize = 32u;
static const uint kFuseSwRasterThreads = 64u;
static const uint kFuseSwMaxVertices = 64u;
static const uint kFuseSwMaxTriangles = 124u;
static const int kFuseSwMaxSpanSubpixels = 12288;
static const uint64_t kFuseSwCoordLimit = 1073741824ull;

static const uint kFuseSwResultNone = 0u;
static const uint kFuseSwResultCulled = 1u;
static const uint kFuseSwResultSoftware = 2u;
static const uint kFuseSwResultHardwareSize = 3u;
static const uint kFuseSwResultHardwareExtent = 4u;
static const uint kFuseSwResultHardwareClip = 5u;
static const uint kFuseSwResultHardwareForced = 6u;
static const uint kFuseSwResultOversize = 7u;

static const uint kFuseSwModeClassify = 0u;
static const uint kFuseSwModeForceSoftware = 1u;
static const uint kFuseSwModeForceHardware = 2u;

static const uint kFuseSwFlagWriteResults = 1u;
static const uint kFuseSwFlagSkipSoftware = 2u;
static const uint kFuseSwFlagSkipHardware = 4u;

static const uint kFuseSwCountClassify0 = 0u;
static const uint kFuseSwCountSoftware0 = 6u;
static const uint kFuseSwCountHardware0 = 12u;
static const uint kFuseSwCountGroupsRequested0 = 20u;
static const uint kFuseSwCountSwRequested0 = 22u;
static const uint kFuseSwCountHwRequested0 = 24u;
static const uint kFuseSwCountOverflow = 26u;
static const uint kFuseSwCountSkipped = 27u;
static const uint kFuseSwCountDemoted = 28u;
static const uint kFuseSwCountOversize = 29u;
static const uint kFuseSwCountSwTriangles = 30u;

static const uint kFuseSwCullPhase1Draws = 0u;
static const uint kFuseSwCullPhase2Draws = 1u;

uint fuseSwHandleIndex(uint handle) { return handle & 0xFFFFFu; }

// SwRasterConstants (144 bytes).
struct SwConstants {
    float viewProj[16];
    uint scene;
    uint width;
    uint height;
    uint flags;
    uint mode;
    uint triangleThreshold;
    uint maxClusterExtent;
    uint capacity;
    uint groupsBuffer;
    uint swBuffer;
    uint hwBuffer;
    uint countsBuffer;
    uint resultsBuffer;
    uint target64;
    uint cullArgsBuffer;
    uint cullCountsBuffer;
    uint phase2DrawBase;
    uint maxDraws;
    uint reserved[2];
};

void fuseSwViewProj(SwConstants* C, out float4 vp[4]) {
    for (uint c = 0u; c < 4u; ++c) {
        vp[c] = float4(C.viewProj[c * 4u], C.viewProj[c * 4u + 1u], C.viewProj[c * 4u + 2u], C.viewProj[c * 4u + 3u]);
    }
}

bool fuseSwRatioRound(float num, float den, uint scale, out int64_t outValue) {
    outValue = 0ll;
    const uint nb = asuint(num);
    const uint db = asuint(den);
    const uint ne = (nb >> 23u) & 0xFFu;
    const uint de = (db >> 23u) & 0xFFu;
    if ((db >> 31u) != 0u || de == 0u || de == 0xFFu || ne == 0xFFu) {
        return false;
    }
    if ((nb & 0x7FFFFFFFu) == 0u) {
        return true;
    }
    const uint64_t mn = ne == 0u ? uint64_t(nb & 0x7FFFFFu) : uint64_t((nb & 0x7FFFFFu) | 0x800000u);
    const int en = ne == 0u ? -149 : int(ne) - 150;
    const uint64_t md = uint64_t((db & 0x7FFFFFu) | 0x800000u);
    const int ed = int(de) - 150;
    const int k = en - ed;
    uint64_t numer = mn * uint64_t(scale) * 2ull;
    uint64_t denom = md;
    if (k >= 0) {
        if (k > 15) {
            return false;
        }
        numer = numer << uint64_t(k);
    } else {
        if (-k > 38) {
            return true;
        }
        denom = denom << uint64_t(-k);
    }
    const bool negative = (nb >> 31u) != 0u;
    const uint64_t q = negative ? (numer + denom - 1ull) / (denom * 2ull) : (numer + denom) / (denom * 2ull);
    if (q > kFuseSwCoordLimit) {
        return false;
    }
    outValue = negative ? -int64_t(q) : int64_t(q);
    return true;
}

uint fuseSwRatioFloorZ(float z, float w) {
    const uint zb = asuint(z);
    const uint wb = asuint(w);
    if ((zb & 0x7FFFFFFFu) == 0u) {
        return 0u;
    }
    const uint ze = (zb >> 23u) & 0xFFu;
    const uint64_t mz = ze == 0u ? uint64_t(zb & 0x7FFFFFu) : uint64_t((zb & 0x7FFFFFu) | 0x800000u);
    const int ez = ze == 0u ? -149 : int(ze) - 150;
    const uint64_t mw = uint64_t((wb & 0x7FFFFFu) | 0x800000u);
    const int ew = int((wb >> 23u) & 0xFFu) - 150;
    const int k = ez - ew + 32;
    uint64_t q = 0ull;
    if (k >= 0) {
        q = (mz << uint64_t(k)) / mw;
    } else if (-k < 24) {
        q = mz / (mw << uint64_t(-k));
    }
    return q > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint(q);
}

struct SwVertex {
    int x;
    int y;
    uint z;
    uint valid;
};

SwVertex fuseSwProject(float4 c, uint width, uint height) {
    SwVertex v;
    v.x = 0;
    v.y = 0;
    v.z = 0u;
    v.valid = 0u;
    if (!(c.w > 0.0) || !(c.z >= 0.0) || !(c.z <= c.w)) {
        return v;
    }
    int64_t rx;
    int64_t ry;
    if (!fuseSwRatioRound(c.x, c.w, width << 7u, rx) || !fuseSwRatioRound(c.y, c.w, height << 7u, ry)) {
        return v;
    }
    v.x = int(rx + int64_t(width << 7u));
    v.y = int(ry + int64_t(height << 7u));
    v.z = fuseSwRatioFloorZ(c.z, c.w);
    v.valid = 1u;
    return v;
}

int fuseSwEdgeBias(int dx, int dy) { return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1; }
int fuseSwFirstCentre(int v) { return (v + (kFuseSwSubpixelHalf - 1)) >> kFuseSwSubpixelBits; }
int fuseSwLastCentre(int v) { return (v - kFuseSwSubpixelHalf) >> kFuseSwSubpixelBits; }

uint64_t fuseSwPack(uint depth24, uint instance, uint triangle) {
    return (uint64_t(depth24) << kFuseVis64DepthShift) | (uint64_t(instance & kFuseVis64IdMask) << kFuseVis64InstanceShift) |
           uint64_t(triangle & kFuseVis64IdMask);
}

uint fuseSwMeshletVertices(GpuMeshlet m) { return m.counts & 0xFFu; }
uint fuseSwMeshletTriangles(GpuMeshlet m) { return (m.counts >> 8u) & 0xFFu; }

uint fuseSwClassify(GpuMeshlet m, GpuTransform xf, SwConstants* C) {
    const uint vertices = fuseSwMeshletVertices(m);
    const uint triangles = fuseSwMeshletTriangles(m);
    if (vertices > kFuseSwMaxVertices || triangles > kFuseSwMaxTriangles) {
        return kFuseSwResultOversize;
    }
    float4 vp[4];
    fuseSwViewProj(C, vp);
    float4 corner[8];
    uint allOut = 0x3Fu;
    bool clip = false;
    for (uint k = 0u; k < 8u; ++k) {
        float3 p;
        p.x = (k & 1u) != 0u ? m.aabbMax[0] : m.aabbMin[0];
        p.y = (k & 2u) != 0u ? m.aabbMax[1] : m.aabbMin[1];
        p.z = (k & 4u) != 0u ? m.aabbMax[2] : m.aabbMin[2];
        const float4 q = fuseVisClip(xf, vp, p);
        corner[k] = q;
        const uint outCode = (q.x < -q.w ? 1u : 0u) | (q.x > q.w ? 2u : 0u) | (q.y < -q.w ? 4u : 0u) | (q.y > q.w ? 8u : 0u) |
                             (q.z < 0.0 ? 16u : 0u) | (q.z > q.w ? 32u : 0u);
        allOut &= outCode;
        if (!(q.w > 0.0) || !(q.z >= 0.0) || !(q.z <= q.w)) {
            clip = true;
        }
    }
    if (allOut != 0u) {
        return kFuseSwResultCulled;
    }
    if (C.mode == kFuseSwModeForceHardware) {
        return kFuseSwResultHardwareForced;
    }
    if (clip) {
        return kFuseSwResultHardwareClip;
    }
    int minX = 0x7FFFFFFF;
    int minY = 0x7FFFFFFF;
    int maxX = -0x7FFFFFFF;
    int maxY = -0x7FFFFFFF;
    for (uint k = 0u; k < 8u; ++k) {
        const SwVertex v = fuseSwProject(corner[k], C.width, C.height);
        if (v.valid == 0u) {
            return kFuseSwResultHardwareClip;
        }
        minX = v.x < minX ? v.x : minX;
        maxX = v.x > maxX ? v.x : maxX;
        minY = v.y < minY ? v.y : minY;
        maxY = v.y > maxY ? v.y : maxY;
    }
    const int lastX = int(C.width - 1u) * kFuseSwSubpixelOne + kFuseSwSubpixelHalf;
    const int lastY = int(C.height - 1u) * kFuseSwSubpixelOne + kFuseSwSubpixelHalf;
    if (maxX + kFuseSwSubpixelOne < kFuseSwSubpixelHalf || maxY + kFuseSwSubpixelOne < kFuseSwSubpixelHalf ||
        minX - kFuseSwSubpixelOne > lastX || minY - kFuseSwSubpixelOne > lastY) {
        return kFuseSwResultCulled;
    }
    const uint rectW = uint(maxX - minX);
    const uint rectH = uint(maxY - minY);
    if (rectW > C.maxClusterExtent || rectH > C.maxClusterExtent) {
        return kFuseSwResultHardwareExtent;
    }
    if (C.mode == kFuseSwModeForceSoftware) {
        return kFuseSwResultSoftware;
    }
    const uint64_t thr = uint64_t(C.triangleThreshold);
    return uint64_t(rectW) * uint64_t(rectH) <= thr * thr * uint64_t(triangles) ? kFuseSwResultSoftware
                                                                                 : kFuseSwResultHardwareSize;
}
