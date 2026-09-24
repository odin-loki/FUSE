// WP-3.2 virtual shadow maps: GLSL side of include/fuse/renderer/shadow/vsm_raster/vsm_raster_types.hpp
// and of the shadow lookup of vsm_raster_kernel.hpp (raster_math::dir_visibility / local_visibility /
// shadow_visibility): same expressions in the same order, `precise` (no contraction). Twin:
// vsm_shadow.slang. Keep all three in sync.
//
// Include after bindless.glsl and gpu_scene.glsl. light.shade and the forward pass reach it through
// lc_ltc.glsl: fuse_vsm_shadow(address, slot, position, normal) is 1 for address 0 (no shadows) and
// for lights without a shadow. The images are read as R32_UINT storage images (GENERAL layout), so
// every reading pass declares StorageRead of the pool, the page table and the local atlas
// (VsmShadows::addSamplingUse).
#ifndef FUSE_VSM_SHADOW_GLSL
#define FUSE_VSM_SHADOW_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#include "../../include/fuse/renderer/shadow/vsm/vsm_common.glsl"

#define FUSE_VSMR_MAX_LOCAL 16u
#define FUSE_VSMR_FILTER_HARD 0u
#define FUSE_VSMR_FILTER_PCF 1u
#define FUSE_VSMR_FILTER_PCSS 2u
#define FUSE_VSMR_LOCAL_SPOT 1u
#define FUSE_VSMR_LOCAL_POINT 2u
#define FUSE_VSMR_MAX_RADIUS 4u
#define FUSE_VSMR_BLOCKER_GRID 5u
#define FUSE_VSMR_DEPTH_PAGES 256.0
#define FUSE_VSMR_DEPTH_PER_TEXEL (1.0 / 32768.0)
#define FUSE_VSMR_WINDOW_TEXELS 16384
#define FUSE_VSMR_NO_SLOT 0xFFFFFFFFu

