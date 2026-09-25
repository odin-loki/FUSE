// WP-1.1 exit-test kernel (fuse_rp_gpu_scene), Slang twin of gpu_scene_readback.comp: same push
// layout, same output words (see the GLSL file for the mode 0 / mode 1 word map). Reads the scene
// only through the bindless address table and BDA pointers (gpu_scene.slang).
#include "gpu_scene.slang"

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_words[];

struct Push {
    uint scene;
    uint outBuffer;
    uint mode;
    uint table;
    uint wordCount;
    uint outOffset;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const uint kTypedWords = 40u;

void store(uint index, uint value) { fuse_words[pc.outBuffer & 0xFFFFFu][index] = value; }

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.wordCount) {
        return;
    }
    GpuSceneHeader* scene = fuseGpuScene(pc.scene);
    if (pc.mode == 0u) {
        uint64_t base = fuse_buffer_address(pc.scene);
        if (pc.table < kFuseGpuSceneTableCount) {
            base = scene.addresses[pc.table];
        } else if (pc.table >= 8u) {
            base = fuseGpuSceneMeshes(scene)[pc.table - 8u].meshlets;
        }
        store(pc.outOffset + i, ((uint*)base)[i]);
        return;
    }
    const uint o = pc.outOffset + i * kTypedWords;
    const GpuInstance inst = fuseGpuSceneInstances(scene)[i];
    store(o + 0u, inst.mesh);
    store(o + 1u, inst.material);
    store(o + 2u, inst.flags);
    store(o + 3u, inst.generation);
    store(o + 4u, inst.entityIndex);
    store(o + 5u, inst.entityGeneration);
    store(o + 6u, inst.userData);
    store(o + 7u, inst.reserved);
    const GpuTransform cur = fuseGpuSceneTransforms(scene)[i];
    const GpuTransform prev = fuseGpuScenePrevTransforms(scene)[i];
    for (uint r = 0u; r < 3u; ++r) {
        for (uint c = 0u; c < 4u; ++c) {
            store(o + 8u + r * 4u + c, asuint(cur.rows[r][c]));
            store(o + 20u + r * 4u + c, asuint(prev.rows[r][c]));
        }
    }
    uint w32 = kFuseGpuSceneInvalid, w33 = kFuseGpuSceneInvalid, w34 = kFuseGpuSceneInvalid, w35 = kFuseGpuSceneInvalid;
    if (inst.mesh < scene.counts[kFuseGpuSceneMeshes]) {
        const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
        w32 = mesh.meshletCount;
        w33 = asuint(mesh.boundsRadius);
        w34 = mesh.geometryHandle;
        w35 = uint(mesh.meshlets & 0xFFFFFFFFull);
    }
    store(o + 32u, w32);
    store(o + 33u, w33);
    store(o + 34u, w34);
    store(o + 35u, w35);
    uint w36 = kFuseGpuSceneInvalid, w37 = kFuseGpuSceneInvalid;
    if (inst.material < scene.counts[kFuseGpuSceneMaterials]) {
        uint* materials = (uint*)scene.addresses[kFuseGpuSceneMaterials];
        w36 = materials[inst.material * 32u + 0u];
        w37 = materials[inst.material * 32u + 19u];
    }
    store(o + 36u, w36);
    store(o + 37u, w37);
    store(o + 38u, scene.magic);
    store(o + 39u, scene.liveInstances);
}
