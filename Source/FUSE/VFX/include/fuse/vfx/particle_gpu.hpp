#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/vfx/particle_emitter.hpp>

#include <array>
#include <vector>

namespace fuse::vfx {

/// CPU mirror sync preconditions for stub tests — production upload paths use the same guards.
enum class ParticleGpuSyncGuard : u8 {
    Ok,
    MirrorUninitialized,
    CpuUninitialized,
    CapacityMismatch,
};

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

/// CPU mirror sync preflight for stub upload paths (B7.7 GPU deepen follow-up).
struct ParticleGpuMirrorPreflight {
    ParticleGpuSyncGuard sync_guard = ParticleGpuSyncGuard::MirrorUninitialized;
    ParticleGpuSyncGuard write_guard = ParticleGpuSyncGuard::MirrorUninitialized;
    bool alive_count_matches_flags = true;
    bool already_synced = false;

    [[nodiscard]] bool can_sync_from_cpu() const;
    [[nodiscard]] bool can_write_to_cpu() const;
    [[nodiscard]] bool can_pack() const;
    [[nodiscard]] bool needs_resize_sync() const;
    [[nodiscard]] bool can_bind_device() const;
};

/// Device-upload preflight combining mirror sync guards and packed layout sizing (B7.7 GPU deepen).
struct ParticleGpuMirrorUploadPreflight {
    ParticleGpuMirrorPreflight mirror{};
    bool layout_bytes_ok = false;

    [[nodiscard]] bool can_upload() const;
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
    static bool validateSlotIndex(u32 slot_index, u32 capacity);
    static usize slotDeviceOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity);
    static bool containsByteOffset(usize byte_offset, u32 capacity);
    static bool locateColumnAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumnSpan* out_span);
    static bool validatePackedLayout(u32 capacity);
    static bool isColumnOffsetAligned(ParticleGpuColumn column, u32 capacity);
    static bool validateColumnSpanChain(u32 capacity);
    static std::array<ParticleGpuColumnSpan, 8> collectColumnSpans(u32 capacity);
    static const char* columnName(ParticleGpuColumn column);
    static const char* syncGuardName(ParticleGpuSyncGuard guard);
};

/// Dispatch preflight for stub CUDA launch wiring (B7.7 GPU deepen follow-up).
struct ParticleGpuDispatchPreflight {
    u32 slot_count = 0u;
    u32 emit_count = 0u;
    bool skip_sim_launch = true;
    bool skip_emit_launch = true;
    bool sim_covers = false;
    bool emit_covers = false;
    bool sim_padding_ok = false;
    bool emit_padding_ok = false;

    [[nodiscard]] bool can_launch_sim() const;
    [[nodiscard]] bool can_launch_emit() const;
    [[nodiscard]] bool ready_for_stub() const;
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
    [[nodiscard]] u32 firstSimPaddingThread(u32 slot_count) const;
    [[nodiscard]] u32 firstEmitPaddingThread(u32 emit_count) const;
    [[nodiscard]] bool isSimPaddingThread(u32 global_thread_index, u32 slot_count) const;
    [[nodiscard]] bool isEmitPaddingThread(u32 global_thread_index, u32 emit_count) const;
    [[nodiscard]] bool simPaddingAccountsFor(u32 slot_count) const;
    [[nodiscard]] bool emitPaddingAccountsFor(u32 emit_count) const;
    [[nodiscard]] ParticleGpuDispatchPreflight preflightSimulate(u32 slot_count) const;
    [[nodiscard]] ParticleGpuDispatchPreflight preflightEmit(u32 emit_count) const;
    [[nodiscard]] ParticleGpuDispatchPreflight preflightFrame(u32 slot_count, u32 emit_count) const;
};

/// Buffer binding preflight for stub device SSBO paths (B7.7 GPU deepen).
struct ParticleGpuBuffersPreflight {
    bool capacity_ok = false;
    bool bytes_ok = false;
    bool bound = false;

    [[nodiscard]] bool can_bind() const;
    [[nodiscard]] bool is_empty_capacity() const;
};

/// Emit-count clamp for stub GPU frame planning (B7.7 GPU deepen).
struct ParticleGpuEmitGuard {
    u32 requested = 0u;
    u32 clamped = 0u;
    u32 free_slots = 0u;
    bool at_capacity = false;
    bool skip_emit = true;

    [[nodiscard]] bool can_emit() const;
    [[nodiscard]] bool emit_clamped() const { return requested > clamped; }
};

/// Logical GPU buffer handles — production wiring maps these to `renderer::BufferHandle`.
struct ParticleGpuBuffers {
    u64 packedSoa = 0;
    u64 vertexBuffer = 0;
    u32 capacity = 0;
    usize deviceBytes = 0;

    [[nodiscard]] static ParticleGpuBuffers forCapacity(u32 particle_capacity);
    [[nodiscard]] bool isBound() const { return packedSoa != 0u; }
    [[nodiscard]] bool hasDeviceAllocation() const { return deviceBytes > 0u && capacity > 0u; }
    [[nodiscard]] bool isEmptyCapacity() const { return capacity == 0u; }
    [[nodiscard]] ParticleGpuBuffersPreflight preflight() const;
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
    [[nodiscard]] bool trySyncFromCpuSoA(const ParticleSoA& cpu);

