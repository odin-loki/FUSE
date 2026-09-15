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

    [[nodiscard]] bool hasDeviceBinding() const { return positions != 0u && capacity > 0u; }
    [[nodiscard]] bool allColumnPointersBound() const;
    [[nodiscard]] bool validateAgainstLayout(u64 packed_base, u32 expected_capacity) const;
};

/// Byte span of one packed SoA column inside the device SSBO.
struct ParticleGpuColumnSpan {
    usize offset = 0u;
    usize byte_size = 0u;
    usize element_size = 0u;
    u32 slot_count = 0u;

    [[nodiscard]] usize endOffset() const { return offset + byte_size; }
    [[nodiscard]] bool isEmpty() const { return byte_size == 0u || slot_count == 0u; }
};

/// Per-column byte sizing and packed SSBO layout helpers.
struct ParticleGpuBufferLayout {
    static constexpr u32 kSimBlockSize = 256u;
    static constexpr u32 kEmitBlockSize = 64u;
    static constexpr usize kColumnAlignment = 16u;

    static u32 columnCount();
    static u32 columnIndex(ParticleGpuColumn column);
    static usize columnAlignment();
    static usize elementSize(ParticleGpuColumn column);
    static usize columnByteSize(ParticleGpuColumn column, u32 capacity);
    static ParticleGpuColumnSpan columnSpan(ParticleGpuColumn column, u32 capacity);
    static usize columnDeviceOffset(ParticleGpuColumn column, u32 capacity);
    static usize paddingAfterColumn(ParticleGpuColumn column, u32 capacity);
    static usize packedDeviceBytes(u32 capacity);
    static usize dataColumnBytes(u32 capacity);
    static usize packingOverheadBytes(u32 capacity);
    static u64 columnDeviceAddress(ParticleGpuColumn column, u64 base, u32 capacity);
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
    [[nodiscard]] bool isEmpty() const { return simBlockCount == 0u && emitBlockCount == 0u; }
    [[nodiscard]] bool hasSimLaunch() const { return simBlockCount > 0u; }
    [[nodiscard]] bool hasEmitLaunch() const { return emitBlockCount > 0u; }
    [[nodiscard]] bool shouldSkipSimLaunch(u32 capacity) const { return capacity == 0u || !hasSimLaunch(); }
    [[nodiscard]] bool shouldSkipEmitLaunch(u32 emit_count) const {
        return emit_count == 0u || !hasEmitLaunch();
    }
    [[nodiscard]] u32 simPaddingThreads(u32 capacity) const;
    [[nodiscard]] u32 emitPaddingThreads(u32 emit_count) const;
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
    void syncFromCpuSoA(const ParticleSoA& cpu);

    [[nodiscard]] static ParticleGpuMirror fromCpuSoA(const ParticleSoA& cpu);
    [[nodiscard]] bool writeToCpuSoA(ParticleSoA& cpu) const;
    [[nodiscard]] bool isEmpty() const { return alive_count == 0u; }
    [[nodiscard]] bool packedBytesFit(const std::vector<u8>& bytes) const;

    [[nodiscard]] std::vector<u8> packToDeviceLayout() const;
    [[nodiscard]] static ParticleGpuMirror unpackFromDeviceLayout(const std::vector<u8>& bytes, u32 capacity);

    [[nodiscard]] bool matchesCpuSoA(const ParticleSoA& cpu) const;
    [[nodiscard]] bool matchesPackedLayout(const std::vector<u8>& bytes) const;
    void syncAliveCountFromFlags();
    [[nodiscard]] ParticleSoAGPU toGpuPointers(u64 packed_device_address) const;
};

/// Per-frame GPU stub plan: buffer sizing, dispatch counts, and empty-launch guards.
struct ParticleGpuFramePlan {
    ParticleGpuBuffers buffers{};
    ParticleGpuDispatch dispatch{};
    u32 capacity = 0u;
    u32 emit_count = 0u;
    u32 alive_count = 0u;

    [[nodiscard]] static ParticleGpuFramePlan forStub(u32 particle_capacity, u32 emit_count, u32 alive_count);
    [[nodiscard]] bool skipSimLaunch() const;
    [[nodiscard]] bool skipEmitLaunch() const;
    [[nodiscard]] bool isIdle() const { return skipSimLaunch() && skipEmitLaunch(); }
    [[nodiscard]] ParticleSoAGPU gpuPointers(u64 packed_device_address) const;
};

namespace particle_gpu_util {
[[nodiscard]] u32 gridDimX(u32 element_count, u32 block_size);
[[nodiscard]] u32 coveredThreadCount(u32 block_count, u32 block_size);
[[nodiscard]] u32 paddingThreads(u32 element_count, u32 block_count, u32 block_size);
} // namespace particle_gpu_util

} // namespace fuse::vfx
