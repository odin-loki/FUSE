// WP-3.2 virtual shadow maps: Slang twin of vsm_shadow.glsl (see there; C++ reference
// vsm_raster_kernel.hpp). #include after gpu_scene.slang; compile with -fp-mode precise.
#pragma once
#include "../../include/fuse/renderer/shadow/vsm/vsm_common.slang"

static const uint kFuseVsmrMaxLocal = 16u;
static const uint kFuseVsmrFilterHard = 0u;
static const uint kFuseVsmrFilterPcf = 1u;
static const uint kFuseVsmrFilterPcss = 2u;
static const uint kFuseVsmrLocalSpot = 1u;
static const uint kFuseVsmrLocalPoint = 2u;
static const uint kFuseVsmrMaxRadius = 4u;
static const uint kFuseVsmrBlockerGrid = 5u;
static const float kFuseVsmrDepthPages = 256.0;
static const float kFuseVsmrDepthPerTexel = 1.0 / 32768.0;
static const int kFuseVsmrWindowTexels = 16384;

struct VsmLocal { // VsmLocalLight, 128 bytes
    float position[3];
    uint slot;
    float forward[3];
    float range;
    float right[3];
    float nearPlane;
    float up[3];
    float invTanHalf;
    uint type;
    uint faces;
    float invRange;
    float lightSize;
    uint page[6];
    uint reserved[6];
};

struct VsmShadow { // VsmShadowConstants, 128 + 16 x 128 bytes
    uint64_t vsm;
    uint64_t work;
    uint directionalSlot;
    uint filterMode;
    uint pcfRadius;
    uint localCount;
    float normalOffset;
    float depthBias;
    float sunTanAngle;
    uint pcssMaxRadius;
    uint localPool;
    uint localPagesX;
    uint scene;
    uint instanceCount;
    uint forceMask;
    uint workHandle;
    uint localClear;
    uint frame;
    uint reserved[12];
    VsmLocal local[16];
};

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> fuse_vsm_shadow_images[];

uint fuseVsmrLoad(uint handle, int2 p) { return fuse_vsm_shadow_images[handle & 0xFFFFFu].Load(p); }

// --- math shared with the rasteriser --------------------------------------------------------------
float3 fuseVsmrLightPoint(VsmConstants* C, float3 w) {
    const float lx = (C.lightRotation[0] * w.x + C.lightRotation[1] * w.y) + C.lightRotation[2] * w.z;
    const float ly = (C.lightRotation[4] * w.x + C.lightRotation[5] * w.y) + C.lightRotation[6] * w.z;
    const float lz = (C.lightRotation[8] * w.x + C.lightRotation[9] * w.y) + C.lightRotation[10] * w.z;
    return float3(lx, ly, lz);
}

float fuseVsmrDirDepth(VsmLevel L, float lz) { return ((L.depthCenter - lz) * L.invPageWorld) * (1.0 / kFuseVsmrDepthPages) + 0.5; }

void fuseVsmrCubeBasis(uint face, out float3 f, out float3 r, out float3 u) {
    f = float3(0.0, 0.0, 0.0);
    r = float3(0.0, 0.0, 0.0);
    u = float3(0.0, 0.0, 0.0);
    if (face == 0u) {
        f.x = 1.0; r.z = -1.0; u.y = 1.0;
    } else if (face == 1u) {
        f.x = -1.0; r.z = 1.0; u.y = 1.0;
    } else if (face == 2u) {
        f.y = 1.0; r.x = 1.0; u.z = -1.0;
    } else if (face == 3u) {
        f.y = -1.0; r.x = 1.0; u.z = 1.0;
    } else if (face == 4u) {
        f.z = 1.0; r.x = 1.0; u.y = 1.0;
    } else {
        f.z = -1.0; r.x = -1.0; u.y = 1.0;
    }
}

uint fuseVsmrCubeFace(float3 rel) {
    const float ax = abs(rel.x);
    const float ay = abs(rel.y);
    const float az = abs(rel.z);
    if (ax >= ay && ax >= az) {
        return rel.x >= 0.0 ? 0u : 1u;
    }
    if (ay >= az) {
        return rel.y >= 0.0 ? 2u : 3u;
    }
    return rel.z >= 0.0 ? 4u : 5u;
}

float fuseVsmrDot(float3 a, float3 b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }

void fuseVsmrLocalBasis(VsmLocal e, uint face, out float3 f, out float3 r, out float3 u) {
    if (e.type == kFuseVsmrLocalPoint) {
        fuseVsmrCubeBasis(face, f, r, u);
        return;
    }
    f = float3(e.forward[0], e.forward[1], e.forward[2]);
    r = float3(e.right[0], e.right[1], e.right[2]);
    u = float3(e.up[0], e.up[1], e.up[2]);
}

