#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/vfx/particle_emitter.hpp>

#include <vector>

namespace fuse::vfx {

/// GPU SoA column identifiers — mirrors P7 `ParticleSoAGPU` device arrays.
enum class ParticleGpuColumn : u8 {
    Positions,
    Velocities,
    Ages,
    Lifetimes,
    Sizes,
    Colors,
    Alphas,
    AliveFlags,
};

/// Device-side SoA pointer bundle passed to CUDA kernels (B7.7 GPU stub).
struct ParticleSoAGPU {
    u64 positions = 0;
    u64 velocities = 0;
    u64 ages = 0;
    u64 lifetimes = 0;
    u64 sizes = 0;
    u64 colors = 0;
    u64 alphas = 0;
    u64 alive_flags = 0;
    u32 count = 0;
    u32 capacity = 0;
};

/// Per-column byte sizing and packed SSBO layout helpers.
struct ParticleGpuBufferLayout {
    static constexpr u32 kSimBlockSize = 256u;
    static constexpr u32 kEmitBlockSize = 64u;
    static constexpr usize kColumnAlignment = 16u;

    static u32 columnCount();
    static usize columnAlignment();
    static usize elementSize(ParticleGpuColumn column);
    static usize columnByteSize(ParticleGpuColumn column, u32 capacity);
    static usize columnDeviceOffset(ParticleGpuColumn column, u32 capacity);
    static usize paddingAfterColumn(ParticleGpuColumn column, u32 capacity);
    static usize packedDeviceBytes(u32 capacity);
    static bool validatePackedLayout(u32 capacity);
    static bool isColumnOffsetAligned(ParticleGpuColumn column, u32 capacity);
    static const char* columnName(ParticleGpuColumn column);
};

/// CUDA launch grid bookkeeping for simulate/emit kernels.
struct ParticleGpuDispatch {
    u32 simBlockCount = 0;
    u32 simThreadCount = 0;
    u32 emitBlockCount = 0;
    u32 emitThreadCount = 0;

    [[nodiscard]] static ParticleGpuDispatch forSimulate(u32 capacity);
    [[nodiscard]] static ParticleGpuDispatch forEmit(u32 emit_count);
    [[nodiscard]] static ParticleGpuDispatch forFrame(u32 capacity, u32 emit_count);
    [[nodiscard]] u32 totalSimThreads() const { return simBlockCount * simThreadCount; }
    [[nodiscard]] u32 totalEmitThreads() const { return emitBlockCount * emitThreadCount; }
    [[nodiscard]] bool simCovers(u32 slot_count) const { return totalSimThreads() >= slot_count; }
    [[nodiscard]] bool emitCovers(u32 emit_count) const {
        return emit_count == 0u || totalEmitThreads() >= emit_count;
    }
};

/// Logical GPU buffer handles — production wiring maps these to `renderer::BufferHandle`.
struct ParticleGpuBuffers {
    u64 packedSoa = 0;
    u64 vertexBuffer = 0;
    u32 capacity = 0;
    usize deviceBytes = 0;

    [[nodiscard]] static ParticleGpuBuffers forCapacity(u32 particle_capacity);
};

/// CPU-side column mirror for layout/dispatch stub tests — no device readback in production.
struct ParticleGpuMirror {
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> velocities;
    std::vector<f32> ages;
    std::vector<f32> lifetimes;
    std::vector<f32> sizes;
    std::vector<math::Vec3> colors;
    std::vector<f32> alphas;
    std::vector<u32> alive_flags;
    u32 capacity = 0;
    u32 alive_count = 0;

    void reserve(u32 particle_capacity);
    void clear();

    [[nodiscard]] static ParticleGpuMirror fromCpuSoA(const ParticleSoA& cpu);
    void writeToCpuSoA(ParticleSoA& cpu) const;

    [[nodiscard]] std::vector<u8> packToDeviceLayout() const;
    [[nodiscard]] static ParticleGpuMirror unpackFromDeviceLayout(const std::vector<u8>& bytes, u32 capacity);

    [[nodiscard]] bool matchesCpuSoA(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleSoAGPU toGpuPointers(u64 packed_device_address) const;
};

namespace particle_gpu_util {
[[nodiscard]] u32 gridDimX(u32 element_count, u32 block_size);
[[nodiscard]] u32 coveredThreadCount(u32 block_count, u32 block_size);
} // namespace particle_gpu_util

} // namespace fuse::vfx
