// FUSE mesh-shader path (WP-5.1): Slang side of include/fuse/renderer/meshlet/meshlet_types.hpp and
// twin of meshlet_common.glsl / meshlet_cull_kernel.hpp (same operations in the same order; compile
// with -fp-mode precise, Slang does not honour `precise` per variable).
//
// #include after gpu_scene.slang and (shaders/culling/) cull_common.slang.
#pragma once

static const uint kFuseMeshletTaskGroup = 32u;
static const uint kFuseMeshletMeshThreads = 64u;
static const uint kFuseMeshletMaxVertices = 64u;
static const uint kFuseMeshletMaxTriangles = 124u;

static const uint kFuseMeshletCullFrustum = 1u;
static const uint kFuseMeshletCullCone = 2u;
static const uint kFuseMeshletCullOcclusion = 4u;
static const uint kFuseMeshletWriteResults = 8u;

static const uint kFuseMeshletModeEarly = 0u;
static const uint kFuseMeshletModeLate = 1u;
static const uint kFuseMeshletModePhase2 = 2u;

static const uint kFuseMeshletResultNone = 0u;
static const uint kFuseMeshletResultFrustumCulled = 1u;
static const uint kFuseMeshletResultConeCulled = 2u;
static const uint kFuseMeshletResultPhase1Drawn = 3u;
static const uint kFuseMeshletResultDeferred = 4u;
static const uint kFuseMeshletResultPhase2Drawn = 5u;
static const uint kFuseMeshletResultOccluded = 6u;
static const uint kFuseMeshletResultOversize = 7u;

static const uint kFuseMeshletCountTasks0 = 0u;
static const uint kFuseMeshletCountTasks1 = 3u;
static const uint kFuseMeshletCountRequested0 = 6u;
static const uint kFuseMeshletCountOverflow = 8u;
static const uint kFuseMeshletCountSkipped = 9u;
static const uint kFuseMeshletCountOversize = 10u;

// MeshletConstants = CullConstants (352 bytes) + this tail.
static const uint64_t kFuseMeshletTailOffset = 352ull;