float3 fuseVsmrLocalView(VsmLocal e, float3 f, float3 r, float3 u, float3 w) {
    const float3 rel = float3(w.x - e.position[0], w.y - e.position[1], w.z - e.position[2]);
    return float3(fuseVsmrDot(rel, r), fuseVsmrDot(rel, u), fuseVsmrDot(rel, f));
}

float3 fuseVsmrLocalProject(VsmLocal e, float3 v) {
    const float x = ((v.x / v.z) * e.invTanHalf) * 64.0 + 64.0;
    const float y = ((v.y / v.z) * e.invTanHalf) * 64.0 + 64.0;
    const float iz = 1.0 / v.z;
    return float3(x, y, iz);
}

// --- lookup ---------------------------------------------------------------------------------------
struct VsmrTaps {
    float lit;
    float taps;
    float margin;
    float blockerSum;
    float blockers;
};

VsmrTaps fuseVsmrTaps() {
    VsmrTaps a;
    a.lit = 0.0;
    a.taps = 0.0;
    a.margin = 3.0e38;
    a.blockerSum = 0.0;
    a.blockers = 0.0;
    return a;
}

void fuseVsmrCompare(inout VsmrTaps a, float rd, uint bits) {
    const float stored = asfloat(bits);
    a.taps = a.taps + 1.0;
    a.lit = a.lit + (rd <= stored ? 1.0 : 0.0);
    a.margin = min(a.margin, abs(rd - stored));
}

void fuseVsmrBlocker(inout VsmrTaps a, float rd, uint bits) {
    const float stored = asfloat(bits);
    a.margin = min(a.margin, abs(rd - stored));
    if (stored < rd) {
        a.blockerSum = a.blockerSum + stored;
        a.blockers = a.blockers + 1.0;
    }
}

int fuseVsmrBlockerOffset(uint k, uint radius) {
    const int i = int(k) - 2;
    const int m = int((uint(i < 0 ? -i : i) * radius) / 2u);
    return i < 0 ? -m : m;
}

uint fuseVsmrPenumbraRadius(float texels, uint cap) {
    const float w = texels > 0.0 ? texels + 0.5 : 0.0;
    const float capped = w < float(cap) ? w : float(cap);
    return uint(floor(capped));
}

bool fuseVsmrDirFetch(VsmConstants* C, uint level, int2 p, out uint bits) {
    bits = 0u;
    if (p.x < 0 || p.y < 0 || p.x >= kFuseVsmrWindowTexels || p.y >= kFuseVsmrWindowTexels) {
        return false;
    }
    const VsmLevel L = C.level[level];
    const int half_ = int(kFuseVsmPagesPerAxis / 2u);
    const int ax = (L.originX - half_) + p.x / int(kFuseVsmPageTexels);
    const int ay = (L.originY - half_) + p.y / int(kFuseVsmPageTexels);
    const uint v = level * kFuseVsmPagesPerLevel + fuseVsmSlot(ay) * kFuseVsmPagesPerAxis + fuseVsmSlot(ax);
    const uint pte = ((uint*)C.pageTable)[v];
    if ((pte & kFuseVsmPteMapped) == 0u) {
        return false;
    }
    const uint phys = pte & kFuseVsmPtePhysMask;
    const uint x = (phys % C.poolPagesX) * kFuseVsmPageTexels + uint(p.x) % kFuseVsmPageTexels;
    const uint y = (phys / C.poolPagesX) * kFuseVsmPageTexels + uint(p.y) % kFuseVsmPageTexels;
    bits = fuseVsmrLoad(C.poolHandle, int2(int(x), int(y)));
    return true;
}

struct VsmrSample {
    float visibility;
    int level;
    float receiverDepth;
    float margin;
};

VsmrSample fuseVsmrSample() {
    VsmrSample r;
    r.visibility = 1.0;
    r.level = -1;
    r.receiverDepth = 0.0;
    r.margin = 3.0e38;
    return r;
}

