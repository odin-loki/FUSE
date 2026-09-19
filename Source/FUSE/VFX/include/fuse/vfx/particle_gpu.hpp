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

/// Slot index validation for kernel column access (B7.7 GPU stub).
enum class ParticleGpuSlotGuard : u8 {
    Ok,
    OutOfRange,
    InvalidCapacity,
/// Per-slot packed-layout access guards for column byte/device offset helpers.
enum class ParticleGpuSlotOffsetGuard : u8 {
    UninitializedCapacity,
    SlotOutOfRange,
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

/// Pack preconditions for mirror device-layout serialization (B7.7 GPU deepen follow-up).
enum class ParticleGpuPackGuard : u8 {
    Ok,
    MirrorUninitialized,
    AliveCountMismatch,
};

/// CPU mirror sync preflight for stub upload paths (B7.7 GPU deepen follow-up).
struct ParticleGpuMirrorPreflight {
    ParticleGpuSyncGuard sync_guard = ParticleGpuSyncGuard::MirrorUninitialized;
    ParticleGpuSyncGuard write_guard = ParticleGpuSyncGuard::MirrorUninitialized;
    ParticleGpuPackGuard pack_guard = ParticleGpuPackGuard::MirrorUninitialized;
    bool alive_count_matches_flags = true;
    bool already_synced = false;
    bool packed_layout_ok = false;

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
    [[nodiscard]] bool can_unpack(const std::vector<u8>& bytes, u32 particle_capacity) const;
    [[nodiscard]] bool can_unpack(const std::vector<u8>& bytes, u32 capacity) const;
};

/// Mirror pack preflight for guarded device-layout serialization (B7.7 GPU deepen follow-up).
struct ParticleGpuMirrorPackPreflight {
    ParticleGpuMirrorPreflight mirror{};
    bool layout_bytes_ok = false;
    bool packed_bytes_fit = false;

    [[nodiscard]] bool can_pack() const;
};

/// Slot-offset preflight for stub kernel access guards (B7.7 GPU deepen follow-up).
struct ParticleGpuSlotPreflight {
    ParticleGpuColumn column = ParticleGpuColumn::Positions;
    u32 slot_index = 0u;
    u32 capacity = 0u;
    bool slot_in_bounds = false;
    bool offset_in_column = false;
    bool offset_aligned = false;
    usize device_offset = 0u;

    [[nodiscard]] bool ready_for_stub_access() const;
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
    static bool validateSlotDeviceOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity);
    static usize slotDeviceOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity);
    static u32 slotIndexFromColumnOffset(ParticleGpuColumn column, usize column_byte_offset, u32 capacity);
    static u64 slotDeviceAddress(ParticleGpuColumn column, u64 base, u32 slot_index, u32 capacity);
    static bool validateSlotDeviceAddress(u64 address, u64 packed_base, ParticleGpuColumn column, u32 slot_index,
                                          u32 capacity);
    static bool locateSlotAtByteOffset(usize byte_offset, u32 capacity, ParticleGpuColumn* out_column,
                                     u32* out_slot_index);
    static bool isSlotOffsetAligned(ParticleGpuColumn column, u32 slot_index, u32 capacity);
    static ParticleGpuSlotPreflight preflightSlotAccess(ParticleGpuColumn column, u32 slot_index, u32 capacity);
    static u32 slotIndexAtColumnOffset(ParticleGpuColumn column, usize column_relative_offset, u32 capacity);
    static bool locateSlotAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumn* out_column, u32* out_slot_index);
    static bool containsByteOffset(usize byte_offset, u32 capacity);
    static bool locateColumnAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumnSpan* out_span);
    static bool locateSlotAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumn* out_column, u32* out_slot);
    static bool packedBytesFitCapacity(const std::vector<u8>& bytes, u32 capacity);
    static bool validatePackedLayout(u32 capacity);
    static bool isColumnOffsetAligned(ParticleGpuColumn column, u32 capacity);
    static bool validateColumnSpanChain(u32 capacity);
    static std::array<ParticleGpuColumnSpan, 8> collectColumnSpans(u32 capacity);
    static const char* columnName(ParticleGpuColumn column);
    static const char* syncGuardName(ParticleGpuSyncGuard guard);
    static const char* slotGuardName(ParticleGpuSlotGuard guard);

    [[nodiscard]] static bool isSlotInRange(u32 slot_index, u32 capacity);
    [[nodiscard]] static ParticleGpuSlotGuard slotGuard(u32 slot_index, u32 capacity);
    [[nodiscard]] static usize slotElementByteOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity);
    [[nodiscard]] static usize slotColumnByteOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity);
};

