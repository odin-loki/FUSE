// WP-3.2 "vsm.raster". Slang twin of vsm_raster.comp (see there).
#include "vsm_raster_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 group: SV_GroupID, uint3 lid: SV_GroupThreadID) {
    VsmShadow* S = fuseVsmrShadow();
    VsmConstants* C = (VsmConstants*)S.vsm;
    const uint tid = lid.x;
    const uint entry = group.x;
    uint* vsmWork = (uint*)C.work;
    const uint vp = vsmWork[C.offRenderList + entry * 2u];
    const uint phys = vsmWork[C.offRenderList + entry * 2u + 1u];
    uint level;
    int ax;
    int ay;
    fuseVsmrDirPageOf(C, vp, level, ax, ay);
    const uint words = fuseVsmrIndex(S.workHandle);
    if (tid == 0u) {
        uint previous;
        InterlockedAdd(fuse_vsmr_words[words][kFuseVsmrWordDirPages], 1u, previous);
    }
    if (level >= C.levels || level >= 16u || phys >= C.physPages) {
        return;
    }
    const VsmLevel L = C.level[level];
    const int2 origin = int2(int((phys % C.poolPagesX) * kFuseVsmPageTexels), int((phys / C.poolPagesX) * kFuseVsmPageTexels));
    const uint pool = fuseVsmrIndex(C.poolHandle);
    GpuSceneHeader* scene = fuseGpuScene(C.scene);
    GpuInstance* instances = fuseGpuSceneInstances(scene);
    GpuTransform* transforms = fuseGpuSceneTransforms(scene);
    GpuMesh* meshes = fuseGpuSceneMeshes(scene);
    const uint meshCount = scene.counts[kFuseGpuSceneMeshes];
    const uint instanceCount = C.instanceCount;
    const float margin = 1.0 / 64.0;
    const float fax = float(ax);
    const float fay = float(ay);
    uint tris = 0u;
    uint big = 0u;
    for (uint i = 0u; i < instanceCount; ++i) {
        const GpuInstance inst = instances[i];
        if (inst.mesh >= meshCount) {
            continue;
        }
        const GpuMesh mesh = meshes[inst.mesh];
        const GpuTransform xf = transforms[i];
        float3 c;
        float3 e;
        if (!fuseVsmrCasterBounds(inst, mesh, xf, c, e)) {
            continue;
        }
        const float3 lc = fuseVsmrLightPoint(C, c);
        const float lex = (abs(C.lightRotation[0]) * e.x + abs(C.lightRotation[1]) * e.y) + abs(C.lightRotation[2]) * e.z;
        const float ley = (abs(C.lightRotation[4]) * e.x + abs(C.lightRotation[5]) * e.y) + abs(C.lightRotation[6]) * e.z;
        const float lez = (abs(C.lightRotation[8]) * e.x + abs(C.lightRotation[9]) * e.y) + abs(C.lightRotation[10]) * e.z;
        const float u0 = (lc.x - lex) * L.invPageWorld;
        const float u1 = (lc.x + lex) * L.invPageWorld;
        const float v0 = (lc.y - ley) * L.invPageWorld;
        const float v1 = (lc.y + ley) * L.invPageWorld;
        const float nearest = fuseVsmrDirDepth(L, lc.z + lez);
        if (!(u1 >= fax - margin && u0 <= fax + 1.0 + margin && v1 >= fay - margin && v0 <= fay + 1.0 + margin && nearest <= 1.001)) {
            continue;
        }
        const uint triangles = mesh.indexCount / 3u;
        for (uint t = tid; t < triangles; t += kFuseVsmrThreads) {
            float3 w[3];
            if (!fuseVsmrTriangleWorld(scene, mesh, xf, t, w)) {
                continue;
            }
            VsmrTri tri;
            tri.h = 0.0;
            for (uint j = 0u; j < 3u; ++j) {
                const float3 pv = fuseVsmrDirPageVertex(L, ax, ay, fuseVsmrLightPoint(C, w[j]));
                tri.x[j] = pv.x;
                tri.y[j] = pv.y;
                tri.z[j] = pv.z;
            }
            VsmrSetup s;
            if ((tri.z[0] > 1.0 && tri.z[1] > 1.0 && tri.z[2] > 1.0) || !fuseVsmrSetup(tri, s)) {
                continue;
            }
            ++tris;
            big += (s.box.z - s.box.x + 1) * (s.box.w - s.box.y + 1) > kFuseVsmrBigTexels ? 1u : 0u;
            for (int py = s.box.y; py <= s.box.w; ++py) {
                for (int px = s.box.x; px <= s.box.z; ++px) {
                    fuseVsmrWriteDir(pool, origin, tri, s, px, py);
                }
            }
        }
    }
    if (tris != 0u) {
        uint previous;
        InterlockedAdd(fuse_vsmr_words[words][kFuseVsmrWordDirTriangles], tris, previous);
        InterlockedAdd(fuse_vsmr_words[words][kFuseVsmrWordDirBig], big, previous);
    }
}