VsmrSample fuseVsmrDirVisibility(VsmConstants* C, VsmShadow* S, float3 pos, float3 n, int forceLevel) {
    VsmrSample r = fuseVsmrSample();
    const float3 lp = fuseVsmrLightPoint(C, pos);
    if (!(abs(lp.x) <= kFuseVsmMaxLightCoord) || !(abs(lp.y) <= kFuseVsmMaxLightCoord) || !(abs(lp.z) <= kFuseVsmMaxLightCoord)) {
        return r;
    }
    int first = forceLevel;
    if (first < 0) {
        const float dx = abs(lp.x - C.cameraLight[0]);
        const float dy = abs(lp.y - C.cameraLight[1]);
        const float dz = abs(lp.z - C.cameraLight[2]);
        float d = dx > dy ? dx : dy;
        d = d > dz ? d : dz;
        first = max(fuseVsmLevelOf(d * C.levelSelectScale), fuseVsmLevelOf(d * C.densityScale) + C.lodBias);
    }
    const int levels = int(min(C.levels, 16u));
    if (first >= levels) {
        if (forceLevel >= 0) {
            r.visibility = -1.0;
        }
        return r;
    }
    // Levels tried: the selected one, one finer, then coarser ones.
    const int tries = forceLevel >= 0 ? 1 : levels - first + 1;
    for (int i = 0; i < tries; ++i) {
        const int lev = i == 0 ? first : (i == 1 ? first - 1 : first + i - 1);
        if (lev < 0) {
            continue;
        }
        const VsmLevel L = C.level[lev];
        const float texel = L.pageWorld * (1.0 / float(kFuseVsmPageTexels));
        const float off = S.normalOffset * texel;
        const float3 bp = float3(pos.x + n.x * off, pos.y + n.y * off, pos.z + n.z * off);
        const float3 bl = fuseVsmrLightPoint(C, bp);
        const float u = bl.x * L.invPageWorld;
        const float v = bl.y * L.invPageWorld;
        if (!(abs(u) < kFuseVsmMaxPageCoord) || !(abs(v) < kFuseVsmMaxPageCoord)) {
            return r;
        }
        const int half_ = int(kFuseVsmPagesPerAxis / 2u);
        const float tu = (u - float(L.originX - half_)) * float(kFuseVsmPageTexels);
        const float tv = (v - float(L.originY - half_)) * float(kFuseVsmPageTexels);
        const float limit = float(kFuseVsmrWindowTexels);
        uint bits = 0u;
        const bool inside = tu >= 0.0 && tu < limit && tv >= 0.0 && tv < limit;
        const int ix = inside ? int(floor(tu)) : -1;
        const int iy = inside ? int(floor(tv)) : -1;
        if (!inside || !fuseVsmrDirFetch(C, uint(lev), int2(ix, iy), bits)) {
            if (forceLevel >= 0) {
                r.visibility = -1.0;
                r.level = lev;
                return r;
            }
            continue;
        }
        const float rd = fuseVsmrDirDepth(L, bl.z) - S.depthBias * kFuseVsmrDepthPerTexel;
        r.level = lev;
        r.receiverDepth = rd;
        VsmrTaps a = fuseVsmrTaps();
        int radius = 0;
        if (S.filterMode == kFuseVsmrFilterPcf) {
            radius = int(min(S.pcfRadius, kFuseVsmrMaxRadius));
        } else if (S.filterMode == kFuseVsmrFilterPcss) {
            const uint search = min(S.pcssMaxRadius, kFuseVsmrMaxRadius);
            for (uint j = 0u; j < kFuseVsmrBlockerGrid; ++j) {
                for (uint i = 0u; i < kFuseVsmrBlockerGrid; ++i) {
                    uint b = 0u;
                    const int2 q = int2(ix + fuseVsmrBlockerOffset(i, search), iy + fuseVsmrBlockerOffset(j, search));
                    if (fuseVsmrDirFetch(C, uint(lev), q, b)) {
                        fuseVsmrBlocker(a, rd, b);
                    }
                }
            }
            if (!(a.blockers > 0.0)) {
                r.visibility = 1.0;
                r.margin = a.margin;
                return r;
            }
            const float avg = a.blockerSum / a.blockers;
            const float texels = ((rd - avg) * (1.0 / kFuseVsmrDepthPerTexel)) * S.sunTanAngle;
            radius = int(fuseVsmrPenumbraRadius(texels, search));
        }
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                uint b = 0u;
                if (fuseVsmrDirFetch(C, uint(lev), int2(ix + dx, iy + dy), b)) {
                    fuseVsmrCompare(a, rd, b);
                }
            }
        }
        r.visibility = a.lit / a.taps;
        r.margin = a.margin;
        return r;
    }
    return r;
}

uint fuseVsmrLocalFetch(VsmShadow* S, uint page, int ix, int iy) {
    const int top = 127;
    const uint x = uint(ix < 0 ? 0 : (ix > top ? top : ix));
    const uint y = uint(iy < 0 ? 0 : (iy > top ? top : iy));
    const uint px = (page % S.localPagesX) * 128u + x;
    const uint py = (page / S.localPagesX) * 128u + y;
    return fuseVsmrLoad(S.localPool, int2(int(px), int(py)));
}