/// Non-mutating simulate/emit launch diagnostics for stub tests.
struct ParticleGpuDispatchPreflight {
    u32 element_count = 0u;
    u32 block_count = 0u;
    u32 block_size = 0u;
    u32 covered_threads = 0u;
    u32 padding_threads = 0u;
    bool skipped = true;

    [[nodiscard]] bool coversElements() const { return !skipped && covered_threads >= element_count; }
    [[nodiscard]] bool hasPadding() const { return padding_threads > 0u; }

/// Dispatch preflight for stub CUDA launch wiring (B7.7 GPU deepen follow-up).
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
    static const char* slotOffsetGuardName(ParticleGpuSlotOffsetGuard guard);

    [[nodiscard]] static bool isValidSlotIndex(u32 slot, u32 capacity);
    [[nodiscard]] static ParticleGpuSlotOffsetGuard slotOffsetGuard(u32 slot, u32 capacity);
    [[nodiscard]] static usize columnSlotByteOffset(ParticleGpuColumn column, u32 capacity, u32 slot);
    [[nodiscard]] static u64 columnSlotDeviceAddress(ParticleGpuColumn column, u64 base, u32 capacity, u32 slot);
    [[nodiscard]] static bool canAccessSlotAtOffset(ParticleGpuColumn column, u32 capacity, u32 slot);

/// Non-mutating dispatch launch bookkeeping for stub frame planning.
struct DispatchPreflight {
    u32 capacity = 0u;
    bool skip_sim = true;
    bool skip_emit = true;
    u32 sim_padding_threads = 0u;
    u32 emit_padding_threads = 0u;
    bool is_idle = true;

/// Non-mutating mirror sync/write guard summary for CPU upload paths.
struct MirrorPreflight {
    ParticleGpuSyncGuard sync_guard = ParticleGpuSyncGuard::MirrorUninitialized;
    ParticleGpuSyncGuard write_guard = ParticleGpuSyncGuard::MirrorUninitialized;
    bool can_sync = false;
    bool can_write = false;
    bool would_resize = false;

/// Non-mutating slot offset guard for packed column access.
struct ParticleGpuSlotOffsetPreflight {
    ParticleGpuSlotOffsetGuard guard = ParticleGpuSlotOffsetGuard::UninitializedCapacity;
    u32 slot = 0u;
    usize column_byte_offset = 0u;
    bool can_access = false;

/// Non-mutating frame-plan guard tying buffer sizing to dispatch preflight.
struct FramePlanPreflight {
    DispatchPreflight dispatch{};
    bool buffers_sized = false;
    static const char* packGuardName(ParticleGpuPackGuard guard);
};

/// Dispatch launch preflight for stub kernel wiring (B7.7 GPU deepen follow-up).
struct ParticleGpuDispatchPreflight {
    bool sim_covers_capacity = false;
    bool emit_covers_count = false;
    bool sim_will_skip_launch = false;
    bool emit_will_skip_launch = false;
    bool sim_padding_ok = false;
    bool emit_padding_ok = false;

    [[nodiscard]] bool ready_for_stub() const;
    bool sim_covers = false;
    bool emit_covers = false;
    bool sim_skip_ok = false;
    bool emit_skip_ok = false;

    [[nodiscard]] bool ready_for_stub(u32 slot_count, u32 emit_count) const;
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
    [[nodiscard]] static ParticleGpuDispatchPreflight preflightSimulate(u32 capacity);
    [[nodiscard]] static ParticleGpuDispatchPreflight preflightEmit(u32 emit_count);
    [[nodiscard]] ParticleGpuDispatchPreflight simPreflight(u32 slot_count) const;
    [[nodiscard]] ParticleGpuDispatchPreflight emitPreflight(u32 emit_count) const;
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

/// Empty-buffer preflight for stub device allocation (B7.7 GPU deepen follow-up).
struct ParticleGpuBufferPreflight {
    bool capacity_ok = false;
    bool device_bytes_ok = false;
    bool is_empty = true;
    bool has_binding = false;

    [[nodiscard]] bool can_allocate() const;
    [[nodiscard]] bool ready_for_stub() const;