// VsmLocalLight, 128 bytes.
struct FuseVsmLocal {
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

// VsmShadowConstants, 128 + 16 x 128 bytes.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseVsmShadowRef {
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
    FuseVsmLocal local[16];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseVsmrWordsRef { uint v[]; };

FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(r32ui) uniform readonly uimage2D fuse_vsm_shadow_images[];

uint fuse_vsmr_load(uint handle, ivec2 p) { return imageLoad(fuse_vsm_shadow_images[fuse_handle_index(handle)], p).r; }

// --- math shared with the rasteriser --------------------------------------------------------------
vec3 fuse_vsmr_light_point(FuseVsmConstantsRef C, vec3 w) {
    precise float lx = (C.lightRotation[0] * w.x + C.lightRotation[1] * w.y) + C.lightRotation[2] * w.z;
    precise float ly = (C.lightRotation[4] * w.x + C.lightRotation[5] * w.y) + C.lightRotation[6] * w.z;
    precise float lz = (C.lightRotation[8] * w.x + C.lightRotation[9] * w.y) + C.lightRotation[10] * w.z;
    return vec3(lx, ly, lz);
}

float fuse_vsmr_dir_depth(FuseVsmLevel L, float lz) {
    precise float d = ((L.depthCenter - lz) * L.invPageWorld) * (1.0 / FUSE_VSMR_DEPTH_PAGES) + 0.5;
    return d;
}

void fuse_vsmr_cube_basis(uint face, out vec3 f, out vec3 r, out vec3 u) {
    f = vec3(0.0);
    r = vec3(0.0);
    u = vec3(0.0);
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

uint fuse_vsmr_cube_face(vec3 rel) {
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

float fuse_vsmr_dot(vec3 a, vec3 b) {
    precise float r = (a.x * b.x + a.y * b.y) + a.z * b.z;
    return r;
}

void fuse_vsmr_local_basis(FuseVsmLocal e, uint face, out vec3 f, out vec3 r, out vec3 u) {
    if (e.type == FUSE_VSMR_LOCAL_POINT) {
        fuse_vsmr_cube_basis(face, f, r, u);
        return;
    }
    f = vec3(e.forward[0], e.forward[1], e.forward[2]);
    r = vec3(e.right[0], e.right[1], e.right[2]);
    u = vec3(e.up[0], e.up[1], e.up[2]);
}

vec3 fuse_vsmr_local_view(FuseVsmLocal e, vec3 f, vec3 r, vec3 u, vec3 w) {
    precise vec3 rel = vec3(w.x - e.position[0], w.y - e.position[1], w.z - e.position[2]);
    return vec3(fuse_vsmr_dot(rel, r), fuse_vsmr_dot(rel, u), fuse_vsmr_dot(rel, f));
}

// Screen texel coordinates (xy) and 1 / z (z) of a view-space point in front of the near plane.
vec3 fuse_vsmr_local_project(FuseVsmLocal e, vec3 v) {
    precise float x = ((v.x / v.z) * e.invTanHalf) * 64.0 + 64.0;
    precise float y = ((v.y / v.z) * e.invTanHalf) * 64.0 + 64.0;
    precise float iz = 1.0 / v.z;
    return vec3(x, y, iz);
}

// --- lookup ---------------------------------------------------------------------------------------
struct FuseVsmrTaps {
    float lit;
    float taps;
    float margin;
    float blockerSum;
    float blockers;
};

FuseVsmrTaps fuse_vsmr_taps() {
    FuseVsmrTaps a;
    a.lit = 0.0;
    a.taps = 0.0;
    a.margin = 3.0e38;
    a.blockerSum = 0.0;
    a.blockers = 0.0;
    return a;
}

void fuse_vsmr_compare(inout FuseVsmrTaps a, float rd, uint bits) {
    const float stored = uintBitsToFloat(bits);
    a.taps = a.taps + 1.0;
    a.lit = a.lit + (rd <= stored ? 1.0 : 0.0);
    precise float diff = abs(rd - stored);
    a.margin = min(a.margin, diff);
}

void fuse_vsmr_blocker(inout FuseVsmrTaps a, float rd, uint bits) {
    const float stored = uintBitsToFloat(bits);
    precise float diff = abs(rd - stored);
    a.margin = min(a.margin, diff);
    if (stored < rd) {
        precise float sum = a.blockerSum + stored;
        a.blockerSum = sum;
        a.blockers = a.blockers + 1.0;
    }
}

int fuse_vsmr_blocker_offset(uint k, uint radius) {
    const int i = int(k) - 2;
    const int m = int((uint(i < 0 ? -i : i) * radius) / 2u);
    return i < 0 ? -m : m;
}

uint fuse_vsmr_penumbra_radius(float texels, uint cap) {
    precise float w = texels > 0.0 ? texels + 0.5 : 0.0;
    const float capped = w < float(cap) ? w : float(cap);
    return uint(floor(capped));
}

// Directional tap: depth bits of window texel p of a level when its page is mapped.
bool fuse_vsmr_dir_fetch(FuseVsmConstantsRef C, uint level, ivec2 p, out uint bits) {
    bits = 0u;
    if (p.x < 0 || p.y < 0 || p.x >= FUSE_VSMR_WINDOW_TEXELS || p.y >= FUSE_VSMR_WINDOW_TEXELS) {
        return false;
    }
    const FuseVsmLevel L = C.level[level];
    const int half_ = int(FUSE_VSM_PAGES_PER_AXIS / 2u);
    const int ax = (L.originX - half_) + p.x / int(FUSE_VSM_PAGE_TEXELS);
    const int ay = (L.originY - half_) + p.y / int(FUSE_VSM_PAGE_TEXELS);
    const uint v = level * FUSE_VSM_PAGES_PER_LEVEL + fuse_vsm_slot(ay) * FUSE_VSM_PAGES_PER_AXIS + fuse_vsm_slot(ax);
    const uint pte = FuseVsmrWordsRef(C.pageTable).v[v];
    if ((pte & FUSE_VSM_PTE_MAPPED) == 0u) {
        return false;
    }
    const uint phys = pte & FUSE_VSM_PTE_PHYS_MASK;
    const uint x = (phys % C.poolPagesX) * FUSE_VSM_PAGE_TEXELS + uint(p.x) % FUSE_VSM_PAGE_TEXELS;
    const uint y = (phys / C.poolPagesX) * FUSE_VSM_PAGE_TEXELS + uint(p.y) % FUSE_VSM_PAGE_TEXELS;
    bits = fuse_vsmr_load(C.poolHandle, ivec2(x, y));
    return true;
}

// vsm_raster_kernel.hpp SampleResult.
struct FuseVsmrSample {
    float visibility;
    int level;
    float receiverDepth;
    float margin;
};

FuseVsmrSample fuse_vsmr_sample() {
    FuseVsmrSample r;
    r.visibility = 1.0;
    r.level = -1;
    r.receiverDepth = 0.0;
    r.margin = 3.0e38;
    return r;
}

FuseVsmrSample fuse_vsmr_dir_visibility(FuseVsmConstantsRef C, FuseVsmShadowRef S, vec3 pos, vec3 n, int forceLevel) {
    FuseVsmrSample r = fuse_vsmr_sample();
    const vec3 lp = fuse_vsmr_light_point(C, pos);
    if (!(abs(lp.x) <= FUSE_VSM_MAX_LIGHT_COORD) || !(abs(lp.y) <= FUSE_VSM_MAX_LIGHT_COORD) ||
        !(abs(lp.z) <= FUSE_VSM_MAX_LIGHT_COORD)) {
        return r;
    }
    int first = forceLevel;
    if (first < 0) {
        precise float dx = abs(lp.x - C.cameraLight[0]);
        precise float dy = abs(lp.y - C.cameraLight[1]);
        precise float dz = abs(lp.z - C.cameraLight[2]);
        float d = dx > dy ? dx : dy;
        d = d > dz ? d : dz;
        precise float qc = d * C.levelSelectScale;
        precise float qd = d * C.densityScale;
        first = max(fuse_vsm_level_of(qc), fuse_vsm_level_of(qd) + C.lodBias);
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
        const FuseVsmLevel L = C.level[lev];
        precise float texel = L.pageWorld * (1.0 / float(FUSE_VSM_PAGE_TEXELS));
        precise float off = S.normalOffset * texel;
        precise vec3 bp = vec3(pos.x + n.x * off, pos.y + n.y * off, pos.z + n.z * off);
        const vec3 bl = fuse_vsmr_light_point(C, bp);
        precise float u = bl.x * L.invPageWorld;
        precise float v = bl.y * L.invPageWorld;
        if (!(abs(u) < FUSE_VSM_MAX_PAGE_COORD) || !(abs(v) < FUSE_VSM_MAX_PAGE_COORD)) {
            return r;
        }
        const int half_ = int(FUSE_VSM_PAGES_PER_AXIS / 2u);
        precise float tu = (u - float(L.originX - half_)) * float(FUSE_VSM_PAGE_TEXELS);
        precise float tv = (v - float(L.originY - half_)) * float(FUSE_VSM_PAGE_TEXELS);
        const float limit = float(FUSE_VSMR_WINDOW_TEXELS);
        uint bits = 0u;
        const bool inside = tu >= 0.0 && tu < limit && tv >= 0.0 && tv < limit;
        const int ix = inside ? int(floor(tu)) : -1;
        const int iy = inside ? int(floor(tv)) : -1;
        if (!inside || !fuse_vsmr_dir_fetch(C, uint(lev), ivec2(ix, iy), bits)) {
            if (forceLevel >= 0) {
                r.visibility = -1.0;
                r.level = lev;
                return r;
            }
            continue;
        }
        precise float rd = fuse_vsmr_dir_depth(L, bl.z) - S.depthBias * FUSE_VSMR_DEPTH_PER_TEXEL;
        r.level = lev;
        r.receiverDepth = rd;
        FuseVsmrTaps a = fuse_vsmr_taps();
        int radius = 0;
        if (S.filterMode == FUSE_VSMR_FILTER_PCF) {
            radius = int(min(S.pcfRadius, FUSE_VSMR_MAX_RADIUS));
        } else if (S.filterMode == FUSE_VSMR_FILTER_PCSS) {
            const uint search = min(S.pcssMaxRadius, FUSE_VSMR_MAX_RADIUS);
            for (uint j = 0u; j < FUSE_VSMR_BLOCKER_GRID; ++j) {
                for (uint i = 0u; i < FUSE_VSMR_BLOCKER_GRID; ++i) {
                    uint b = 0u;
                    const ivec2 q = ivec2(ix + fuse_vsmr_blocker_offset(i, search), iy + fuse_vsmr_blocker_offset(j, search));
                    if (fuse_vsmr_dir_fetch(C, uint(lev), q, b)) {
                        fuse_vsmr_blocker(a, rd, b);
                    }
                }
            }
            if (!(a.blockers > 0.0)) {
                r.visibility = 1.0;
                r.margin = a.margin;
                return r;
            }
            precise float avg = a.blockerSum / a.blockers;
            precise float texels = ((rd - avg) * (1.0 / FUSE_VSMR_DEPTH_PER_TEXEL)) * S.sunTanAngle;
            radius = int(fuse_vsmr_penumbra_radius(texels, search));
        }
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                uint b = 0u;
                if (fuse_vsmr_dir_fetch(C, uint(lev), ivec2(ix + dx, iy + dy), b)) {
                    fuse_vsmr_compare(a, rd, b);
                }
            }
        }
        precise float vis = a.lit / a.taps;
        r.visibility = vis;
        r.margin = a.margin;
        return r;
    }
    return r;
}

uint fuse_vsmr_local_fetch(FuseVsmShadowRef S, uint page, int ix, int iy) {
    const int top = 127;
    const uint x = uint(ix < 0 ? 0 : (ix > top ? top : ix));
    const uint y = uint(iy < 0 ? 0 : (iy > top ? top : iy));
    const uint px = (page % S.localPagesX) * 128u + x;
    const uint py = (page / S.localPagesX) * 128u + y;
    return fuse_vsmr_load(S.localPool, ivec2(px, py));
}

FuseVsmrSample fuse_vsmr_local_visibility(FuseVsmShadowRef S, FuseVsmLocal e, vec3 pos, vec3 n) {
    FuseVsmrSample r = fuse_vsmr_sample();
    vec3 f, rt, up;
    precise vec3 rel0 = vec3(pos.x - e.position[0], pos.y - e.position[1], pos.z - e.position[2]);
    uint face = e.type == FUSE_VSMR_LOCAL_POINT ? fuse_vsmr_cube_face(rel0) : 0u;
    fuse_vsmr_local_basis(e, face, f, rt, up);
    vec3 v = fuse_vsmr_local_view(e, f, rt, up, pos);
    if (!(v.z > e.nearPlane)) {
        return r;
    }
    precise float texel = (v.z * (1.0 / 64.0)) / e.invTanHalf;
    precise float off = S.normalOffset * texel;
    precise vec3 bp = vec3(pos.x + n.x * off, pos.y + n.y * off, pos.z + n.z * off);
    if (e.type == FUSE_VSMR_LOCAL_POINT) {
        precise vec3 rel = vec3(bp.x - e.position[0], bp.y - e.position[1], bp.z - e.position[2]);
        face = fuse_vsmr_cube_face(rel);
        fuse_vsmr_local_basis(e, face, f, rt, up);
    }
    v = fuse_vsmr_local_view(e, f, rt, up, bp);
    if (!(v.z > e.nearPlane)) {
        return r;
    }
    vec3 t = fuse_vsmr_local_project(e, v);
    const float limit = 128.0;
    if (!(t.x >= 0.0 && t.x < limit && t.y >= 0.0 && t.y < limit)) {
        if (e.type != FUSE_VSMR_LOCAL_POINT) {
            return r;
        }
        t.x = t.x < 0.0 ? 0.0 : (t.x < limit ? t.x : limit - 1.0);
        t.y = t.y < 0.0 ? 0.0 : (t.y < limit ? t.y : limit - 1.0);
    }
    const int ix = int(floor(t.x));
    const int iy = int(floor(t.y));
    const uint page = e.page[face < 6u ? face : 0u];
    precise float rd = v.z * e.invRange - (S.depthBias * texel) * e.invRange;
    r.receiverDepth = rd;
    FuseVsmrTaps a = fuse_vsmr_taps();
    int radius = 0;
    if (S.filterMode == FUSE_VSMR_FILTER_PCF) {
        radius = int(min(S.pcfRadius, FUSE_VSMR_MAX_RADIUS));
    } else if (S.filterMode == FUSE_VSMR_FILTER_PCSS) {
        const uint search = min(S.pcssMaxRadius, FUSE_VSMR_MAX_RADIUS);
        for (uint j = 0u; j < FUSE_VSMR_BLOCKER_GRID; ++j) {
            for (uint i = 0u; i < FUSE_VSMR_BLOCKER_GRID; ++i) {
                fuse_vsmr_blocker(a, rd, fuse_vsmr_local_fetch(S, page, ix + fuse_vsmr_blocker_offset(i, search),
                                                               iy + fuse_vsmr_blocker_offset(j, search)));
            }
        }
        if (!(a.blockers > 0.0)) {
            r.margin = a.margin;
            return r;
        }
        precise float zb = (a.blockerSum / a.blockers) * e.range;
        const float zr = v.z;
        precise float world = ((zr - zb) / max(zb, e.nearPlane)) * e.lightSize;
        precise float texels = ((world * e.invTanHalf) * 64.0) / zr;
        radius = int(fuse_vsmr_penumbra_radius(texels, search));
    }
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            fuse_vsmr_compare(a, rd, fuse_vsmr_local_fetch(S, page, ix + dx, iy + dy));
        }
    }
    precise float vis = a.lit / a.taps;
    r.visibility = vis;
    r.margin = a.margin;
    return r;
}

// Visibility of light `slot` at a world point (1: no shadows, or no shadow for this light).
FuseVsmrSample fuse_vsmr_shadow_sample(uint64_t address, uint slot, vec3 pos, vec3 n, int forceLevel) {
    if (address == 0ul) {
        return fuse_vsmr_sample();
    }
    FuseVsmShadowRef S = FuseVsmShadowRef(address);
    if (slot == S.directionalSlot && S.vsm != 0ul) {
        return fuse_vsmr_dir_visibility(FuseVsmConstantsRef(S.vsm), S, pos, n, forceLevel);
    }
    const uint count = min(S.localCount, FUSE_VSMR_MAX_LOCAL);
    for (uint k = 0u; k < count; ++k) {
        if (S.local[k].slot == slot) {
            return fuse_vsmr_local_visibility(S, S.local[k], pos, n);
        }
    }
    return fuse_vsmr_sample();
}

float fuse_vsm_shadow(uint64_t address, uint slot, vec3 pos, vec3 n) {
    return fuse_vsmr_shadow_sample(address, slot, pos, n, -1).visibility;
}

#endif // FUSE_VSM_SHADOW_GLSL
