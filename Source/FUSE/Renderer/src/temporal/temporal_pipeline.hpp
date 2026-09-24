#pragma once
// WP-4.1 internal: embedded kernels and compute-pipeline creation shared by TemporalMotion and TaauGpu.

#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class VulkanDevice;
class BindlessDescriptors;
} // namespace fuse::renderer

namespace fuse::renderer::temporal::detail {

enum class TemporalKernel : u8 { Motion = 0, Taau = 1 };

struct KernelCode {
    const u32* words = nullptr;
    usize bytes = 0;
    const char* language = "none";
};

/// The embedded SPIR-V of `kernel` for `language` (Auto: Slang if built, else GLSL); words == null if absent.
KernelCode kernelCode(TemporalKernel kernel, TemporalKernelLanguage language);

/// Pipeline layout (the bindless set + a 16-byte compute push range) and the compute pipeline. False (both
/// null) on failure.
bool createComputePipeline(VulkanDevice& device, const BindlessDescriptors& bindless, const KernelCode& code,
                           void*& layout, void*& pipeline);
void destroyComputePipeline(VulkanDevice& device, void*& layout, void*& pipeline);

/// Binds pipeline + bindless set, pushes `push` (16 bytes) and dispatches.
void dispatch(void* commandBuffer, const BindlessDescriptors& bindless, void* layout, void* pipeline, const void* push,
              u32 groupsX, u32 groupsY);

} // namespace fuse::renderer::temporal::detail