    [[nodiscard]] ParticleGpuSyncGuard syncGuardForCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleGpuSyncGuard writeGuardForCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] bool canSyncFromCpuSoA(const ParticleSoA& cpu) const;
    [[nodiscard]] bool canWriteToCpuSoA(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleGpuMirrorPreflight preflightFromCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleGpuMirrorUploadPreflight preflightDeviceUpload(const ParticleSoA& cpu) const;
    [[nodiscard]] bool shouldSkipSyncFromCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] bool aliveCountMatchesFlags() const;

    [[nodiscard]] static ParticleGpuMirror fromCpuSoA(const ParticleSoA& cpu);
    [[nodiscard]] bool writeToCpuSoA(ParticleSoA& cpu) const;
    [[nodiscard]] bool tryWriteToCpuSoA(ParticleSoA& cpu) const;
    [[nodiscard]] bool isEmpty() const { return alive_count == 0u; }
    [[nodiscard]] bool packedBytesFit(const std::vector<u8>& bytes) const;

    [[nodiscard]] std::vector<u8> packToDeviceLayout() const;
    [[nodiscard]] bool shouldSkipPack() const;
    [[nodiscard]] bool tryPackToDeviceLayout(std::vector<u8>& out) const;
    [[nodiscard]] static ParticleGpuMirror unpackFromDeviceLayout(const std::vector<u8>& bytes, u32 capacity);

    [[nodiscard]] bool matchesCpuSoA(const ParticleSoA& cpu) const;
    [[nodiscard]] bool matchesPackedLayout(const std::vector<u8>& bytes) const;
    void syncAliveCountFromFlags();
    [[nodiscard]] ParticleSoAGPU toGpuPointers(u64 packed_device_address) const;
};

/// Frame-plan preflight for stub launch wiring (B7.7 GPU deepen follow-up).
struct ParticleGpuFramePreflight {
    bool buffers_ok = false;
    bool sim_dispatch_ok = false;
    bool emit_dispatch_ok = false;
    bool sim_padding_ok = false;
    bool emit_padding_ok = false;

    [[nodiscard]] bool ready_for_stub() const;
};

/// Combined launch preflight tying emit, sim, and buffer guards (B7.7 GPU deepen).
struct ParticleGpuFrameLaunchPreflight {
    ParticleGpuFramePreflight frame{};
    ParticleGpuEmitGuard emit{};
    ParticleGpuBuffersPreflight buffers{};
    bool skip_sim_when_empty = true;
    bool sim_can_launch = false;
    bool emit_can_launch = false;

    [[nodiscard]] bool ready_for_stub() const;
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
    [[nodiscard]] bool buffersSizedForCapacity() const;
    [[nodiscard]] u32 simPaddingThreadCount() const;
    [[nodiscard]] u32 emitPaddingThreadCount() const;
    [[nodiscard]] ParticleGpuFramePreflight preflight() const;
    [[nodiscard]] ParticleGpuFrameLaunchPreflight launchPreflight() const;
    [[nodiscard]] u32 freeSlotCount() const;
    [[nodiscard]] u32 clampedEmitCount() const;
    [[nodiscard]] ParticleGpuEmitGuard emitGuard() const;
    [[nodiscard]] bool shouldSkipSimWhenEmpty() const;
    [[nodiscard]] bool canSimulate() const;
    [[nodiscard]] bool canEmit() const;
    [[nodiscard]] bool buffersBound() const;
    [[nodiscard]] bool requiresBufferBind() const;
    [[nodiscard]] ParticleSoAGPU gpuPointers(u64 packed_device_address) const;
};

[[nodiscard]] u32 gpu_free_slot_count(u32 capacity, u32 alive_count);
[[nodiscard]] u32 clamp_gpu_emit_count(u32 emit_count, u32 capacity, u32 alive_count);
[[nodiscard]] ParticleGpuEmitGuard emit_guard_for_frame(u32 requested, u32 capacity, u32 alive_count);
[[nodiscard]] bool should_skip_sim_dispatch(u32 capacity);
[[nodiscard]] bool should_skip_emit_dispatch(u32 emit_count);
[[nodiscard]] bool should_skip_sim_when_empty(u32 alive_count);
[[nodiscard]] bool should_skip_pack(u32 capacity);
[[nodiscard]] bool is_empty_device_buffer(const ParticleGpuBuffers& buffers);
[[nodiscard]] bool should_skip_mirror_sync(const ParticleGpuMirror& mirror, const ParticleSoA& cpu);
[[nodiscard]] bool should_skip_mirror_write(const ParticleGpuMirror& mirror, const ParticleSoA& cpu);

namespace particle_gpu_util {
[[nodiscard]] u32 gridDimX(u32 element_count, u32 block_size);
[[nodiscard]] u32 coveredThreadCount(u32 block_count, u32 block_size);
[[nodiscard]] u32 paddingThreads(u32 element_count, u32 block_count, u32 block_size);
[[nodiscard]] bool threadCoversElement(u32 thread_index, u32 element_count);
[[nodiscard]] bool isPaddingThread(u32 thread_index, u32 element_count);
[[nodiscard]] u32 blockIndexOf(u32 global_thread_index, u32 block_size);
[[nodiscard]] u32 localThreadIndex(u32 global_thread_index, u32 block_size);
[[nodiscard]] u32 globalThreadIndex(u32 block_index, u32 local_thread_index, u32 block_size);
} // namespace particle_gpu_util

} // namespace fuse::vfx
