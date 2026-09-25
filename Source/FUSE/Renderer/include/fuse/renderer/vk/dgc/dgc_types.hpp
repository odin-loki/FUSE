#pragma once
// WP-9.3 device-generated commands: records shared by the C++ host, the generate kernel
// (shaders/dgc/dgc_generate.{slang,comp}) and the CPU reference / emulation (dgc_reference.hpp).
//
// The generate kernel turns the WP-1.3 culling output (compacted VkDrawIndexedIndirectCommand
// array + GPU draw count, firstInstance = GPU-scene instance slot) into
//   (a) the VK_EXT_device_generated_commands token stream: one DgcSequence per culled draw, in the
//       culled order (sequence i = draw i), selecting the pipeline of the draw's material bucket
//       through an EXECUTION_SET token and drawing it through a DRAW_INDEXED token; and / or
//   (b) the fallback: per-bucket compacted draw args + counts for bucketCount x
//       (vkCmdBindPipeline + vkCmdDrawIndexedIndirectCount) (the existing indirect-count path,
//       split by pipeline).
// The bucket of a draw is chosen on the GPU: instance = scene.instances[slot], material =
// instance.material, bucket = materialBuckets[material] (default bucket when out of range).
#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::dgc {

inline constexpr u32 kDgcWorkgroup = 64u;
/// Hard cap of pipelines (buckets) per selector; the device limit maxIndirectPipelineCount applies too.
inline constexpr u32 kDgcMaxBuckets = 64u;

/// Byte layout of VkDrawIndexedIndirectCommand (same as culling::DrawIndexedIndirectCommand).
struct DgcDrawIndexed {
    u32 indexCount = 0;
    u32 instanceCount = 0;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    u32 firstInstance = 0;
};
static_assert(sizeof(DgcDrawIndexed) == 20u, "VkDrawIndexedIndirectCommand is 20 bytes");

/// One device-generated sequence: token 0 = EXECUTION_SET (u32 pipeline index at offset 0),
/// token 1 = DRAW_INDEXED (VkDrawIndexedIndirectCommand at offset 4). indirectStride = 24.
struct DgcSequence {
    u32 pipelineIndex = 0;
    DgcDrawIndexed draw{};
};
static_assert(sizeof(DgcSequence) == 24u && offsetof(DgcSequence, draw) == 4u, "DgcSequence layout (dgc_generate.*)");
inline constexpr u32 kDgcSequenceStride = 24u;
inline constexpr u32 kDgcTokenExecutionSetOffset = 0u;
inline constexpr u32 kDgcTokenDrawIndexedOffset = 4u;

/// Counts buffer words (reset to 0 by "dgc.reset" every frame).
enum DgcCount : u32 {
    kDgcCountSequences = 0u,   ///< DGC sequence count (== min(culled draw count, maxDraws))
    kDgcCountBucketBase = 1u,  ///< [1, 1 + bucketCount): fallback draw count of each bucket
};
inline constexpr u32 dgc_count_words(u32 bucketCount) { return (kDgcCountBucketBase + bucketCount + 3u) & ~3u; }

/// DgcPush::flags
enum DgcFlag : u32 {
    kDgcWriteSequences = 1u << 0,  ///< write the DGC token stream + sequence count
    kDgcWriteBucketArgs = 1u << 1, ///< write the per-bucket fallback args + counts
};

/// Push constants of dgc_generate (80 bytes; every buffer is read / written through BDA).
struct DgcPush {
    u64 args = 0;            ///< culled VkDrawIndexedIndirectCommand array (the phase's region)
    u64 drawCount = 0;       ///< address of the culled draw count (u32)
    u64 scene = 0;           ///< GpuScene header address (GpuScene::headerAddress())
    u64 materialBuckets = 0; ///< u32 per material row -> bucket
    u64 sequences = 0;       ///< out: DgcSequence[maxDraws]
    u64 bucketArgs = 0;      ///< out: DgcDrawIndexed[bucketCount][maxDraws]
    u64 counts = 0;          ///< in/out: DgcCount words
    u32 maxDraws = 0;
    u32 bucketCount = 0;
    u32 materialCount = 0;
    u32 defaultBucket = 0;
    u32 flags = 0;
    u32 pad = 0;
};
static_assert(sizeof(DgcPush) == 80u, "DgcPush layout (dgc_generate.slang / .comp)");
static_assert(offsetof(DgcPush, counts) == 48u && offsetof(DgcPush, maxDraws) == 56u && offsetof(DgcPush, flags) == 72u,
              "DgcPush offsets (dgc_generate.slang / .comp)");

/// Bucket of a material row; one definition for the CPU reference and (line for line) the kernels.
inline constexpr u32 dgc_select_bucket(u32 material, const u32* materialBuckets, u32 materialCount, u32 bucketCount,
                                       u32 defaultBucket) {
    u32 bucket = material < materialCount ? materialBuckets[material] : defaultBucket;
    return bucket < bucketCount ? bucket : defaultBucket;
}

} // namespace fuse::renderer::dgc
