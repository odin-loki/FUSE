// FUSE compute software rasteriser (WP-5.4): shader side of
// include/fuse/renderer/swraster/swraster_types.hpp and the GLSL twin of swraster_kernel.hpp (the CPU
// reference: same operations in the same order; the only float math is the WP-1.4 vertex transform,
// `precise`). Keep in sync with swraster_common.slang. fuse_rp_swraster checks GPU == CPU bit for bit.
//
// Include after bindless.glsl, gpu_scene.glsl, vis_format.glsl and vis_common.glsl.
#ifndef FUSE_SWRASTER_COMMON_GLSL
#define FUSE_SWRASTER_COMMON_GLSL

#define FUSE_SW_SUBPIXEL_BITS 8
#define FUSE_SW_SUBPIXEL_ONE 256
#define FUSE_SW_SUBPIXEL_HALF 128
#define FUSE_SW_GROUP_SIZE 32u
#define FUSE_SW_RASTER_THREADS 64u
#define FUSE_SW_MAX_VERTICES 64u
#define FUSE_SW_MAX_TRIANGLES 124u
#define FUSE_SW_MAX_SPAN_SUBPIXELS 12288
#define FUSE_SW_COORD_LIMIT 1073741824ul

#define FUSE_SW_RESULT_NONE 0u
#define FUSE_SW_RESULT_CULLED 1u
#define FUSE_SW_RESULT_SOFTWARE 2u
#define FUSE_SW_RESULT_HARDWARE_SIZE 3u
#define FUSE_SW_RESULT_HARDWARE_EXTENT 4u
#define FUSE_SW_RESULT_HARDWARE_CLIP 5u
#define FUSE_SW_RESULT_HARDWARE_FORCED 6u
#define FUSE_SW_RESULT_OVERSIZE 7u

#define FUSE_SW_MODE_CLASSIFY 0u
#define FUSE_SW_MODE_FORCE_SOFTWARE 1u
#define FUSE_SW_MODE_FORCE_HARDWARE 2u

#define FUSE_SW_FLAG_WRITE_RESULTS 1u
#define FUSE_SW_FLAG_SKIP_SOFTWARE 2u
#define FUSE_SW_FLAG_SKIP_HARDWARE 4u

#define FUSE_SW_COUNT_CLASSIFY0 0u
#define FUSE_SW_COUNT_SOFTWARE0 6u
#define FUSE_SW_COUNT_HARDWARE0 12u
#define FUSE_SW_COUNT_GROUPS_REQUESTED0 20u
#define FUSE_SW_COUNT_SW_REQUESTED0 22u
#define FUSE_SW_COUNT_HW_REQUESTED0 24u
#define FUSE_SW_COUNT_OVERFLOW 26u
#define FUSE_SW_COUNT_SKIPPED 27u
#define FUSE_SW_COUNT_DEMOTED 28u
#define FUSE_SW_COUNT_OVERSIZE 29u
#define FUSE_SW_COUNT_SW_TRIANGLES 30u

// WP-1.3 CullCount words the expand pass reads.
#define FUSE_SW_CULL_PHASE1_DRAWS 0u
#define FUSE_SW_CULL_PHASE2_DRAWS 1u

