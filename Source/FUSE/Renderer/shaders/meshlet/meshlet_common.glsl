// FUSE mesh-shader path (WP-5.1): shader side of include/fuse/renderer/meshlet/meshlet_types.hpp
// and the GLSL twin of include/fuse/renderer/meshlet/meshlet_cull_kernel.hpp (same operations in the
// same order, `precise` so nothing is contracted into an FMA). Keep in sync with meshlet_common.slang.
//
// Include after bindless.glsl, gpu_scene.glsl and (shaders/culling/) cull_common.glsl.
#ifndef FUSE_MESHLET_COMMON_GLSL
#define FUSE_MESHLET_COMMON_GLSL

#define FUSE_MESHLET_TASK_GROUP 32u
#define FUSE_MESHLET_MESH_THREADS 64u
#define FUSE_MESHLET_MAX_VERTICES 64u
#define FUSE_MESHLET_MAX_TRIANGLES 124u
#define FUSE_MESHLET_EXPAND_WORKGROUP 64u

#define FUSE_MESHLET_CULL_FRUSTUM 1u
#define FUSE_MESHLET_CULL_CONE 2u
#define FUSE_MESHLET_CULL_OCCLUSION 4u
#define FUSE_MESHLET_WRITE_RESULTS 8u

#define FUSE_MESHLET_MODE_EARLY 0u
#define FUSE_MESHLET_MODE_LATE 1u
#define FUSE_MESHLET_MODE_PHASE2 2u

#define FUSE_MESHLET_RESULT_NONE 0u
#define FUSE_MESHLET_RESULT_FRUSTUM_CULLED 1u
#define FUSE_MESHLET_RESULT_CONE_CULLED 2u
#define FUSE_MESHLET_RESULT_PHASE1_DRAWN 3u
#define FUSE_MESHLET_RESULT_DEFERRED 4u
#define FUSE_MESHLET_RESULT_PHASE2_DRAWN 5u
#define FUSE_MESHLET_RESULT_OCCLUDED 6u
#define FUSE_MESHLET_RESULT_OVERSIZE 7u

#define FUSE_MESHLET_COUNT_TASKS0 0u
#define FUSE_MESHLET_COUNT_TASKS1 3u
#define FUSE_MESHLET_COUNT_REQUESTED0 6u
#define FUSE_MESHLET_COUNT_OVERFLOW 8u
#define FUSE_MESHLET_COUNT_SKIPPED 9u
#define FUSE_MESHLET_COUNT_OVERSIZE 10u

// MeshletConstants = CullConstants (352 bytes, FuseCullConstantsRef at the same address) + this tail.
#define FUSE_MESHLET_TAIL_OFFSET 352ul

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseMeshletTailRef {
    float camera[3];
    uint flags;
    uint groupsBuffer;
    uint masksBuffer;
    uint countsBuffer;
    uint resultsBuffer;
    uint groupCapacity;
    uint scene;
    uint reserved[2];
};

// Task -> mesh payload: the instance and the surviving meshlets, compacted in meshlet order.
struct FuseMeshletPayload {
    uint instance;
    uint meshlets[FUSE_MESHLET_TASK_GROUP];
};

float fuse_meshlet_abs(float v) { return v < 0.0 ? -v : v; }
float fuse_meshlet_max(float a, float b) { return a > b ? a : b; }

// meshlet_cull_kernel.hpp is_similarity (tolerance = kMeshletSimilarityTolerance, 1e-4f exactly).
bool fuse_meshlet_is_similarity(FuseGpuTransform t) {
    precise float g00 = t.rows[0].x * t.rows[0].x + t.rows[1].x * t.rows[1].x + t.rows[2].x * t.rows[2].x;
    precise float g11 = t.rows[0].y * t.rows[0].y + t.rows[1].y * t.rows[1].y + t.rows[2].y * t.rows[2].y;
    precise float g22 = t.rows[0].z * t.rows[0].z + t.rows[1].z * t.rows[1].z + t.rows[2].z * t.rows[2].z;
    precise float d01 = t.rows[0].x * t.rows[0].y + t.rows[1].x * t.rows[1].y + t.rows[2].x * t.rows[2].y;
    precise float d02 = t.rows[0].x * t.rows[0].z + t.rows[1].x * t.rows[1].z + t.rows[2].x * t.rows[2].z;
    precise float d12 = t.rows[0].y * t.rows[0].z + t.rows[1].y * t.rows[1].z + t.rows[2].y * t.rows[2].z;
    const float gmax = fuse_meshlet_max(g00, fuse_meshlet_max(g11, g22));
    precise float tol = gmax * uintBitsToFloat(0x38D1B717u);
    precise float e01 = g00 - g11;
    precise float e02 = g00 - g22;
    return gmax > 0.0 && fuse_meshlet_abs(e01) <= tol && fuse_meshlet_abs(e02) <= tol && fuse_meshlet_abs(d01) <= tol &&
           fuse_meshlet_abs(d02) <= tol && fuse_meshlet_abs(d12) <= tol;
}

// meshlet_cull_kernel.hpp WorldMeshlet (the cone fields are only read when `cone` is set).
struct FuseMeshletWorld {
    FuseCullSphere sphere;
    float apex[3];
    float axis[3];
    float cutoff;
    bool frustum;
    bool cone;
};