VsmrSample fuseVsmrLocalVisibility(VsmShadow* S, VsmLocal e, float3 pos, float3 n) {
    VsmrSample r = fuseVsmrSample();
    float3 f, rt, up;
    const float3 rel0 = float3(pos.x - e.position[0], pos.y - e.position[1], pos.z - e.position[2]);
    uint face = e.type == kFuseVsmrLocalPoint ? fuseVsmrCubeFace(rel0) : 0u;
    fuseVsmrLocalBasis(e, face, f, rt, up);
    float3 v = fuseVsmrLocalView(e, f, rt, up, pos);
    if (!(v.z > e.nearPlane)) {
        return r;
    }
    const float texel = (v.z * (1.0 / 64.0)) / e.invTanHalf;
    const float off = S.normalOffset * texel;
    const float3 bp = float3(pos.x + n.x * off, pos.y + n.y * off, pos.z + n.z * off);
    if (e.type == kFuseVsmrLocalPoint) {
        const float3 rel = float3(bp.x - e.position[0], bp.y - e.position[1], bp.z - e.position[2]);
        face = fuseVsmrCubeFace(rel);
        fuseVsmrLocalBasis(e, face, f, rt, up);
    }
    v = fuseVsmrLocalView(e, f, rt, up, bp);
    if (!(v.z > e.nearPlane)) {
        return r;
    }
    float3 t = fuseVsmrLocalProject(e, v);
    const float limit = 128.0;
    if (!(t.x >= 0.0 && t.x < limit && t.y >= 0.0 && t.y < limit)) {
        if (e.type != kFuseVsmrLocalPoint) {
            return r;
        }
        t.x = t.x < 0.0 ? 0.0 : (t.x < limit ? t.x : limit - 1.0);
        t.y = t.y < 0.0 ? 0.0 : (t.y < limit ? t.y : limit - 1.0);
    }
    const int ix = int(floor(t.x));
    const int iy = int(floor(t.y));
    const uint page = e.page[face < 6u ? face : 0u];
    const float rd = v.z * e.invRange - (S.depthBias * texel) * e.invRange;
    r.receiverDepth = rd;
    VsmrTaps a = fuseVsmrTaps();
    int radius = 0;
    if (S.filterMode == kFuseVsmrFilterPcf) {
        radius = int(min(S.pcfRadius, kFuseVsmrMaxRadius));
    } else if (S.filterMode == kFuseVsmrFilterPcss) {
        const uint search = min(S.pcssMaxRadius, kFuseVsmrMaxRadius);
        for (uint j = 0u; j < kFuseVsmrBlockerGrid; ++j) {
            for (uint i = 0u; i < kFuseVsmrBlockerGrid; ++i) {
                fuseVsmrBlocker(a, rd, fuseVsmrLocalFetch(S, page, ix + fuseVsmrBlockerOffset(i, search), iy + fuseVsmrBlockerOffset(j, search)));
            }
        }
        if (!(a.blockers > 0.0)) {
            r.margin = a.margin;
            return r;
        }
        const float zb = (a.blockerSum / a.blockers) * e.range;
        const float zr = v.z;
        const float world = ((zr - zb) / max(zb, e.nearPlane)) * e.lightSize;
        const float texels = ((world * e.invTanHalf) * 64.0) / zr;
        radius = int(fuseVsmrPenumbraRadius(texels, search));
    }
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            fuseVsmrCompare(a, rd, fuseVsmrLocalFetch(S, page, ix + dx, iy + dy));
        }
    }
    r.visibility = a.lit / a.taps;
    r.margin = a.margin;
    return r;
}

VsmrSample fuseVsmrShadowSample(uint64_t address, uint slot, float3 pos, float3 n, int forceLevel) {
    if (address == 0ull) {
        return fuseVsmrSample();
    }
    VsmShadow* S = (VsmShadow*)address;
    if (slot == S.directionalSlot && S.vsm != 0ull) {
        return fuseVsmrDirVisibility((VsmConstants*)S.vsm, S, pos, n, forceLevel);
    }
    const uint count = min(S.localCount, kFuseVsmrMaxLocal);
    for (uint k = 0u; k < count; ++k) {
        if (S.local[k].slot == slot) {
            return fuseVsmrLocalVisibility(S, S.local[k], pos, n);
        }
    }
    return fuseVsmrSample();
}

float fuseVsmShadow(uint64_t address, uint slot, float3 pos, float3 n) { return fuseVsmrShadowSample(address, slot, pos, n, -1).visibility; }
