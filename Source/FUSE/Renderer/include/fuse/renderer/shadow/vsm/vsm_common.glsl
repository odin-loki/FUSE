// WP-3.1 virtual shadow maps: GLSL side of include/fuse/renderer/shadow/vsm/vsm_types.hpp and of the
// marking / invalidation math of vsm_kernel.hpp (same expressions, same order; every result that
// feeds a page decision is `precise`, so nothing is contracted and the request bits equal the CPU
// reference bit for bit). Twin: vsm_common.slang. Keep all three in sync.
//
// Include after bindless.glsl and gpu_scene.glsl. WP-3.2 (page rendering, filtering) includes it too:
// fuse_vsm_lookup() maps a light-space point of a level to its physical texel.
#ifndef FUSE_VSM_COMMON_GLSL
#define FUSE_VSM_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_VSM_PAGES_PER_AXIS 128u
#define FUSE_VSM_PAGE_TEXELS 128u
#define FUSE_VSM_PAGES_PER_LEVEL 16384u
#define FUSE_VSM_PTE_MAPPED 0x80000000u
#define FUSE_VSM_PTE_CACHED 0x40000000u
#define FUSE_VSM_PTE_PHYS_MASK 0x00FFFFFFu
#define FUSE_VSM_PAGE_NONE 0xFFFFFFFFu
#define FUSE_VSM_FREE_KEY 16u
#define FUSE_VSM_AGE_BUCKETS 16u
#define FUSE_VSM_MAX_LIGHT_COORD 1e30
#define FUSE_VSM_MAX_PAGE_COORD 1e9

// VsmCounter
#define FUSE_VSM_COUNTER_RENDER_COUNT 0u
#define FUSE_VSM_COUNTER_REQUESTED 3u
#define FUSE_VSM_COUNTER_ALREADY_MAPPED 4u
#define FUSE_VSM_COUNTER_NEEDED 5u
#define FUSE_VSM_COUNTER_CANDIDATES 6u
#define FUSE_VSM_COUNTER_ALLOCATED 7u
#define FUSE_VSM_COUNTER_EVICTED 8u
#define FUSE_VSM_COUNTER_FAILED 9u
#define FUSE_VSM_COUNTER_INVALIDATED 10u
#define FUSE_VSM_COUNTER_SCROLLED 11u

struct FuseVsmLevel { // VsmLevelConstants, 48 bytes
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

// VsmFrameConstants (304 + 16 x 48 bytes).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseVsmConstantsRef {
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
    FuseVsmLevel level[16];
};

uint fuse_vsm_slot(int a) { return uint(a) & (FUSE_VSM_PAGES_PER_AXIS - 1u); }

// Absolute page held by `slot` in the window centred on `origin` (core_logic vsm_slot_to_abs).
int fuse_vsm_slot_to_abs(uint slot, int origin) {
    const int base = origin - int(FUSE_VSM_PAGES_PER_AXIS / 2u);
    const uint off = uint(int(slot) - base) & (FUSE_VSM_PAGES_PER_AXIS - 1u);
    return base + int(off);
}

uint fuse_vsm_candidate_key(uint owner, uint lastUsed, uint frame) {
    if (owner == FUSE_VSM_PAGE_NONE) {
        return FUSE_VSM_FREE_KEY;
    }
    const uint age = frame - lastUsed;
    return min(age, FUSE_VSM_AGE_BUCKETS - 1u);
}

// vsm_kernel.hpp level_of: floor(log2(q)) + 1 for q >= 1 (exponent bits), 0 below.
int fuse_vsm_level_of(float q) {
    return q >= 1.0 ? int((floatBitsToUint(q) >> 23u) & 0xFFu) - 127 + 1 : 0;
}

