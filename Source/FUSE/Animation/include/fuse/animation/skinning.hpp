#pragma once

#include <fuse/animation/skeleton.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::animation {

enum class SkinningBackend {
    CpuReference,
    Cuda,
};

struct SkinningWeights {
    u32 bone_indices[4] = {};
    f32 bone_weights[4] = {1.f, 0.f, 0.f, 0.f};
};

struct SkinningInput {
    std::vector<vec3> rest_positions;
    std::vector<vec3> rest_normals;
    std::vector<SkinningWeights> weights;
    std::vector<mat4> bone_transforms;
};

struct SkinningOutput {
    std::vector<vec3> positions;
    std::vector<vec3> normals;
};

/// True only when the CUDA skinning kernel (kernels/skinning.cu) is compiled into this build AND a
/// CUDA device is usable (`kernel::backend_available(Backend::Cuda)`).
bool skinning_cuda_kernel_available();

/// Backend skin_vertices() actually runs on: Cuda when skinning_cuda_kernel_available(); CpuReference
/// (the CPU kernel backends, bit-identical to each other) otherwise.
SkinningBackend skinning_backend();

/// Linear blend skinning through the single-source kernel (fuse/animation/skinning_kernel.hpp,
/// kernel name "skinning_lbs"). Normals use the bone matrices' linear part and are renormalized.
/// Returns false when there are no vertices or the weight/normal arrays do not match the vertex
/// count (normals may also be empty, in which case none are produced). Runs on skinning_backend();
/// reuses `output`'s storage, so steady-state calls do not allocate on the CPU backends.
bool skin_vertices(const SkinningInput& input, SkinningOutput& output);

/// skin_vertices on an explicit kernel backend. Cuda / Auto use the CUDA kernel when
/// skinning_cuda_kernel_available(); otherwise kernel::launch falls back to CpuParallel and records
/// the requested vs executed backend. `stream` is a cudaStream_t (may be null).
bool skin_vertices_on(kernel::Backend backend, const SkinningInput& input, SkinningOutput& output,
                      void* stream = nullptr);

/// CUDA-event timing of the skinning kernel on resident device buffers (B7 row "GPU skinning transforms
/// 10k vertices in < 0.5 ms — verified with CUDA events"). The mesh, weights and palette are staged on the
/// device once; then, on a dedicated stream, one untimed warm-up launch, `iterations` launches each
/// bracketed by its own cudaEventRecord pair, a batch of `iterations` back-to-back launches bracketed by
/// one pair, and `iterations` "frames" of palette upload + launch. `output` receives the device result.
struct SkinningCudaTiming {
    bool ran = false;
    const char* reason = "";      ///< Why ran is false (no CUDA build / device / invalid input / CUDA error).
    u32 vertices = 0;
    u32 bones = 0;
    u32 iterations = 0;
    f32 kernel_ms_min = 0.f;      ///< Single launch, CUDA events.
    f32 kernel_ms_median = 0.f;
    f32 kernel_ms_max = 0.f;
    f32 batch_ms_per_launch = 0.f; ///< Back-to-back launches / iterations (no host gaps between kernels).
    f32 frame_ms_median = 0.f;    ///< Palette upload (host -> device) + launch per frame.
    f32 upload_ms = 0.f;          ///< One-time staging of mesh + weights + palette.
    f32 download_ms = 0.f;        ///< Read-back of positions + normals.
};

/// Runs the timing above; ran=false without the CUDA kernel (skinning_cuda_kernel_available()).
SkinningCudaTiming time_skinning_cuda(const SkinningInput& input, SkinningOutput& output, u32 iterations);

/// Skinning palette (bone buffer contents): out[i] = pose world[i] * skeleton inverse_bind[i].
/// Reuses `out`'s storage, so steady-state per-frame calls do not allocate.
void compute_skinning_palette(const Skeleton& skel, const Pose& pose, std::vector<mat4>& out);

} // namespace fuse::animation