    [[nodiscard]] DispatchPreflight preflight(u32 capacity, u32 emit_count) const;
    [[nodiscard]] u32 simSlotForThread(u32 global_thread_index, u32 slot_count) const;
    [[nodiscard]] u32 emitIndexForThread(u32 global_thread_index, u32 emit_count) const;
    [[nodiscard]] bool simThreadHasValidSlot(u32 global_thread_index, u32 slot_count) const;
    [[nodiscard]] bool emitThreadHasValidIndex(u32 global_thread_index, u32 emit_count) const;
    [[nodiscard]] ParticleGpuDispatchPreflight preflight(u32 capacity, u32 emit_count) const;
    [[nodiscard]] ParticleGpuDispatchPreflight preflight(u32 slot_count, u32 emit_count) const;
};

/// Logical GPU buffer handles — production wiring maps these to `renderer::BufferHandle`.
struct ParticleGpuBuffers {
    u64 packedSoa = 0;
    u64 vertexBuffer = 0;
    u32 capacity = 0;
    usize deviceBytes = 0;

    [[nodiscard]] static ParticleGpuBuffers forCapacity(u32 particle_capacity);
    [[nodiscard]] bool isEmpty() const { return capacity == 0u || deviceBytes == 0u; }
    [[nodiscard]] bool hasDeviceBinding() const { return packedSoa != 0u; }
    [[nodiscard]] ParticleGpuBufferPreflight preflight() const;
};

/// Non-mutating CPU mirror sync/write diagnostics for stub tests.
struct ParticleGpuMirrorSyncPreflight {
    ParticleGpuSyncGuard guard = ParticleGpuSyncGuard::Ok;
    bool needs_resize = false;
    bool can_sync = false;
    bool can_write = false;