// SwRasterConstants (144 bytes).
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseSwConstantsRef {
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

void fuse_sw_view_proj(FuseSwConstantsRef C, out vec4 vp[4]) {
    for (uint c = 0u; c < 4u; ++c) {
        vp[c] = vec4(C.viewProj[c * 4u], C.viewProj[c * 4u + 1u], C.viewProj[c * 4u + 2u], C.viewProj[c * 4u + 3u]);
    }
}

// round(num * scale / den) of the exact quotient (swraster_kernel.hpp ratio_round).
bool fuse_sw_ratio_round(float num, float den, uint scale, out int64_t outValue) {
    outValue = 0l;
    const uint nb = floatBitsToUint(num);
    const uint db = floatBitsToUint(den);
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
    uint64_t numer = mn * uint64_t(scale) * 2ul;
    uint64_t denom = md;
    if (k >= 0) {
        if (k > 15) {
            return false;
        }
        numer = numer << uint(k);
    } else {
        if (-k > 38) {
            return true;
        }
        denom = denom << uint(-k);
    }
    const bool negative = (nb >> 31u) != 0u;
    const uint64_t q = negative ? (numer + denom - 1ul) / (denom * 2ul) : (numer + denom) / (denom * 2ul);
    if (q > FUSE_SW_COORD_LIMIT) {
        return false;
    }
    outValue = negative ? -int64_t(q) : int64_t(q);
    return true;
}

// floor(z / w * 2^32) clamped (swraster_kernel.hpp ratio_floor_z).
uint fuse_sw_ratio_floor_z(float z, float w) {
    const uint zb = floatBitsToUint(z);
    const uint wb = floatBitsToUint(w);
    if ((zb & 0x7FFFFFFFu) == 0u) {
        return 0u;
    }
    const uint ze = (zb >> 23u) & 0xFFu;
    const uint64_t mz = ze == 0u ? uint64_t(zb & 0x7FFFFFu) : uint64_t((zb & 0x7FFFFFu) | 0x800000u);
    const int ez = ze == 0u ? -149 : int(ze) - 150;
    const uint64_t mw = uint64_t((wb & 0x7FFFFFu) | 0x800000u);
    const int ew = int((wb >> 23u) & 0xFFu) - 150;
    const int k = ez - ew + 32;
    uint64_t q = 0ul;
    if (k >= 0) {
        q = (mz << uint(k)) / mw;
    } else if (-k < 24) {
        q = mz / (mw << uint(-k));
    }
    return q > 0xFFFFFFFFul ? 0xFFFFFFFFu : uint(q);
}

// SwVertex: (x, y) sub-pixels, z = floor(z / w * 2^32), valid (swraster_kernel.hpp project_vertex).
struct FuseSwVertex {
    int x;
    int y;
    uint z;
    uint valid;
};

FuseSwVertex fuse_sw_project(vec4 c, uint width, uint height) {
    FuseSwVertex v;
    v.x = 0;
    v.y = 0;
    v.z = 0u;
    v.valid = 0u;
    if (!(c.w > 0.0) || !(c.z >= 0.0) || !(c.z <= c.w)) {
        return v;
    }
    int64_t rx;
    int64_t ry;
    if (!fuse_sw_ratio_round(c.x, c.w, width << 7u, rx) || !fuse_sw_ratio_round(c.y, c.w, height << 7u, ry)) {
        return v;
    }
    v.x = int(rx + int64_t(width << 7u));
    v.y = int(ry + int64_t(height << 7u));
    v.z = fuse_sw_ratio_floor_z(c.z, c.w);
    v.valid = 1u;
    return v;
}

int fuse_sw_edge_bias(int dx, int dy) { return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1; }
int fuse_sw_first_centre(int v) { return (v + (FUSE_SW_SUBPIXEL_HALF - 1)) >> FUSE_SW_SUBPIXEL_BITS; }
int fuse_sw_last_centre(int v) { return (v - FUSE_SW_SUBPIXEL_HALF) >> FUSE_SW_SUBPIXEL_BITS; }

uint64_t fuse_sw_pack(uint depth24, uint instance, uint triangle) {
    return (uint64_t(depth24) << FUSE_VIS64_DEPTH_SHIFT) | (uint64_t(instance & FUSE_VIS64_ID_MASK) << FUSE_VIS64_INSTANCE_SHIFT) |
           uint64_t(triangle & FUSE_VIS64_ID_MASK);
}

uint fuse_sw_meshlet_vertices(FuseGpuMeshlet m) { return m.counts & 0xFFu; }
uint fuse_sw_meshlet_triangles(FuseGpuMeshlet m) { return (m.counts >> 8u) & 0xFFu; }

// SwResult of one meshlet (swraster_kernel.hpp classify_cluster).
uint fuse_sw_classify(FuseGpuMeshlet m, FuseGpuTransform xf, FuseSwConstantsRef C) {
    const uint vertices = fuse_sw_meshlet_vertices(m);
    const uint triangles = fuse_sw_meshlet_triangles(m);
    if (vertices > FUSE_SW_MAX_VERTICES || triangles > FUSE_SW_MAX_TRIANGLES) {
        return FUSE_SW_RESULT_OVERSIZE;
    }
    vec4 vp[4];
    fuse_sw_view_proj(C, vp);
    vec4 corner[8];
    uint allOut = 0x3Fu;
    bool clip = false;
    for (uint k = 0u; k < 8u; ++k) {
        vec3 p;
        p.x = (k & 1u) != 0u ? m.aabbMax[0] : m.aabbMin[0];
        p.y = (k & 2u) != 0u ? m.aabbMax[1] : m.aabbMin[1];
        p.z = (k & 4u) != 0u ? m.aabbMax[2] : m.aabbMin[2];
        const vec4 q = fuse_vis_clip(xf, vp, p);
        corner[k] = q;
        const uint outCode = (q.x < -q.w ? 1u : 0u) | (q.x > q.w ? 2u : 0u) | (q.y < -q.w ? 4u : 0u) | (q.y > q.w ? 8u : 0u) |
                             (q.z < 0.0 ? 16u : 0u) | (q.z > q.w ? 32u : 0u);
        allOut &= outCode;
        if (!(q.w > 0.0) || !(q.z >= 0.0) || !(q.z <= q.w)) {
            clip = true;
        }
    }
    if (allOut != 0u) {
        return FUSE_SW_RESULT_CULLED;
    }
    if (C.mode == FUSE_SW_MODE_FORCE_HARDWARE) {
        return FUSE_SW_RESULT_HARDWARE_FORCED;
    }
    if (clip) {
        return FUSE_SW_RESULT_HARDWARE_CLIP;
    }
    int minX = 0x7FFFFFFF;
    int minY = 0x7FFFFFFF;
    int maxX = -0x7FFFFFFF;
    int maxY = -0x7FFFFFFF;
    for (uint k = 0u; k < 8u; ++k) {
        const FuseSwVertex v = fuse_sw_project(corner[k], C.width, C.height);
        if (v.valid == 0u) {
            return FUSE_SW_RESULT_HARDWARE_CLIP;
        }
        minX = v.x < minX ? v.x : minX;
        maxX = v.x > maxX ? v.x : maxX;
        minY = v.y < minY ? v.y : minY;
        maxY = v.y > maxY ? v.y : maxY;
    }
    const int lastX = int(C.width - 1u) * FUSE_SW_SUBPIXEL_ONE + FUSE_SW_SUBPIXEL_HALF;
    const int lastY = int(C.height - 1u) * FUSE_SW_SUBPIXEL_ONE + FUSE_SW_SUBPIXEL_HALF;
    if (maxX + FUSE_SW_SUBPIXEL_ONE < FUSE_SW_SUBPIXEL_HALF || maxY + FUSE_SW_SUBPIXEL_ONE < FUSE_SW_SUBPIXEL_HALF ||
        minX - FUSE_SW_SUBPIXEL_ONE > lastX || minY - FUSE_SW_SUBPIXEL_ONE > lastY) {
        return FUSE_SW_RESULT_CULLED;
    }
    const uint rectW = uint(maxX - minX);
    const uint rectH = uint(maxY - minY);
    if (rectW > C.maxClusterExtent || rectH > C.maxClusterExtent) {
        return FUSE_SW_RESULT_HARDWARE_EXTENT;
    }
    if (C.mode == FUSE_SW_MODE_FORCE_SOFTWARE) {
        return FUSE_SW_RESULT_SOFTWARE;
    }
    const uint64_t thr = uint64_t(C.triangleThreshold);
    return uint64_t(rectW) * uint64_t(rectH) <= thr * thr * uint64_t(triangles) ? FUSE_SW_RESULT_SOFTWARE
                                                                                 : FUSE_SW_RESULT_HARDWARE_SIZE;
}

#endif // FUSE_SWRASTER_COMMON_GLSL
