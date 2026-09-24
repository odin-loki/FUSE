// WP-1.1 GPU scene scatter upload (Slang; GLSL twin gpu_scene_scatter.comp). GpuScene::commit()
// stages the dirty rows of a table as one packed block (rows, then one u32 destination row index per
// row) through the UploadQueue; this kernel, recorded by GpuScene::importInto() as the render-graph
// pass "gpu_scene.scatter", copies each word of each row to its slot in the table. Pure BDA: no
// descriptors, so the pipeline works next to either bindless backend.
struct Push {
    uint64_t src;           // packed rows, then u32 indices at word indexOffsetWords
    uint64_t dst;           // table base address
    uint rowWords;
    uint rowCount;
    uint indexOffsetWords;
    uint threadCount;       // dispatched threads (grid-stride loop)
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint* src = (uint*)pc.src;
    uint* dst = (uint*)pc.dst;
    const uint total = pc.rowWords * pc.rowCount;
    for (uint i = tid.x; i < total; i += pc.threadCount) {
        const uint row = i / pc.rowWords;
        const uint word = i - row * pc.rowWords;
        const uint target = src[pc.indexOffsetWords + row];
        dst[target * pc.rowWords + word] = src[i];
    }
}