// vsm_kernel.hpp select_level: max(containment, density + lodBias).
int fuse_vsm_select_level(FuseVsmConstantsRef C, float lx, float ly, float lz) {
    precise float dx = abs(lx - C.cameraLight[0]);
    precise float dy = abs(ly - C.cameraLight[1]);
    precise float dz = abs(lz - C.cameraLight[2]);
    float d = dx > dy ? dx : dy;
    d = d > dz ? d : dz;
    precise float qc = d * C.levelSelectScale;
    precise float qd = d * C.densityScale;
    const int level = max(fuse_vsm_level_of(qc), fuse_vsm_level_of(qd) + C.lodBias);
    return level < int(C.levels) ? level : -1;
}

// vsm_kernel.hpp mark_pixel: the virtual pages a depth texel requests (count 0..4).
uint fuse_vsm_mark_pixel(FuseVsmConstantsRef C, uint x, uint y, float depth, out uint pages[4]) {
    pages[0] = FUSE_VSM_PAGE_NONE;
    pages[1] = FUSE_VSM_PAGE_NONE;
    pages[2] = FUSE_VSM_PAGE_NONE;
    pages[3] = FUSE_VSM_PAGE_NONE;
    if (!(depth < 1.0) || !(depth >= 0.0)) {
        return 0u;
    }
    precise float nx = (float(x) + 0.5) * C.ndcScaleX - 1.0;
    precise float ny = (float(y) + 0.5) * C.ndcScaleY - 1.0;
    precise float hx = ((C.depthToLight[0] * nx + C.depthToLight[4] * ny) + C.depthToLight[8] * depth) + C.depthToLight[12];
    precise float hy = ((C.depthToLight[1] * nx + C.depthToLight[5] * ny) + C.depthToLight[9] * depth) + C.depthToLight[13];
    precise float hz = ((C.depthToLight[2] * nx + C.depthToLight[6] * ny) + C.depthToLight[10] * depth) + C.depthToLight[14];
    precise float hw = ((C.depthToLight[3] * nx + C.depthToLight[7] * ny) + C.depthToLight[11] * depth) + C.depthToLight[15];
    if (!(hw > 0.0)) {
        return 0u;
    }
    precise float lx = hx / hw;
    precise float ly = hy / hw;
    precise float lz = hz / hw;
    if (!(abs(lx) <= FUSE_VSM_MAX_LIGHT_COORD) || !(abs(ly) <= FUSE_VSM_MAX_LIGHT_COORD) ||
        !(abs(lz) <= FUSE_VSM_MAX_LIGHT_COORD)) {
        return 0u;
    }
    const int level = fuse_vsm_select_level(C, lx, ly, lz);
    if (level < 0) {
        return 0u;
    }
    const FuseVsmLevel L = C.level[level];
    precise float u = lx * L.invPageWorld;
    precise float v = ly * L.invPageWorld;
    if (!(abs(u) < FUSE_VSM_MAX_PAGE_COORD) || !(abs(v) < FUSE_VSM_MAX_PAGE_COORD)) {
        return 0u;
    }
    const int half_ = int(FUSE_VSM_PAGES_PER_AXIS / 2u);
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
    precise float u0 = u - r;
    precise float u1 = u + r;
    precise float v0 = v - r;
    precise float v1 = v + r;
    const int x0 = max(int(floor(u0)), wx0);
    const int x1 = min(int(floor(u1)), wx1);
    const int y0 = max(int(floor(v0)), wy0);
    const int y1 = min(int(floor(v1)), wy1);
    const uint base = uint(level) * FUSE_VSM_PAGES_PER_LEVEL;
    uint n = 0u;
    for (int py = y0; py <= y1 && n < 4u; ++py) {
        for (int px = x0; px <= x1 && n < 4u; ++px) {
            pages[n] = base + fuse_vsm_slot(py) * FUSE_VSM_PAGES_PER_AXIS + fuse_vsm_slot(px);
            ++n;
        }
    }
    return n;
}

// VsmBoundsRecord as 8 words: center xyz (float bits), tracked, extent xyz, reserved.
struct FuseVsmBounds {
    uint w[8];
};

