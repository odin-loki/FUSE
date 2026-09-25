// WP-3.1 virtual shadow maps: Slang twin of vsm_common.glsl (see there). #include after
// gpu_scene.slang. Compile with -fp-mode precise (Slang does not honour `precise` per variable), so no
// multiply-add is contracted and the request bits equal the CPU reference (vsm_kernel.hpp) bit for bit.
#pragma once

static const uint kFuseVsmPagesPerAxis = 128u;
static const uint kFuseVsmPageTexels = 128u;
static const uint kFuseVsmPagesPerLevel = 16384u;
static const uint kFuseVsmPteMapped = 0x80000000u;
static const uint kFuseVsmPteCached = 0x40000000u;
static const uint kFuseVsmPtePhysMask = 0x00FFFFFFu;
static const uint kFuseVsmPageNone = 0xFFFFFFFFu;
static const uint kFuseVsmFreeKey = 16u;
static const uint kFuseVsmAgeBuckets = 16u;
static const float kFuseVsmMaxLightCoord = 1e30;
static const float kFuseVsmMaxPageCoord = 1e9;

static const uint kFuseVsmCounterRenderCount = 0u;
static const uint kFuseVsmCounterRequested = 3u;
static const uint kFuseVsmCounterAlreadyMapped = 4u;
static const uint kFuseVsmCounterNeeded = 5u;
static const uint kFuseVsmCounterCandidates = 6u;
static const uint kFuseVsmCounterAllocated = 7u;
static const uint kFuseVsmCounterEvicted = 8u;
static const uint kFuseVsmCounterFailed = 9u;
static const uint kFuseVsmCounterInvalidated = 10u;
static const uint kFuseVsmCounterScrolled = 11u;

struct VsmLevel { // VsmLevelConstants, 48 bytes
    int originX;
    int originY;
    int prevOriginX;
    int prevOriginY;
    float invPageWorld;
    float pageWorld;
    int depthKey;
    int prevDepthKey;
    float depthCenter;
    float depthStep;
    uint flags;
    uint reserved;
};

struct VsmConstants { // VsmFrameConstants (304 + 16 x 48 bytes)
    uint64_t pageTable;
    uint64_t physMeta;
    uint64_t work;
    uint64_t bounds;
    float depthToLight[16];
    float lightRotation[12];
    float cameraLight[4];
    float levelSelectScale;
    float markRadiusPages;
    float ndcScaleX;
    float ndcScaleY;
    uint levels;
    uint pagesPerAxis;
    uint pageTexels;
    uint physPages;
    uint poolPagesX;
    uint frame;
    uint flags;
    uint depthWidth;
    uint depthHeight;
    uint depthTexture;
    uint depthSampler;
    uint instanceCount;
    uint scene;
    int lodBias;
    uint pageTableHandle;
    uint physMetaHandle;
    uint workHandle;
    uint boundsHandle;
    uint poolHandle;
    uint clearValue;
    uint virtualPages;
    uint requestWords;
    uint offRequest;
    uint offNeed;
    uint offRenderList;
    uint offCandidates;
    uint boundsRead;
    uint boundsWrite;
    uint boundsCapacity;
    float densityScale;
    uint reserved0;
    uint reserved1;
    VsmLevel level[16];
};

uint fuseVsmHandleIndex(uint handle) { return handle & 0xFFFFFu; }

uint fuseVsmSlot(int a) { return uint(a) & (kFuseVsmPagesPerAxis - 1u); }

int fuseVsmSlotToAbs(uint slot, int origin) {
    const int base = origin - int(kFuseVsmPagesPerAxis / 2u);
    const uint off = uint(int(slot) - base) & (kFuseVsmPagesPerAxis - 1u);
    return base + int(off);
}

uint fuseVsmCandidateKey(uint owner, uint lastUsed, uint frame) {
    if (owner == kFuseVsmPageNone) {
        return kFuseVsmFreeKey;
    }
    const uint age = frame - lastUsed;
    return min(age, kFuseVsmAgeBuckets - 1u);
}

int fuseVsmLevelOf(float q) {
    return q >= 1.0 ? int((asuint(q) >> 23u) & 0xFFu) - 127 + 1 : 0;
}

int fuseVsmSelectLevel(VsmConstants* C, float lx, float ly, float lz) {
    const float dx = abs(lx - C.cameraLight[0]);
    const float dy = abs(ly - C.cameraLight[1]);
    const float dz = abs(lz - C.cameraLight[2]);
    float d = dx > dy ? dx : dy;
    d = d > dz ? d : dz;
    const int level = max(fuseVsmLevelOf(d * C.levelSelectScale), fuseVsmLevelOf(d * C.densityScale) + C.lodBias);
    return level < int(C.levels) ? level : -1;
}

