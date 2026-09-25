// WP-1.3 instance cull (both phases), Slang twin of instance_cull.comp (see the GLSL file for the
// phase protocol). Same push layout, same outputs.
#include "gpu_scene.slang"
#include "cull_common.slang"

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_cull_words[];

struct Push {
    uint64_t constants;
    uint scene;
    uint phase;
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

void writeDraw(CullConstants* C, uint slot, GpuMesh mesh, uint instance) {
    const uint args = fuseHandleIndex(C.argsBuffer);
    const uint o = slot * 5u;
    // Draw range of the mesh in the scene index buffer (instance_cull_kernel.hpp make_draw).
    const bool ranged = mesh.indexCount != 0u;
    fuse_cull_words[args][o + 0u] = ranged ? mesh.indexCount : mesh.triangleCount * 3u;
    fuse_cull_words[args][o + 1u] = 1u;
    fuse_cull_words[args][o + 2u] = ranged ? mesh.firstIndex : 0u;
    fuse_cull_words[args][o + 3u] = ranged ? uint(mesh.vertexOffset) : 0u;
    fuse_cull_words[args][o + 4u] = instance;
}

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    CullConstants* C = (CullConstants*)pc.constants;
    const uint counts = fuseHandleIndex(C.countsBuffer);
    const uint results = fuseHandleIndex(C.resultsBuffer);
    const uint candidates = fuseHandleIndex(C.candidatesBuffer);
    uint i = tid.x;
    if (pc.phase == 1u) {
        if (i >= C.instanceCount) {
            return;
        }
    } else {
        if (i >= fuse_cull_words[counts][kFuseCullCountCandidates]) {
            return;
        }
        i = fuse_cull_words[candidates][i];
    }
    GpuSceneHeader* scene = fuseGpuScene(pc.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[i];
    const uint need = kFuseInstanceValid | kFuseInstanceVisible;
    if ((inst.flags & (need | kFuseInstanceTransparent)) != need || inst.mesh >= scene.counts[kFuseGpuSceneMeshes]) {
        fuse_cull_words[results][i] = kFuseCullResultNone;
        return;
    }
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuTransform xf = fuseGpuSceneTransforms(scene)[i];
    const CullSphere s = fuseCullWorldSphere(xf, mesh, 1.0);
    uint result;
    if (pc.phase == 1u) {
        if ((C.flags & kFuseCullFrustum) != 0u && !fuseCullFrustumVisible(C, s)) {
            result = kFuseCullResultFrustumCulled;
        } else if ((C.flags & kFuseCullOcclusion) == 0u) {
            result = kFuseCullResultPhase1Drawn;
        } else if ((C.flags & kFuseCullHistoryValid) == 0u) {
            result = kFuseCullResultCandidate;
        } else {
            const GpuTransform prevXf = fuseGpuScenePrevTransforms(scene)[i];
            const CullSphere prev = fuseCullWorldSphere(prevXf, mesh, 1.0);
            result = fuseCullHizVisible(C, 1u, prev) ? kFuseCullResultPhase1Drawn : kFuseCullResultCandidate;
        }
        fuse_cull_words[results][i] = result;
        if (result == kFuseCullResultPhase1Drawn) {
            uint slot;
            InterlockedAdd(fuse_cull_words[counts][kFuseCullCountPhase1Draws], 1u, slot);
            writeDraw(C, slot, mesh, i);
        } else if (result == kFuseCullResultCandidate) {
            uint c;
            InterlockedAdd(fuse_cull_words[counts][kFuseCullCountCandidates], 1u, c);
            fuse_cull_words[candidates][c] = i;
            InterlockedMax(fuse_cull_words[counts][kFuseCullCountDispatchX], c / 64u + 1u);
        }
        return;
    }
    result = fuseCullHizVisible(C, 0u, s) ? kFuseCullResultPhase2Drawn : kFuseCullResultOccluded;
    fuse_cull_words[results][i] = result;
    if (result == kFuseCullResultPhase2Drawn) {
        uint slot;
        InterlockedAdd(fuse_cull_words[counts][kFuseCullCountPhase2Draws], 1u, slot);
        writeDraw(C, C.phase2DrawBase + slot, mesh, i);
    }
}