struct MeshletTail {
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

struct MeshletPayload {
    uint instance;
    uint meshlets[32];
};

float fuseMeshletAbs(float v) { return v < 0.0 ? -v : v; }
float fuseMeshletMax(float a, float b) { return a > b ? a : b; }

bool fuseMeshletIsSimilarity(GpuTransform t) {
    const float g00 = t.rows[0].x * t.rows[0].x + t.rows[1].x * t.rows[1].x + t.rows[2].x * t.rows[2].x;
    const float g11 = t.rows[0].y * t.rows[0].y + t.rows[1].y * t.rows[1].y + t.rows[2].y * t.rows[2].y;
    const float g22 = t.rows[0].z * t.rows[0].z + t.rows[1].z * t.rows[1].z + t.rows[2].z * t.rows[2].z;
    const float d01 = t.rows[0].x * t.rows[0].y + t.rows[1].x * t.rows[1].y + t.rows[2].x * t.rows[2].y;
    const float d02 = t.rows[0].x * t.rows[0].z + t.rows[1].x * t.rows[1].z + t.rows[2].x * t.rows[2].z;
    const float d12 = t.rows[0].y * t.rows[0].z + t.rows[1].y * t.rows[1].z + t.rows[2].y * t.rows[2].z;
    const float gmax = fuseMeshletMax(g00, fuseMeshletMax(g11, g22));
    const float tol = gmax * asfloat(0x38D1B717u); // kMeshletSimilarityTolerance (1e-4f) exactly
    const float e01 = g00 - g11;
    const float e02 = g00 - g22;
    return gmax > 0.0 && fuseMeshletAbs(e01) <= tol && fuseMeshletAbs(e02) <= tol && fuseMeshletAbs(d01) <= tol &&
           fuseMeshletAbs(d02) <= tol && fuseMeshletAbs(d12) <= tol;
}

struct MeshletWorld {
    CullSphere sphere;
    float apex[3];
    float axis[3];
    float cutoff;
    bool frustum;
    bool cone;
};

MeshletWorld fuseMeshletWorld(GpuTransform t, GpuMeshlet m, uint flags) {
    MeshletWorld w;
    GpuMesh bounds = {};
    bounds.boundsCenter[0] = m.center[0];
    bounds.boundsCenter[1] = m.center[1];
    bounds.boundsCenter[2] = m.center[2];
    bounds.boundsRadius = m.radius;
    w.sphere = fuseCullWorldSphere(t, bounds, 1.0);
    for (uint r = 0u; r < 3u; ++r) {
        w.apex[r] = t.rows[r].x * m.coneApex[0] + t.rows[r].y * m.coneApex[1] + t.rows[r].z * m.coneApex[2] + t.rows[r].w;
        w.axis[r] = t.rows[r].x * m.coneAxis[0] + t.rows[r].y * m.coneAxis[1] + t.rows[r].z * m.coneAxis[2];
    }
    const float len2 = w.axis[0] * w.axis[0] + w.axis[1] * w.axis[1] + w.axis[2] * w.axis[2];
    w.cutoff = m.coneCutoff * sqrt(len2);
    w.frustum = (flags & kFuseMeshletCullFrustum) != 0u;
    w.cone = (flags & kFuseMeshletCullCone) != 0u && m.coneCutoff < 1.0 && fuseMeshletIsSimilarity(t);
    return w;
}

uint fuseMeshletFrustumCone(CullConstants* C, MeshletTail* M, MeshletWorld w) {
    if (w.frustum && !fuseCullFrustumVisible(C, w.sphere)) {
        return kFuseMeshletResultFrustumCulled;
    }
    if (w.cone) {
        const float d0 = w.apex[0] - M.camera[0];
        const float d1 = w.apex[1] - M.camera[1];
        const float d2 = w.apex[2] - M.camera[2];
        const float lhs = d0 * w.axis[0] + d1 * w.axis[1] + d2 * w.axis[2];
        const float dd = d0 * d0 + d1 * d1 + d2 * d2;
        const float rhs = w.cutoff * sqrt(dd);
        if (lhs >= rhs) {
            return kFuseMeshletResultConeCulled;
        }
    }
    return kFuseMeshletResultNone;
}

bool fuseMeshletOversize(GpuMeshlet m) {
    return (m.counts & 0xFFu) > kFuseMeshletMaxVertices || ((m.counts >> 8u) & 0xFFu) > kFuseMeshletMaxTriangles;
}

bool fuseMeshletOcclusion(CullConstants* C, MeshletTail* M) {
    return (M.flags & kFuseMeshletCullOcclusion) != 0u && (C.flags & kFuseCullOcclusion) != 0u;
}

uint fuseMeshletClassify(CullConstants* C, MeshletTail* M, uint mode, GpuTransform xf, GpuTransform prevXf, GpuMeshlet m) {
    if (fuseMeshletOversize(m)) {
        return kFuseMeshletResultOversize;
    }
    const MeshletWorld w = fuseMeshletWorld(xf, m, M.flags);
    const uint fc = fuseMeshletFrustumCone(C, M, w);
    if (fc != kFuseMeshletResultNone) {
        return fc;
    }
    if (mode == kFuseMeshletModeEarly) {
        if (!fuseMeshletOcclusion(C, M)) {
            return kFuseMeshletResultPhase1Drawn;
        }
        if ((C.flags & kFuseCullHistoryValid) == 0u) {
            return kFuseMeshletResultDeferred;
        }
        const MeshletWorld prev = fuseMeshletWorld(prevXf, m, 0u);
        return fuseCullHizVisible(C, 1u, prev.sphere) ? kFuseMeshletResultPhase1Drawn : kFuseMeshletResultDeferred;
    }
    if (!fuseMeshletOcclusion(C, M)) {
        return kFuseMeshletResultPhase2Drawn;
    }
    return fuseCullHizVisible(C, 0u, w.sphere) ? kFuseMeshletResultPhase2Drawn : kFuseMeshletResultOccluded;
}

uint fuseMeshletClassifyLate(CullConstants* C, GpuTransform xf, GpuMeshlet m) {
    const MeshletWorld w = fuseMeshletWorld(xf, m, 0u);
    return fuseCullHizVisible(C, 0u, w.sphere) ? kFuseMeshletResultPhase2Drawn : kFuseMeshletResultOccluded;
}