uint fuseVsmMarkPixel(VsmConstants* C, uint x, uint y, float depth, out uint pages[4]) {
    pages[0] = kFuseVsmPageNone;
    pages[1] = kFuseVsmPageNone;
    pages[2] = kFuseVsmPageNone;
    pages[3] = kFuseVsmPageNone;
    if (!(depth < 1.0) || !(depth >= 0.0)) {
        return 0u;
    }
    const float nx = (float(x) + 0.5) * C.ndcScaleX - 1.0;
    const float ny = (float(y) + 0.5) * C.ndcScaleY - 1.0;
    const float hx = ((C.depthToLight[0] * nx + C.depthToLight[4] * ny) + C.depthToLight[8] * depth) + C.depthToLight[12];
    const float hy = ((C.depthToLight[1] * nx + C.depthToLight[5] * ny) + C.depthToLight[9] * depth) + C.depthToLight[13];
    const float hz = ((C.depthToLight[2] * nx + C.depthToLight[6] * ny) + C.depthToLight[10] * depth) + C.depthToLight[14];
    const float hw = ((C.depthToLight[3] * nx + C.depthToLight[7] * ny) + C.depthToLight[11] * depth) + C.depthToLight[15];
    if (!(hw > 0.0)) {
        return 0u;
    }
    const float lx = hx / hw;
    const float ly = hy / hw;
    const float lz = hz / hw;
    if (!(abs(lx) <= kFuseVsmMaxLightCoord) || !(abs(ly) <= kFuseVsmMaxLightCoord) || !(abs(lz) <= kFuseVsmMaxLightCoord)) {
        return 0u;
    }
    const int level = fuseVsmSelectLevel(C, lx, ly, lz);
    if (level < 0) {
        return 0u;
    }
    const VsmLevel L = C.level[level];
    const float u = lx * L.invPageWorld;
    const float v = ly * L.invPageWorld;
    if (!(abs(u) < kFuseVsmMaxPageCoord) || !(abs(v) < kFuseVsmMaxPageCoord)) {
        return 0u;
    }
    const int half_ = int(kFuseVsmPagesPerAxis / 2u);
    const int wx0 = L.originX - half_;
    const int wx1 = L.originX + half_ - 1;
    const int wy0 = L.originY - half_;
    const int wy1 = L.originY + half_ - 1;
    const int cx = int(floor(u));
    const int cy = int(floor(v));
    if (cx < wx0 || cx > wx1 || cy < wy0 || cy > wy1) {
        return 0u;
    }
    const float r = C.markRadiusPages;
    const int x0 = max(int(floor(u - r)), wx0);
    const int x1 = min(int(floor(u + r)), wx1);
    const int y0 = max(int(floor(v - r)), wy0);
    const int y1 = min(int(floor(v + r)), wy1);
    const uint base = uint(level) * kFuseVsmPagesPerLevel;
    uint n = 0u;
    for (int py = y0; py <= y1 && n < 4u; ++py) {
        for (int px = x0; px <= x1 && n < 4u; ++px) {
            pages[n] = base + fuseVsmSlot(py) * kFuseVsmPagesPerAxis + fuseVsmSlot(px);
            ++n;
        }
    }
    return n;
}

// VsmBoundsRecord as 8 words: center xyz (float bits), tracked, extent xyz, reserved.
struct VsmBounds {
    uint w[8];
};

VsmBounds fuseVsmBoundsRecord(GpuInstance inst, bool meshValid, GpuMesh mesh, GpuTransform t) {
    VsmBounds r;
    for (uint i = 0u; i < 8u; ++i) {
        r.w[i] = 0u;
    }
    const uint need = kFuseInstanceValid | kFuseInstanceCastShadow;
    if ((inst.flags & need) != need || !meshValid) {
        return r;
    }
    const float c0 = mesh.boundsCenter[0];
    const float c1 = mesh.boundsCenter[1];
    const float c2 = mesh.boundsCenter[2];
    const float rad = mesh.boundsRadius;
    for (uint k = 0u; k < 3u; ++k) {
        const float4 row = t.rows[k];
        const float center = ((row.x * c0 + row.y * c1) + row.z * c2) + row.w;
        const float extent = rad * ((abs(row.x) + abs(row.y)) + abs(row.z));
        r.w[k] = asuint(center);
        r.w[4u + k] = asuint(extent);
    }
    r.w[3] = 1u;
    return r;
}

float fuseVsmClampPage(float v) {
    return v < -kFuseVsmMaxPageCoord ? -kFuseVsmMaxPageCoord : (v > kFuseVsmMaxPageCoord ? kFuseVsmMaxPageCoord : v);
}

bool fuseVsmBoundsRect(VsmConstants* C, VsmBounds b, uint level, out int4 rect) {
    rect = int4(0, 0, 0, 0);
    if (b.w[3] == 0u || level >= C.levels) {
        return false;
    }
    const float bcx = asfloat(b.w[0]);
    const float bcy = asfloat(b.w[1]);
    const float bcz = asfloat(b.w[2]);
    const float bex = asfloat(b.w[4]);
    const float bey = asfloat(b.w[5]);
    const float bez = asfloat(b.w[6]);
    const float lcx = (C.lightRotation[0] * bcx + C.lightRotation[1] * bcy) + C.lightRotation[2] * bcz;
    const float lcy = (C.lightRotation[4] * bcx + C.lightRotation[5] * bcy) + C.lightRotation[6] * bcz;
    const float lex = (abs(C.lightRotation[0]) * bex + abs(C.lightRotation[1]) * bey) + abs(C.lightRotation[2]) * bez;
    const float ley = (abs(C.lightRotation[4]) * bex + abs(C.lightRotation[5]) * bey) + abs(C.lightRotation[6]) * bez;
    const VsmLevel L = C.level[level];
    const float fx0 = (lcx - lex) * L.invPageWorld;
    const float fx1 = (lcx + lex) * L.invPageWorld;
    const float fy0 = (lcy - ley) * L.invPageWorld;
    const float fy1 = (lcy + ley) * L.invPageWorld;
    if (!(fx0 <= fx1) || !(fy0 <= fy1)) {
        return false;
    }
    const int half_ = int(kFuseVsmPagesPerAxis / 2u);
    const int x0 = max(int(floor(fuseVsmClampPage(fx0))), L.originX - half_);
    const int x1 = min(int(floor(fuseVsmClampPage(fx1))), L.originX + half_ - 1);
    const int y0 = max(int(floor(fuseVsmClampPage(fy0))), L.originY - half_);
    const int y1 = min(int(floor(fuseVsmClampPage(fy1))), L.originY + half_ - 1);
    if (x0 > x1 || y0 > y1) {
        return false;
    }
    rect = int4(x0, y0, x1, y1);
    return true;
}