    [[nodiscard]] bool ready() const { return guard == ParticleGpuSyncGuard::Ok; }
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
    [[nodiscard]] ParticleGpuMirrorPackPreflight preflightPack() const;
    [[nodiscard]] bool shouldSkipSyncFromCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] bool validateSlotAccess(u32 slot_index) const;
    [[nodiscard]] ParticleGpuSlotPreflight preflightSlot(u32 slot_index, ParticleGpuColumn column) const;
    [[nodiscard]] bool aliveCountMatchesFlags() const;
    [[nodiscard]] ParticleGpuMirrorSyncPreflight preflightSyncFromCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleGpuMirrorSyncPreflight preflightWriteToCpu(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleGpuSlotGuard slotGuard(u32 slot_index) const;
    [[nodiscard]] bool isSlotInRange(u32 slot_index) const;
    [[nodiscard]] MirrorPreflight preflightSync(const ParticleSoA& cpu) const;
    [[nodiscard]] ParticleGpuPackGuard packGuard() const;
    [[nodiscard]] bool canUnpackFromDeviceLayout(const std::vector<u8>& bytes, u32 particle_capacity) const;

    [[nodiscard]] static ParticleGpuMirror fromCpuSoA(const ParticleSoA& cpu);
    [[nodiscard]] bool writeToCpuSoA(ParticleSoA& cpu) const;
    [[nodiscard]] bool tryWriteToCpuSoA(ParticleSoA& cpu) const;
    [[nodiscard]] bool isEmpty() const { return alive_count == 0u; }
    [[nodiscard]] bool packedBytesFit(const std::vector<u8>& bytes) const;

    [[nodiscard]] std::vector<u8> packToDeviceLayout() const;
    [[nodiscard]] std::vector<u8> tryPackToDeviceLayout() const;
    [[nodiscard]] static ParticleGpuSyncGuard unpackGuardForLayout(const std::vector<u8>& bytes, u32 capacity);
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
    bool empty_buffer = false;
    bool sim_alive_ok = false;
    bool emit_slots_ok = false;
    bool skip_sim_zero_alive = false;
    bool skip_emit_at_capacity = false;
    bool dispatch_preflight_ok = false;

    [[nodiscard]] bool ready_for_stub() const;
    [[nodiscard]] bool can_simulate() const;
    [[nodiscard]] bool can_emit() const;
/// Non-mutating frame-plan diagnostics for stub tests.
struct ParticleGpuFramePlanPreflight {
    u32 capacity = 0u;
    u32 emit_count = 0u;
    u32 alive_count = 0u;
    bool sim_covers_capacity = false;
    bool emit_covers_count = false;
    bool skip_sim = true;
    bool skip_emit = true;
    u32 sim_padding_threads = 0u;
    u32 emit_padding_threads = 0u;

    [[nodiscard]] bool isIdle() const { return skip_sim && skip_emit; }
    [[nodiscard]] bool ready() const {
        return buffers_ok && (skip_sim || sim_covers_capacity) && (skip_emit || emit_covers_count);
    }
};

/// Per-frame GPU stub plan: buffer sizing, dispatch counts, and empty-launch guards.
struct ParticleGpuFramePlan {
    ParticleGpuBuffers buffers{};
    ParticleGpuDispatch dispatch{};
    u32 capacity = 0u;
    u32 emit_count = 0u;
    u32 alive_count = 0u;

    [[nodiscard]] static ParticleGpuFramePlan forStub(u32 particle_capacity, u32 emit_count, u32 alive_count);
    [[nodiscard]] static ParticleGpuFramePlanPreflight preflightStub(u32 particle_capacity, u32 emit_count,
                                                                      u32 alive_count);
    [[nodiscard]] ParticleGpuFramePlanPreflight preflight() const;
    [[nodiscard]] bool skipSimLaunch() const;
    [[nodiscard]] bool skipEmitLaunch() const;
    [[nodiscard]] bool isIdle() const { return skipSimLaunch() && skipEmitLaunch(); }
    [[nodiscard]] bool buffersSizedForCapacity() const;
    [[nodiscard]] u32 remainingEmitSlots() const;
    [[nodiscard]] u32 clampedEmitCount() const;
    [[nodiscard]] bool shouldSkipSimWithZeroAlive() const { return alive_count == 0u; }
    [[nodiscard]] bool shouldSkipEmitAtCapacity() const { return capacity == 0u || alive_count >= capacity; }
    [[nodiscard]] bool shouldSkipEmitOverflow() const;
    [[nodiscard]] u32 simPaddingThreadCount() const;
    [[nodiscard]] u32 emitPaddingThreadCount() const;
    [[nodiscard]] ParticleGpuDispatchPreflight dispatchPreflight() const;
    [[nodiscard]] ParticleGpuFramePreflight preflight() const;
    [[nodiscard]] ParticleSoAGPU gpuPointers(u64 packed_device_address) const;
    [[nodiscard]] FramePlanPreflight preflight() const;
};

[[nodiscard]] bool should_skip_sim_dispatch(u32 capacity);
[[nodiscard]] bool should_skip_emit_dispatch(u32 emit_count);
[[nodiscard]] bool should_skip_mirror_sync(const ParticleGpuMirror& mirror, const ParticleSoA& cpu);
[[nodiscard]] bool should_skip_mirror_write(const ParticleGpuMirror& mirror, const ParticleSoA& cpu);
[[nodiscard]] bool should_skip_frame_sim(const ParticleGpuFramePlan& plan);
[[nodiscard]] bool should_skip_frame_emit(const ParticleGpuFramePlan& plan);
[[nodiscard]] bool should_skip_empty_buffer(const ParticleGpuBuffers& buffers);
[[nodiscard]] bool should_skip_device_upload(const ParticleGpuMirror& mirror, const ParticleSoA& cpu);

namespace particle_gpu_util {
[[nodiscard]] u32 gridDimX(u32 element_count, u32 block_size);
[[nodiscard]] u32 coveredThreadCount(u32 block_count, u32 block_size);
[[nodiscard]] u32 paddingThreads(u32 element_count, u32 block_count, u32 block_size);
[[nodiscard]] bool threadCoversElement(u32 thread_index, u32 element_count);
[[nodiscard]] bool isPaddingThread(u32 thread_index, u32 element_count);
[[nodiscard]] u32 blockIndexOf(u32 global_thread_index, u32 block_size);
[[nodiscard]] u32 localThreadIndex(u32 global_thread_index, u32 block_size);
[[nodiscard]] u32 globalThreadIndex(u32 block_index, u32 local_thread_index, u32 block_size);
[[nodiscard]] DispatchPreflight preflight_dispatch(u32 capacity, u32 emit_count);
[[nodiscard]] FramePlanPreflight preflight_frame_plan(u32 capacity, u32 emit_count, u32 alive_count);
[[nodiscard]] ParticleGpuSlotOffsetPreflight preflight_slot_offset(ParticleGpuColumn column, u32 capacity, u32 slot);
[[nodiscard]] u32 elementIndexForThread(u32 global_thread_index, u32 element_count);
[[nodiscard]] bool threadHasValidElement(u32 global_thread_index, u32 element_count);
[[nodiscard]] u32 slotIndexFromGlobalThread(u32 global_thread_index, u32 element_count);
[[nodiscard]] bool globalThreadCoversSlot(u32 global_thread_index, u32 element_count);
} // namespace particle_gpu_util

} // namespace fuse::vfx