FuseMeshletWorld fuse_meshlet_world(FuseGpuTransform t, FuseGpuMeshlet m, uint flags) {
    FuseMeshletWorld w;
    FuseGpuMesh bounds;
    bounds.boundsCenter[0] = m.center[0];
    bounds.boundsCenter[1] = m.center[1];
    bounds.boundsCenter[2] = m.center[2];
    bounds.boundsRadius = m.radius;
    w.sphere = fuse_cull_world_sphere(t, bounds, 1.0);
    for (uint r = 0u; r < 3u; ++r) {
        precise float a = t.rows[r].x * m.coneApex[0] + t.rows[r].y * m.coneApex[1] + t.rows[r].z * m.coneApex[2] + t.rows[r].w;
        precise float x = t.rows[r].x * m.coneAxis[0] + t.rows[r].y * m.coneAxis[1] + t.rows[r].z * m.coneAxis[2];
        w.apex[r] = a;
        w.axis[r] = x;
    }
    precise float len2 = w.axis[0] * w.axis[0] + w.axis[1] * w.axis[1] + w.axis[2] * w.axis[2];
    precise float cutoff = m.coneCutoff * sqrt(len2);
    w.cutoff = cutoff;
    w.frustum = (flags & FUSE_MESHLET_CULL_FRUSTUM) != 0u;
    w.cone = (flags & FUSE_MESHLET_CULL_CONE) != 0u && m.coneCutoff < 1.0 && fuse_meshlet_is_similarity(t);
    return w;
}

// geometry::cull_kernel::cull_meshlet on the world record: NONE (passes), FRUSTUM_CULLED or CONE_CULLED.
uint fuse_meshlet_frustum_cone(FuseCullConstantsRef C, FuseMeshletTailRef M, FuseMeshletWorld w) {
    if (w.frustum && !fuse_cull_frustum_visible(C, w.sphere)) {
        return FUSE_MESHLET_RESULT_FRUSTUM_CULLED;
    }
    if (w.cone) {
        precise float d0 = w.apex[0] - M.camera[0];
        precise float d1 = w.apex[1] - M.camera[1];
        precise float d2 = w.apex[2] - M.camera[2];
        precise float lhs = d0 * w.axis[0] + d1 * w.axis[1] + d2 * w.axis[2];
        precise float dd = d0 * d0 + d1 * d1 + d2 * d2;
        precise float rhs = w.cutoff * sqrt(dd);
        if (lhs >= rhs) {
            return FUSE_MESHLET_RESULT_CONE_CULLED;
        }
    }
    return FUSE_MESHLET_RESULT_NONE;
}

bool fuse_meshlet_oversize(FuseGpuMeshlet m) {
    return (m.counts & 0xFFu) > FUSE_MESHLET_MAX_VERTICES || ((m.counts >> 8u) & 0xFFu) > FUSE_MESHLET_MAX_TRIANGLES;
}

bool fuse_meshlet_occlusion(FuseCullConstantsRef C, FuseMeshletTailRef M) {
    return (M.flags & FUSE_MESHLET_CULL_OCCLUSION) != 0u && (C.flags & FUSE_CULL_OCCLUSION) != 0u;
}

// meshlet_cull_kernel.hpp classify (mode EARLY or PHASE2).
uint fuse_meshlet_classify(FuseCullConstantsRef C, FuseMeshletTailRef M, uint mode, FuseGpuTransform xf,
                           FuseGpuTransform prevXf, FuseGpuMeshlet m) {
    if (fuse_meshlet_oversize(m)) {
        return FUSE_MESHLET_RESULT_OVERSIZE;
    }
    const FuseMeshletWorld w = fuse_meshlet_world(xf, m, M.flags);
    const uint fc = fuse_meshlet_frustum_cone(C, M, w);
    if (fc != FUSE_MESHLET_RESULT_NONE) {
        return fc;
    }
    if (mode == FUSE_MESHLET_MODE_EARLY) {
        if (!fuse_meshlet_occlusion(C, M)) {
            return FUSE_MESHLET_RESULT_PHASE1_DRAWN;
        }
        if ((C.flags & FUSE_CULL_HISTORY_VALID) == 0u) {
            return FUSE_MESHLET_RESULT_DEFERRED;
        }
        const FuseMeshletWorld prev = fuse_meshlet_world(prevXf, m, 0u);
        return fuse_cull_hiz_visible(C, 1u, prev.sphere) ? FUSE_MESHLET_RESULT_PHASE1_DRAWN : FUSE_MESHLET_RESULT_DEFERRED;
    }
    if (!fuse_meshlet_occlusion(C, M)) {
        return FUSE_MESHLET_RESULT_PHASE2_DRAWN;
    }
    return fuse_cull_hiz_visible(C, 0u, w.sphere) ? FUSE_MESHLET_RESULT_PHASE2_DRAWN : FUSE_MESHLET_RESULT_OCCLUDED;
}

// meshlet_cull_kernel.hpp classify_late.
uint fuse_meshlet_classify_late(FuseCullConstantsRef C, FuseGpuTransform xf, FuseGpuMeshlet m) {
    const FuseMeshletWorld w = fuse_meshlet_world(xf, m, 0u);
    return fuse_cull_hiz_visible(C, 0u, w.sphere) ? FUSE_MESHLET_RESULT_PHASE2_DRAWN : FUSE_MESHLET_RESULT_OCCLUDED;
}

#endif // FUSE_MESHLET_COMMON_GLSL