// vsm_kernel.hpp make_bounds_record.
FuseVsmBounds fuse_vsm_bounds_record(FuseGpuInstance inst, bool meshValid, FuseGpuMesh mesh, FuseGpuTransform t) {
    FuseVsmBounds r;
    for (uint i = 0u; i < 8u; ++i) {
        r.w[i] = 0u;
    }
    const uint need = FUSE_INSTANCE_VALID | FUSE_INSTANCE_CAST_SHADOW;
    if ((inst.flags & need) != need || !meshValid) {
        return r;
    }
    const float c0 = mesh.boundsCenter[0];
    const float c1 = mesh.boundsCenter[1];
    const float c2 = mesh.boundsCenter[2];
    const float rad = mesh.boundsRadius;
    for (uint k = 0u; k < 3u; ++k) {
        const vec4 row = t.rows[k];
        precise float center = ((row.x * c0 + row.y * c1) + row.z * c2) + row.w;
        precise float extent = rad * ((abs(row.x) + abs(row.y)) + abs(row.z));
        r.w[k] = floatBitsToUint(center);
        r.w[4u + k] = floatBitsToUint(extent);
    }
    r.w[3] = 1u;
    return r;
}

float fuse_vsm_clamp_page(float v) {
    return v < -FUSE_VSM_MAX_PAGE_COORD ? -FUSE_VSM_MAX_PAGE_COORD : (v > FUSE_VSM_MAX_PAGE_COORD ? FUSE_VSM_MAX_PAGE_COORD : v);
}

// vsm_kernel.hpp bounds_rect: absolute page rectangle (x0, y0, x1, y1) of a record on one level,
// clipped to the window; false when untracked, degenerate or outside.
bool fuse_vsm_bounds_rect(FuseVsmConstantsRef C, FuseVsmBounds b, uint level, out ivec4 rect) {
    rect = ivec4(0);
    if (b.w[3] == 0u || level >= C.levels) {
        return false;
    }
    const float bcx = uintBitsToFloat(b.w[0]);
    const float bcy = uintBitsToFloat(b.w[1]);
    const float bcz = uintBitsToFloat(b.w[2]);
    const float bex = uintBitsToFloat(b.w[4]);
    const float bey = uintBitsToFloat(b.w[5]);
    const float bez = uintBitsToFloat(b.w[6]);
    precise float lcx = (C.lightRotation[0] * bcx + C.lightRotation[1] * bcy) + C.lightRotation[2] * bcz;
    precise float lcy = (C.lightRotation[4] * bcx + C.lightRotation[5] * bcy) + C.lightRotation[6] * bcz;
    precise float lex = (abs(C.lightRotation[0]) * bex + abs(C.lightRotation[1]) * bey) + abs(C.lightRotation[2]) * bez;
    precise float ley = (abs(C.lightRotation[4]) * bex + abs(C.lightRotation[5]) * bey) + abs(C.lightRotation[6]) * bez;
    const FuseVsmLevel L = C.level[level];
    precise float fx0 = (lcx - lex) * L.invPageWorld;
    precise float fx1 = (lcx + lex) * L.invPageWorld;
    precise float fy0 = (lcy - ley) * L.invPageWorld;
    precise float fy1 = (lcy + ley) * L.invPageWorld;
    if (!(fx0 <= fx1) || !(fy0 <= fy1)) {
        return false;
    }
    const int half_ = int(FUSE_VSM_PAGES_PER_AXIS / 2u);
    const int x0 = max(int(floor(fuse_vsm_clamp_page(fx0))), L.originX - half_);
    const int x1 = min(int(floor(fuse_vsm_clamp_page(fx1))), L.originX + half_ - 1);
    const int y0 = max(int(floor(fuse_vsm_clamp_page(fy0))), L.originY - half_);
    const int y1 = min(int(floor(fuse_vsm_clamp_page(fy1))), L.originY + half_ - 1);
    if (x0 > x1 || y0 > y1) {
        return false;
    }
    rect = ivec4(x0, y0, x1, y1);
    return true;
}

#endif // FUSE_VSM_COMMON_GLSL
