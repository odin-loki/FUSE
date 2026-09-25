// WP-3.2 "vsm.local_raster". Slang twin of vsm_local_raster.comp (see there).
#include "vsm_raster_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 group: SV_GroupID, uint3 lid: SV_GroupThreadID) {
    VsmShadow* S = fuseVsmrShadow();
    const uint tid = lid.x;
    const uint words = fuseVsmrIndex(S.workHandle);
    const uint key = fuse_vsmr_words[words][kFuseVsmrWordLocalList + group.x * 2u];
    const uint page = fuse_vsmr_words[words][kFuseVsmrWordLocalList + group.x * 2u + 1u];
    const uint light = key & 0xFFu;
    const uint face = (key >> 8u) & 0xFFu;
    if (tid == 0u) {
        uint previous;
        InterlockedAdd(fuse_vsmr_words[words][kFuseVsmrWordLocalPages], 1u, previous);
    }
    if (light >= min(S.localCount, kFuseVsmrMaxLocal) || S.localPagesX == 0u) {
        return;
    }
    const VsmLocal e = S.local[light];
    float3 f;
    float3 r;
    float3 u;
    fuseVsmrLocalBasis(e, face, f, r, u);
    const int2 origin = int2(int((page % S.localPagesX) * 128u), int((page / S.localPagesX) * 128u));
    const uint image = fuseVsmrIndex(S.localPool);
    GpuSceneHeader* scene = fuseGpuScene(S.scene);
    GpuInstance* instances = fuseGpuSceneInstances(scene);
    GpuTransform* transforms = fuseGpuSceneTransforms(scene);
    GpuMesh* meshes = fuseGpuSceneMeshes(scene);
    const uint meshCount = scene.counts[kFuseGpuSceneMeshes];
    const uint instanceCount = S.instanceCount;
    const float3 lightPos = float3(e.position[0], e.position[1], e.position[2]);
    uint tris = 0u;
    for (uint i = 0u; i < instanceCount; ++i) {
        const GpuInstance inst = instances[i];
        if (inst.mesh >= meshCount) {
            continue;
        }
        const GpuMesh mesh = meshes[inst.mesh];
        const GpuTransform xf = transforms[i];
        float3 c;
        float3 x;
        if (!fuseVsmrCasterBounds(inst, mesh, xf, c, x) || !fuseVsmrBoxSphere(c, x, lightPos, e.range)) {
            continue;
        }
        const uint triangles = mesh.indexCount / 3u;
        for (uint t = tid; t < triangles; t += kFuseVsmrThreads) {
            float3 w[3];
            if (!fuseVsmrTriangleWorld(scene, mesh, xf, t, w)) {
                continue;
            }
            float3 v[3];
            for (uint j = 0u; j < 3u; ++j) {
                v[j] = fuseVsmrLocalView(e, f, r, u, w[j]);
            }
            if (v[0].z > e.range && v[1].z > e.range && v[2].z > e.range) {
                continue;
            }
            VsmrTri clipped[2];
            const uint n = fuseVsmrClipProject(e, v, clipped);
            for (uint j = 0u; j < n; ++j) {
                VsmrSetup s;
                if (!fuseVsmrSetup(clipped[j], s)) {
                    continue;
                }
                ++tris;
                for (int py = s.box.y; py <= s.box.w; ++py) {
                    for (int px = s.box.x; px <= s.box.z; ++px) {
                        fuseVsmrWriteLocal(image, origin, clipped[j], s, px, py, e);
                    }
                }
            }
        }
    }
    if (tris != 0u) {
        uint previous;
        InterlockedAdd(fuse_vsmr_words[words][kFuseVsmrWordLocalTriangles], tris, previous);
    }
}
