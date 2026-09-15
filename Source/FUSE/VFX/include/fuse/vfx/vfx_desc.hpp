#pragma once

#include <fuse/types.hpp>

namespace fuse::vfx {

struct VfxDesc {
    u32 max_emitters = 256;
    u32 max_effect_instances = 512;
    u32 default_max_particles = 4096;
    bool gpu_simulation = false;
};

enum class VfxBackendKind {
    CpuReference,
    Cuda,
};

} // namespace fuse::vfx
