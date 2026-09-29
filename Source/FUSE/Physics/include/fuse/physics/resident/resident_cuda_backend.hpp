#pragma once

// Internal seam between the resident pipeline's host orchestration (src/resident/resident_physics.cpp)
// and its CUDA translation unit (kernels/physics_resident.cu). Declared for every build; defined only
// when the CUDA backend is compiled (FUSE_HAS_CUDA). Streams, events and pointers are opaque
// (cudaStream_t / cudaEvent_t / device pointers).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/physics/broadphase/radix_sort_launch.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::resident::cuda_backend {

/// cuda::entry<Body, Params> of every resident kernel (resident_kernels.hpp) and of the shared
/// scan / radix sort kernels (broadphase_kernel.hpp), all compiled with --fmad=false.
struct Entries {
    kernel::DeviceEntryFn cellCount = nullptr;
    kernel::DeviceEntryFn keys = nullptr;
    kernel::DeviceEntryFn pad = nullptr;
    kernel::DeviceEntryFn runFlags = nullptr;
    kernel::DeviceEntryFn runStarts = nullptr;
    kernel::DeviceEntryFn cells = nullptr;
    kernel::DeviceEntryFn pairs = nullptr;
    kernel::DeviceEntryFn planePairs = nullptr;
    kernel::DeviceEntryFn uniqueFlags = nullptr;
    kernel::DeviceEntryFn uniqueWrite = nullptr;
    kernel::DeviceEntryFn status = nullptr;
    kernel::DeviceEntryFn detect = nullptr;
    kernel::DeviceEntryFn compact = nullptr;
    kernel::DeviceEntryFn solveColor = nullptr;
    kernel::DeviceEntryFn solveSerial = nullptr;
    broadphase_kernel::SortEntries sort{}; ///< scan + radix sort (stream / synchronize set by the caller)
};

enum class CopyKind : u8 { HostToDevice = 0, DeviceToHost = 1, DeviceToDevice = 2 };

#if defined(FUSE_HAS_CUDA)
bool deviceUsable();
Entries entries();
bool createStream(void** stream);
void destroyStream(void* stream);
bool allocate(void** pointer, usize bytes);
void release(void* pointer);
bool copyAsync(void* dst, const void* src, usize bytes, CopyKind kind, void* stream);
bool fillAsync(void* dst, int value, usize bytes, void* stream);
bool synchronize(void* stream);
bool createEvent(void** event);
void destroyEvent(void* event);
bool recordEvent(void* event, void* stream);
bool elapsedMs(void* start, void* stop, float* ms);
#endif

} // namespace fuse::physics::resident::cuda_backend
