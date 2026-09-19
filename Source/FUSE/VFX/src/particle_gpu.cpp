#include <fuse/vfx/particle_gpu.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::vfx {

namespace {

bool vec3Equal(const math::Vec3& lhs, const math::Vec3& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

usize alignUp(usize value, usize alignment) {
    if (alignment == 0u) {
        return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

u32 ParticleGpuBufferLayout::columnCount() { return 8u; }

u32 ParticleGpuBufferLayout::columnIndex(ParticleGpuColumn column) {
    return static_cast<u32>(column);
}

usize ParticleGpuBufferLayout::columnAlignment() { return kColumnAlignment; }

usize ParticleGpuBufferLayout::elementSize(ParticleGpuColumn column) {
    switch (column) {
    case ParticleGpuColumn::Positions:
    case ParticleGpuColumn::Velocities:
    case ParticleGpuColumn::Colors:
        return sizeof(math::Vec3);
    case ParticleGpuColumn::Ages:
    case ParticleGpuColumn::Lifetimes:
    case ParticleGpuColumn::Sizes:
    case ParticleGpuColumn::Alphas:
        return sizeof(f32);
    case ParticleGpuColumn::AliveFlags:
        return sizeof(u32);
    }
    return 0u;
}

usize ParticleGpuBufferLayout::columnByteSize(ParticleGpuColumn column, u32 capacity) {
    return elementSize(column) * static_cast<usize>(capacity);
}

ParticleGpuColumnSpan ParticleGpuBufferLayout::columnSpan(ParticleGpuColumn column, u32 capacity) {
    ParticleGpuColumnSpan span{};
    span.offset = columnDeviceOffset(column, capacity);
    span.element_size = elementSize(column);
    span.byte_size = columnByteSize(column, capacity);
    span.slot_count = capacity;
    return span;
}

usize ParticleGpuBufferLayout::columnDeviceOffset(ParticleGpuColumn column, u32 capacity) {
    usize offset = 0u;
    for (u32 index = 0; index < columnCount(); ++index) {
        const ParticleGpuColumn current = static_cast<ParticleGpuColumn>(index);
        if (current == column) {
            return offset;
        }
        offset = alignUp(offset + columnByteSize(current, capacity), kColumnAlignment);
    }
    return offset;
}

usize ParticleGpuBufferLayout::paddingAfterColumn(ParticleGpuColumn column, u32 capacity) {
    const usize column_end = columnDeviceOffset(column, capacity) + columnByteSize(column, capacity);
    if (column == ParticleGpuColumn::AliveFlags) {
        return packedDeviceBytes(capacity) - column_end;
    }

    const u32 next_index = static_cast<u32>(column) + 1u;
    if (next_index >= columnCount()) {
        return 0u;
    }
    const ParticleGpuColumn next_column = static_cast<ParticleGpuColumn>(next_index);
    const usize next_offset = columnDeviceOffset(next_column, capacity);
    return next_offset > column_end ? next_offset - column_end : 0u;
}

bool ParticleGpuBufferLayout::isColumnOffsetAligned(ParticleGpuColumn column, u32 capacity) {
    if (capacity == 0u) {
        return false;
    }
    const usize offset = columnDeviceOffset(column, capacity);
    return offset % kColumnAlignment == 0u;
}

bool ParticleGpuBufferLayout::validateColumnSpanChain(u32 capacity) {
    if (!validatePackedLayout(capacity)) {
        return false;
    }

    const std::array<ParticleGpuColumnSpan, 8> spans = collectColumnSpans(capacity);
    usize previous_end = 0u;
    for (const ParticleGpuColumnSpan& span : spans) {
        if (span.isEmpty()) {
            return false;
        }
        if (span.offset < previous_end) {
            return false;
        }
        if (span.offset % kColumnAlignment != 0u) {
            return false;
        }
        if (span.endOffset() > packedDeviceBytes(capacity)) {
            return false;
        }
        previous_end = span.endOffset();
    }
    return previous_end <= packedDeviceBytes(capacity);
}

std::array<ParticleGpuColumnSpan, 8> ParticleGpuBufferLayout::collectColumnSpans(u32 capacity) {
    std::array<ParticleGpuColumnSpan, 8> spans{};
    for (u32 index = 0; index < columnCount(); ++index) {
        spans[index] = columnSpan(static_cast<ParticleGpuColumn>(index), capacity);
    }
    return spans;
}

const char* ParticleGpuBufferLayout::slotGuardName(ParticleGpuSlotGuard guard) {
    switch (guard) {
    case ParticleGpuSlotGuard::Ok:
        return "ok";
    case ParticleGpuSlotGuard::OutOfRange:
        return "out_of_range";
    case ParticleGpuSlotGuard::InvalidCapacity:
        return "invalid_capacity";
    }
    return "unknown";
}

bool ParticleGpuBufferLayout::isSlotInRange(u32 slot_index, u32 capacity) {
    return capacity > 0u && slot_index < capacity;
}

ParticleGpuSlotGuard ParticleGpuBufferLayout::slotGuard(u32 slot_index, u32 capacity) {
    if (capacity == 0u) {
        return ParticleGpuSlotGuard::InvalidCapacity;
    }
    if (slot_index >= capacity) {
        return ParticleGpuSlotGuard::OutOfRange;
    }
    return ParticleGpuSlotGuard::Ok;
}

usize ParticleGpuBufferLayout::slotElementByteOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity) {
    if (slotGuard(slot_index, capacity) != ParticleGpuSlotGuard::Ok) {
        return 0u;
    }
    return static_cast<usize>(slot_index) * elementSize(column);
}

usize ParticleGpuBufferLayout::slotColumnByteOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity) {
    if (slotGuard(slot_index, capacity) != ParticleGpuSlotGuard::Ok) {
        return 0u;
    }
    return columnDeviceOffset(column, capacity) + slotElementByteOffset(column, slot_index, capacity);
}

const char* ParticleGpuBufferLayout::syncGuardName(ParticleGpuSyncGuard guard) {
    switch (guard) {
    case ParticleGpuSyncGuard::Ok:
        return "ok";
    case ParticleGpuSyncGuard::MirrorUninitialized:
        return "mirror_uninitialized";
    case ParticleGpuSyncGuard::CpuUninitialized:
        return "cpu_uninitialized";
    case ParticleGpuSyncGuard::CapacityMismatch:
        return "capacity_mismatch";
    }
    return "unknown";
}

const char* ParticleGpuBufferLayout::slotOffsetGuardName(ParticleGpuSlotOffsetGuard guard) {
    switch (guard) {
    case ParticleGpuSlotOffsetGuard::Ok:
        return "ok";
    case ParticleGpuSlotOffsetGuard::UninitializedCapacity:
        return "uninitialized_capacity";
    case ParticleGpuSlotOffsetGuard::SlotOutOfRange:
        return "slot_out_of_range";
const char* ParticleGpuBufferLayout::packGuardName(ParticleGpuPackGuard guard) {
    case ParticleGpuPackGuard::Ok:
    case ParticleGpuPackGuard::MirrorUninitialized:
        return "mirror_uninitialized";
    case ParticleGpuPackGuard::AliveCountMismatch:
        return "alive_count_mismatch";
    }
    return "unknown";
}

const char* ParticleGpuBufferLayout::columnName(ParticleGpuColumn column) {
    switch (column) {
    case ParticleGpuColumn::Positions:
        return "positions";
    case ParticleGpuColumn::Velocities:
        return "velocities";
    case ParticleGpuColumn::Ages:
        return "ages";
    case ParticleGpuColumn::Lifetimes:
        return "lifetimes";
    case ParticleGpuColumn::Sizes:
        return "sizes";
    case ParticleGpuColumn::Colors:
        return "colors";
    case ParticleGpuColumn::Alphas:
        return "alphas";
    case ParticleGpuColumn::AliveFlags:
        return "alive_flags";
    }
    return "unknown";
}

usize ParticleGpuBufferLayout::packedDeviceBytes(u32 capacity) {
    usize total = 0u;
    for (u32 index = 0; index < columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        total = alignUp(total + columnByteSize(column, capacity), kColumnAlignment);
    }
    return total;
}

usize ParticleGpuBufferLayout::dataColumnBytes(u32 capacity) {
    usize total = 0u;
    for (u32 index = 0; index < columnCount(); ++index) {
        total += columnByteSize(static_cast<ParticleGpuColumn>(index), capacity);
    }
    return total;
}

usize ParticleGpuBufferLayout::packingOverheadBytes(u32 capacity) {
    const usize packed = packedDeviceBytes(capacity);
    const usize raw = dataColumnBytes(capacity);
    return packed >= raw ? packed - raw : 0u;
}

u64 ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn column, u64 base, u32 capacity) {
    if (base == 0u || capacity == 0u) {
        return 0u;
    }
    return base + columnDeviceOffset(column, capacity);
}

bool ParticleGpuBufferLayout::validateSlotIndex(u32 slot_index, u32 capacity) {
    return capacity > 0u && slot_index < capacity;
}

bool ParticleGpuBufferLayout::isSlotOffsetAligned(ParticleGpuColumn column, u32 slot_index, u32 capacity) {
    if (!validateSlotIndex(slot_index, capacity)) {
        return false;
    }
    const usize element_size = elementSize(column);
    if (element_size == 0u) {
    const usize column_relative = element_size * static_cast<usize>(slot_index);
    return column_relative + element_size <= columnByteSize(column, capacity);

bool ParticleGpuBufferLayout::validateSlotDeviceOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity) {
    if (!validateSlotIndex(slot_index, capacity)) {
        return false;
    }
    const usize offset = slotDeviceOffset(column, slot_index, capacity);
    return containsByteOffset(offset, capacity);
    if (offset == 0u && slot_index != 0u) {
        return false;
    }
    if (!containsByteOffset(offset, capacity)) {

    ParticleGpuColumnSpan span{};
    if (!locateColumnAtOffset(offset, capacity, &span)) {
    return span.element_size == elementSize(column) && isSlotOffsetAligned(column, slot_index, capacity);

ParticleGpuSlotPreflight ParticleGpuBufferLayout::preflightSlotAccess(ParticleGpuColumn column, u32 slot_index,
                                                                       u32 capacity) {
    ParticleGpuSlotPreflight preflight{};
    preflight.column = column;
    preflight.slot_index = slot_index;
    preflight.capacity = capacity;
    preflight.slot_in_bounds = validateSlotIndex(slot_index, capacity);
    preflight.offset_aligned = isSlotOffsetAligned(column, slot_index, capacity);
    preflight.device_offset = slotDeviceOffset(column, slot_index, capacity);
    preflight.offset_in_column = preflight.slot_in_bounds && preflight.device_offset >= columnDeviceOffset(column, capacity) &&
                                 preflight.device_offset <
                                     columnDeviceOffset(column, capacity) + columnByteSize(column, capacity);
    return preflight;
}

usize ParticleGpuBufferLayout::slotDeviceOffset(ParticleGpuColumn column, u32 slot_index, u32 capacity) {
    if (!validateSlotIndex(slot_index, capacity)) {
        return 0u;
    }
    return columnDeviceOffset(column, capacity) + elementSize(column) * static_cast<usize>(slot_index);
}

u32 ParticleGpuBufferLayout::slotIndexFromColumnOffset(ParticleGpuColumn column, usize column_byte_offset,
                                                       u32 capacity) {
    if (capacity == 0u) {
        return capacity;
    }
    const usize element_bytes = elementSize(column);
    if (element_bytes == 0u || column_byte_offset % element_bytes != 0u) {
    const u32 slot_index = static_cast<u32>(column_byte_offset / element_bytes);
    return validateSlotIndex(slot_index, capacity) ? slot_index : capacity;

bool ParticleGpuBufferLayout::packedBytesFitCapacity(const std::vector<u8>& bytes, u32 capacity) {
        return false;
    return bytes.size() >= packedDeviceBytes(capacity);
u64 ParticleGpuBufferLayout::slotDeviceAddress(ParticleGpuColumn column, u64 base, u32 slot_index, u32 capacity) {
    if (base == 0u || !validateSlotIndex(slot_index, capacity)) {
        return 0u;
    return base + slotDeviceOffset(column, slot_index, capacity);

bool ParticleGpuBufferLayout::validateSlotDeviceAddress(u64 address, u64 packed_base, ParticleGpuColumn column,
                                                        u32 slot_index, u32 capacity) {
    if (address == 0u || packed_base == 0u) {
    return address == slotDeviceAddress(column, packed_base, slot_index, capacity);

bool ParticleGpuBufferLayout::locateSlotAtByteOffset(usize byte_offset, u32 capacity, ParticleGpuColumn* out_column,
                                                      u32* out_slot_index) {
    if (out_column == nullptr || out_slot_index == nullptr || !containsByteOffset(byte_offset, capacity)) {

    ParticleGpuColumnSpan span{};
    if (!locateColumnAtOffset(byte_offset, capacity, &span)) {

    if (span.element_size == 0u) {

    const usize slot_offset = byte_offset - span.offset;
    if (slot_offset % span.element_size != 0u) {

    const u32 slot_index = static_cast<u32>(slot_offset / span.element_size);
    if (!validateSlotIndex(slot_index, capacity)) {

    for (u32 index = 0; index < columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        if (columnSpan(column, capacity).offset == span.offset) {
            *out_column = column;
            *out_slot_index = slot_index;
            return true;
u32 ParticleGpuBufferLayout::slotIndexAtColumnOffset(ParticleGpuColumn column, usize column_relative_offset,
    const usize element_size = elementSize(column);
    if (element_size == 0u || column_relative_offset % element_size != 0u) {
    const u32 slot_index = static_cast<u32>(column_relative_offset / element_size);

bool ParticleGpuBufferLayout::locateSlotAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumn* out_column,

    if (!locateColumnAtOffset(byte_offset, capacity, &span) || span.element_size == 0u) {

    const usize column_relative = byte_offset - span.offset;
    if (column_relative >= span.byte_size) {
    if (column_relative % span.element_size != 0u) {

    const u32 slot_index = static_cast<u32>(column_relative / span.element_size);

        if (columnDeviceOffset(column, capacity) == span.offset && elementSize(column) == span.element_size) {
}

bool ParticleGpuBufferLayout::containsByteOffset(usize byte_offset, u32 capacity) {
    if (capacity == 0u) {
        return false;
    }
    return byte_offset < packedDeviceBytes(capacity);
}

bool ParticleGpuBufferLayout::locateColumnAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumnSpan* out_span) {
    if (out_span == nullptr || !containsByteOffset(byte_offset, capacity)) {
        return false;
    }

    for (u32 index = 0; index < columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        const ParticleGpuColumnSpan span = columnSpan(column, capacity);
        if (byte_offset >= span.offset && byte_offset < span.endOffset()) {
            *out_span = span;
            return true;
        }
    }
    return false;
}

bool ParticleGpuBufferLayout::locateSlotAtOffset(usize byte_offset, u32 capacity, ParticleGpuColumn* out_column,
                                                 u32* out_slot) {
    if (out_column == nullptr || out_slot == nullptr) {
        return false;
    }

    ParticleGpuColumnSpan span{};
    if (!locateColumnAtOffset(byte_offset, capacity, &span)) {
        return false;
    }

    const usize column_byte_offset = byte_offset - span.offset;
    if (column_byte_offset % span.element_size != 0u) {
        return false;
    }

    const u32 slot_index = static_cast<u32>(column_byte_offset / span.element_size);
    if (!validateSlotIndex(slot_index, capacity)) {
        return false;
    }

    for (u32 index = 0; index < columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        if (columnSpan(column, capacity).offset == span.offset) {
            *out_column = column;
            *out_slot = slot_index;
            return true;
        }
    }
    return false;
}

bool ParticleGpuBufferLayout::validatePackedLayout(u32 capacity) {
    if (capacity == 0u) {
        return false;
    }

    usize cursor = 0u;
    for (u32 index = 0; index < columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        if (columnDeviceOffset(column, capacity) != cursor) {
            return false;
        }
        const usize column_bytes = columnByteSize(column, capacity);
        if (column_bytes == 0u) {
            return false;
        }
        cursor = alignUp(cursor + column_bytes, kColumnAlignment);
    }
    return cursor == packedDeviceBytes(capacity);
}

bool ParticleGpuBufferLayout::isValidSlotIndex(u32 slot, u32 capacity) {
    return capacity > 0u && slot < capacity;
}

ParticleGpuSlotOffsetGuard ParticleGpuBufferLayout::slotOffsetGuard(u32 slot, u32 capacity) {
    if (capacity == 0u) {
        return ParticleGpuSlotOffsetGuard::UninitializedCapacity;
    }
    if (slot >= capacity) {
        return ParticleGpuSlotOffsetGuard::SlotOutOfRange;
    }
    return ParticleGpuSlotOffsetGuard::Ok;
}

usize ParticleGpuBufferLayout::columnSlotByteOffset(ParticleGpuColumn column, u32 capacity, u32 slot) {
    if (!isValidSlotIndex(slot, capacity)) {
        return 0u;
    }
    return columnDeviceOffset(column, capacity) + elementSize(column) * static_cast<usize>(slot);
}

u64 ParticleGpuBufferLayout::columnSlotDeviceAddress(ParticleGpuColumn column, u64 base, u32 capacity, u32 slot) {
    if (base == 0u || !isValidSlotIndex(slot, capacity)) {
        return 0u;
    }
    return base + columnSlotByteOffset(column, capacity, slot);
}

bool ParticleGpuBufferLayout::canAccessSlotAtOffset(ParticleGpuColumn column, u32 capacity, u32 slot) {
    if (slotOffsetGuard(slot, capacity) != ParticleGpuSlotOffsetGuard::Ok) {
        return false;
    }
    const usize slot_end = columnSlotByteOffset(column, capacity, slot) + elementSize(column);
    return slot_end <= packedDeviceBytes(capacity);
}

ParticleGpuDispatch ParticleGpuDispatch::forSimulate(u32 capacity) {
    ParticleGpuDispatch dispatch{};
    dispatch.simThreadCount = ParticleGpuBufferLayout::kSimBlockSize;
    dispatch.simBlockCount = particle_gpu_util::gridDimX(capacity, dispatch.simThreadCount);
    return dispatch;
}

ParticleGpuDispatch ParticleGpuDispatch::forEmit(u32 emit_count) {
    ParticleGpuDispatch dispatch{};
    dispatch.emitThreadCount = ParticleGpuBufferLayout::kEmitBlockSize;
    dispatch.emitBlockCount = emit_count == 0u ? 0u : particle_gpu_util::gridDimX(emit_count, dispatch.emitThreadCount);
    return dispatch;
}

ParticleGpuDispatch ParticleGpuDispatch::forFrame(u32 capacity, u32 emit_count) {
    ParticleGpuDispatch dispatch = forSimulate(capacity);
    const ParticleGpuDispatch emit_dispatch = forEmit(emit_count);
    dispatch.emitBlockCount = emit_dispatch.emitBlockCount;
    dispatch.emitThreadCount = emit_dispatch.emitThreadCount;
    return dispatch;
}

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflightSimulate(u32 capacity) {
    ParticleGpuDispatchPreflight preflight{};
    preflight.element_count = capacity;
    preflight.block_size = ParticleGpuBufferLayout::kSimBlockSize;

    if (capacity == 0u) {
        preflight.skipped = true;
        return preflight;
    }

    const ParticleGpuDispatch dispatch = forSimulate(capacity);
    preflight.block_count = dispatch.simBlockCount;
    preflight.covered_threads = dispatch.totalSimThreads();
    preflight.padding_threads = dispatch.simPaddingThreads(capacity);
    preflight.skipped = dispatch.shouldSkipSimLaunch(capacity);
    return preflight;
}

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflightEmit(u32 emit_count) {
    ParticleGpuDispatchPreflight preflight{};
    preflight.element_count = emit_count;
    preflight.block_size = ParticleGpuBufferLayout::kEmitBlockSize;

    if (emit_count == 0u) {
        preflight.skipped = true;
        return preflight;
    }

    const ParticleGpuDispatch dispatch = forEmit(emit_count);
    preflight.block_count = dispatch.emitBlockCount;
    preflight.covered_threads = dispatch.totalEmitThreads();
    preflight.padding_threads = dispatch.emitPaddingThreads(emit_count);
    preflight.skipped = dispatch.shouldSkipEmitLaunch(emit_count);
    return preflight;
}

ParticleGpuDispatchPreflight ParticleGpuDispatch::simPreflight(u32 slot_count) const {
    ParticleGpuDispatchPreflight preflight{};
    preflight.element_count = slot_count;
    preflight.block_size = simThreadCount;
    preflight.block_count = simBlockCount;
    preflight.covered_threads = totalSimThreads();
    preflight.padding_threads = simPaddingThreads(slot_count);
    preflight.skipped = shouldSkipSimLaunch(slot_count);
    return preflight;
}

ParticleGpuDispatchPreflight ParticleGpuDispatch::emitPreflight(u32 emit_count) const {
    ParticleGpuDispatchPreflight preflight{};
    preflight.element_count = emit_count;
    preflight.block_size = emitThreadCount;
    preflight.block_count = emitBlockCount;
    preflight.covered_threads = totalEmitThreads();
    preflight.padding_threads = emitPaddingThreads(emit_count);
    preflight.skipped = shouldSkipEmitLaunch(emit_count);
    return preflight;
}

u32 ParticleGpuDispatch::simPaddingThreads(u32 capacity) const {
    return particle_gpu_util::paddingThreads(capacity, simBlockCount, simThreadCount);
}

u32 ParticleGpuDispatch::emitPaddingThreads(u32 emit_count) const {
    return particle_gpu_util::paddingThreads(emit_count, emitBlockCount, emitThreadCount);
}

u32 ParticleGpuDispatch::firstSimPaddingThread(u32 slot_count) const {
    if (slot_count == 0u || !hasSimLaunch()) {
        return 0u;
    }
    return slot_count;
}

u32 ParticleGpuDispatch::firstEmitPaddingThread(u32 emit_count) const {
    if (emit_count == 0u || !hasEmitLaunch()) {
        return 0u;
    }
    return emit_count;
}

bool ParticleGpuDispatch::isSimPaddingThread(u32 global_thread_index, u32 slot_count) const {
    if (shouldSkipSimLaunch(slot_count)) {
        return false;
    }
    return particle_gpu_util::isPaddingThread(global_thread_index, slot_count);
}

bool ParticleGpuDispatch::isEmitPaddingThread(u32 global_thread_index, u32 emit_count) const {
    if (shouldSkipEmitLaunch(emit_count)) {
        return false;
    }
    return particle_gpu_util::isPaddingThread(global_thread_index, emit_count);
}

bool ParticleGpuDispatch::simPaddingAccountsFor(u32 slot_count) const {
    if (!hasSimLaunch()) {
        return slot_count == 0u;
    }
    if (slot_count == 0u) {
        return false;
    return totalSimThreads() == slot_count + simPaddingThreads(slot_count);

bool ParticleGpuDispatch::emitPaddingAccountsFor(u32 emit_count) const {
    if (emit_count == 0u) {
        return true;
    if (!hasEmitLaunch()) {
    return totalEmitThreads() == emit_count + emitPaddingThreads(emit_count);

bool ParticleGpuDispatchPreflight::can_launch_sim() const {
    return !skip_sim_launch && sim_covers && sim_padding_ok;

bool ParticleGpuDispatchPreflight::can_launch_emit() const {
    return !skip_emit_launch && emit_covers && emit_padding_ok;

bool ParticleGpuDispatchPreflight::ready_for_stub() const {
    const bool sim_ready = skip_sim_launch || can_launch_sim();
    const bool emit_ready = skip_emit_launch || can_launch_emit();
    const bool sim_padding_valid = slot_count == 0u || sim_padding_ok;
    const bool emit_padding_valid = emit_count == 0u || emit_padding_ok;
    return sim_ready && emit_ready && sim_padding_valid && emit_padding_valid;

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflightSimulate(u32 slot_count) const {
    ParticleGpuDispatchPreflight preflight{};
    preflight.slot_count = slot_count;
    preflight.skip_sim_launch = shouldSkipSimLaunch(slot_count);
    preflight.sim_covers = simCovers(slot_count);
    preflight.sim_padding_ok = simPaddingAccountsFor(slot_count);
    preflight.skip_emit_launch = true;
    preflight.emit_covers = true;
    preflight.emit_padding_ok = emitPaddingAccountsFor(0u);
    return preflight;

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflightEmit(u32 emit_count) const {
    preflight.emit_count = emit_count;
    preflight.skip_emit_launch = shouldSkipEmitLaunch(emit_count);
    preflight.emit_covers = emitCovers(emit_count);
    preflight.emit_padding_ok = emitPaddingAccountsFor(emit_count);
    preflight.skip_sim_launch = true;
    preflight.sim_covers = true;
    preflight.sim_padding_ok = simPaddingAccountsFor(0u);

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflightFrame(u32 slot_count, u32 emit_count) const {
    ParticleGpuDispatchPreflight preflight = preflightSimulate(slot_count);
    const ParticleGpuDispatchPreflight emit_preflight = preflightEmit(emit_count);
    preflight.emit_count = emit_preflight.emit_count;
    preflight.skip_emit_launch = emit_preflight.skip_emit_launch;
    preflight.emit_covers = emit_preflight.emit_covers;
    preflight.emit_padding_ok = emit_preflight.emit_padding_ok;
DispatchPreflight ParticleGpuDispatch::preflight(u32 capacity, u32 emit_count) const {
    DispatchPreflight result{};
    result.capacity = capacity;
    result.emit_count = emit_count;
    result.skip_sim = shouldSkipSimLaunch(capacity);
    result.skip_emit = shouldSkipEmitLaunch(emit_count);
    result.sim_padding_threads = simPaddingThreads(capacity);
    result.emit_padding_threads = emitPaddingThreads(emit_count);
    result.sim_covers = simCovers(capacity);
    result.emit_covers = emitCovers(emit_count);
    result.is_idle = result.skip_sim && result.skip_emit;
    return result;
}

u32 ParticleGpuDispatch::simSlotForThread(u32 global_thread_index, u32 slot_count) const {
    return particle_gpu_util::elementIndexForThread(global_thread_index, slot_count);
}

u32 ParticleGpuDispatch::emitIndexForThread(u32 global_thread_index, u32 emit_count) const {
    return particle_gpu_util::elementIndexForThread(global_thread_index, emit_count);
}

bool ParticleGpuDispatch::simThreadHasValidSlot(u32 global_thread_index, u32 slot_count) const {
    if (shouldSkipSimLaunch(slot_count)) {
        return false;
    }
    return particle_gpu_util::threadHasValidElement(global_thread_index, slot_count);
}

bool ParticleGpuDispatch::emitThreadHasValidIndex(u32 global_thread_index, u32 emit_count) const {
    if (shouldSkipEmitLaunch(emit_count)) {
        return false;
    }
    return particle_gpu_util::threadHasValidElement(global_thread_index, emit_count);
}

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflight(u32 capacity, u32 emit_count) const {
    ParticleGpuDispatchPreflight result{};
    result.sim_covers_capacity = simCovers(capacity);
    result.emit_covers_count = emitCovers(emit_count);
    result.sim_will_skip_launch = shouldSkipSimLaunch(capacity);
    result.emit_will_skip_launch = shouldSkipEmitLaunch(emit_count);
    result.sim_padding_ok = simPaddingAccountsFor(capacity);
    result.emit_padding_ok = emitPaddingAccountsFor(emit_count);
    return result;
}

bool ParticleGpuDispatchPreflight::ready_for_stub() const {
    return sim_covers_capacity && emit_covers_count && sim_padding_ok && emit_padding_ok;
}

bool ParticleGpuDispatchPreflight::ready_for_stub(u32 slot_count, u32 emit_count) const {
    if (slot_count == 0u && emit_count == 0u) {
        return sim_skip_ok && emit_skip_ok;
    }
    if (slot_count > 0u) {
        if (!sim_covers || !sim_padding_ok) {
            return false;
        }
    } else if (!sim_skip_ok) {
        return false;
    }
    if (emit_count > 0u) {
        return emit_covers && emit_padding_ok;
    }
    return emit_skip_ok;
}

ParticleGpuDispatchPreflight ParticleGpuDispatch::preflight(u32 slot_count, u32 emit_count) const {
    ParticleGpuDispatchPreflight result{};
    result.sim_covers = simCovers(slot_count);
    result.emit_covers = emitCovers(emit_count);
    result.sim_skip_ok = shouldSkipSimLaunch(slot_count) == (slot_count == 0u);
    result.emit_skip_ok = shouldSkipEmitLaunch(emit_count) == (emit_count == 0u);
    result.sim_padding_ok = simPaddingAccountsFor(slot_count);
    result.emit_padding_ok = emitPaddingAccountsFor(emit_count);
    return result;
}

bool ParticleGpuBuffersPreflight::can_bind() const {
    return capacity_ok && bytes_ok;
}

bool ParticleGpuBuffersPreflight::is_empty_capacity() const {
    return !capacity_ok;
}

bool ParticleGpuEmitGuard::can_emit() const {
    return !skip_emit && clamped > 0u;
}

ParticleGpuBuffers ParticleGpuBuffers::forCapacity(u32 particle_capacity) {
    ParticleGpuBuffers buffers{};
    buffers.capacity = particle_capacity;
    buffers.deviceBytes = ParticleGpuBufferLayout::packedDeviceBytes(particle_capacity);
    return buffers;
}

bool ParticleGpuBufferPreflight::can_allocate() const {
    return capacity_ok && device_bytes_ok && !is_empty;
}

bool ParticleGpuBufferPreflight::ready_for_stub() const {
    return can_allocate();

ParticleGpuBufferPreflight ParticleGpuBuffers::preflight() const {
    ParticleGpuBufferPreflight result{};
    result.capacity_ok = capacity > 0u;
    result.device_bytes_ok = deviceBytes > 0u &&
                             deviceBytes == ParticleGpuBufferLayout::packedDeviceBytes(capacity);
    result.is_empty = isEmpty();
    result.has_binding = hasDeviceBinding();
    return result;
bool ParticleGpuBuffers::bytesMatchLayout() const {
    if (isEmpty()) {
        return false;
    return deviceBytes == ParticleGpuBufferLayout::packedDeviceBytes(capacity);

    ParticleGpuBufferPreflight preflight{};
    preflight.capacity = capacity;
    preflight.device_bytes = deviceBytes;
    preflight.bound = isBound();
        preflight.layout_bytes_ok = false;
        return preflight;
    preflight.layout_bytes_ok =
        ParticleGpuBufferLayout::validatePackedLayout(capacity) && bytesMatchLayout();
ParticleGpuBuffersPreflight ParticleGpuBuffers::preflight() const {
    ParticleGpuBuffersPreflight result{};
    result.bytes_ok = deviceBytes == ParticleGpuBufferLayout::packedDeviceBytes(capacity);
    result.bound = packedSoa != 0u;
}

void ParticleGpuMirror::reserve(u32 particle_capacity) {
    capacity = particle_capacity;
    positions.assign(particle_capacity, {});
    velocities.assign(particle_capacity, {});
    ages.assign(particle_capacity, 0.f);
    lifetimes.assign(particle_capacity, 0.f);
    sizes.assign(particle_capacity, 0.f);
    colors.assign(particle_capacity, {});
    alphas.assign(particle_capacity, 0.f);
    alive_flags.assign(particle_capacity, 0u);
    alive_count = 0;
}

void ParticleGpuMirror::clear() {
    for (u32 index = 0; index < capacity; ++index) {
        ages[index] = 0.f;
        lifetimes[index] = 0.f;
        sizes[index] = 0.f;
        alphas[index] = 0.f;
        alive_flags[index] = 0u;
    }
    alive_count = 0;
}

bool ParticleGpuSlotPreflight::ready_for_stub_access() const {
    return slot_in_bounds && offset_in_column && offset_aligned;
}

bool ParticleGpuMirrorPackPreflight::can_pack() const {
    return mirror.can_bind_device() && layout_bytes_ok;
}

bool ParticleGpuMirrorPreflight::can_sync_from_cpu() const {
    return sync_guard == ParticleGpuSyncGuard::Ok;
}

bool ParticleGpuMirrorPreflight::can_write_to_cpu() const {
    return write_guard == ParticleGpuSyncGuard::Ok && alive_count_matches_flags;
}

bool ParticleGpuMirrorPreflight::can_pack() const {
    return sync_guard != ParticleGpuSyncGuard::MirrorUninitialized &&
           sync_guard != ParticleGpuSyncGuard::CpuUninitialized && alive_count_matches_flags;
}

bool ParticleGpuMirrorPreflight::can_unpack(const std::vector<u8>& bytes, u32 particle_capacity) const {
    return can_pack() && packed_layout_ok && ParticleGpuBufferLayout::packedBytesFitCapacity(bytes, particle_capacity);

bool ParticleGpuMirrorPreflight::needs_resize_sync() const {
    return sync_guard == ParticleGpuSyncGuard::MirrorUninitialized;

bool ParticleGpuMirrorPreflight::can_bind_device() const {
    return can_pack() && alive_count_matches_flags;

bool ParticleGpuMirrorUploadPreflight::can_upload() const {
    return mirror.can_bind_device() && layout_bytes_ok;
    return pack_guard == ParticleGpuPackGuard::Ok;

bool ParticleGpuMirrorPreflight::can_unpack(const std::vector<u8>& bytes, u32 capacity) const {
    if (capacity == 0u) {
        return false;
    if (sync_guard == ParticleGpuSyncGuard::MirrorUninitialized) {
    return bytes.size() >= ParticleGpuBufferLayout::packedDeviceBytes(capacity);
}

ParticleGpuSyncGuard ParticleGpuMirror::syncGuardForCpu(const ParticleSoA& cpu) const {
    if (cpu.capacity == 0u) {
        return ParticleGpuSyncGuard::CpuUninitialized;
    }
    if (capacity == 0u) {
        return ParticleGpuSyncGuard::MirrorUninitialized;
    }
    if (cpu.capacity != capacity) {
        return ParticleGpuSyncGuard::CapacityMismatch;
    }
    return ParticleGpuSyncGuard::Ok;
}

ParticleGpuSyncGuard ParticleGpuMirror::writeGuardForCpu(const ParticleSoA& cpu) const {
    if (capacity == 0u) {
        return ParticleGpuSyncGuard::MirrorUninitialized;
    }
    if (cpu.capacity == 0u) {
        return ParticleGpuSyncGuard::CpuUninitialized;
    }
    if (cpu.capacity != capacity) {
        return ParticleGpuSyncGuard::CapacityMismatch;
    }
    return ParticleGpuSyncGuard::Ok;
}

bool ParticleGpuMirror::canSyncFromCpuSoA(const ParticleSoA& cpu) const {
    if (syncGuardForCpu(cpu) == ParticleGpuSyncGuard::Ok) {
        return true;
    }
    if (cpu.capacity == 0u) {
        return false;
    }
    return capacity == 0u || cpu.capacity == capacity;
}

bool ParticleGpuMirror::canWriteToCpuSoA(const ParticleSoA& cpu) const {
    return writeGuardForCpu(cpu) == ParticleGpuSyncGuard::Ok;
}

ParticleGpuMirrorPreflight ParticleGpuMirror::preflightFromCpu(const ParticleSoA& cpu) const {
    ParticleGpuMirrorPreflight preflight{};
    preflight.sync_guard = syncGuardForCpu(cpu);
    preflight.write_guard = writeGuardForCpu(cpu);
    preflight.pack_guard = packGuard();
    preflight.alive_count_matches_flags = aliveCountMatchesFlags();
    preflight.already_synced = shouldSkipSyncFromCpu(cpu);
    preflight.packed_layout_ok = capacity > 0u && ParticleGpuBufferLayout::validatePackedLayout(capacity);
    return preflight;
}

ParticleGpuMirrorUploadPreflight ParticleGpuMirror::preflightDeviceUpload(const ParticleSoA& cpu) const {
    ParticleGpuMirrorUploadPreflight preflight{};
    preflight.mirror = preflightFromCpu(cpu);
    if (cpu.capacity == 0u) {
        preflight.layout_bytes_ok = false;
    preflight.layout_bytes_ok =
        ParticleGpuBufferLayout::validatePackedLayout(cpu.capacity) &&
        ParticleGpuBufferLayout::packedDeviceBytes(cpu.capacity) > 0u;

bool ParticleGpuMirror::shouldSkipSyncFromCpu(const ParticleSoA& cpu) const {
    return syncGuardForCpu(cpu) == ParticleGpuSyncGuard::Ok && matchesCpuSoA(cpu);
ParticleGpuPackGuard ParticleGpuMirror::packGuard() const {
    if (capacity == 0u) {
        return ParticleGpuPackGuard::MirrorUninitialized;
    }
    if (!aliveCountMatchesFlags()) {
        return ParticleGpuPackGuard::AliveCountMismatch;
    return ParticleGpuPackGuard::Ok;

bool ParticleGpuMirror::canUnpackFromDeviceLayout(const std::vector<u8>& bytes, u32 particle_capacity) const {
    if (particle_capacity != capacity || capacity == 0u) {
        return false;
    return bytes.size() >= ParticleGpuBufferLayout::packedDeviceBytes(capacity);

ParticleGpuMirrorPackPreflight ParticleGpuMirror::preflightPack() const {
    ParticleGpuMirrorPackPreflight preflight{};
    preflight.mirror.sync_guard = capacity == 0u ? ParticleGpuSyncGuard::MirrorUninitialized : ParticleGpuSyncGuard::Ok;
    preflight.mirror.write_guard = preflight.mirror.sync_guard;
    preflight.mirror.alive_count_matches_flags = aliveCountMatchesFlags();
    preflight.layout_bytes_ok =
        capacity > 0u && ParticleGpuBufferLayout::validatePackedLayout(capacity) &&
        ParticleGpuBufferLayout::packedDeviceBytes(capacity) > 0u;
    preflight.packed_bytes_fit = capacity == 0u || preflight.layout_bytes_ok;
    return preflight;
}

bool ParticleGpuMirror::validateSlotAccess(u32 slot_index) const {
    return ParticleGpuBufferLayout::validateSlotIndex(slot_index, capacity);
}

ParticleGpuSlotPreflight ParticleGpuMirror::preflightSlot(u32 slot_index, ParticleGpuColumn column) const {
    return ParticleGpuBufferLayout::preflightSlotAccess(column, slot_index, capacity);
}

bool ParticleGpuMirror::aliveCountMatchesFlags() const {
    if (capacity == 0u) {
        return alive_count == 0u;

    u32 flagged = 0u;
    for (u32 slot = 0; slot < capacity; ++slot) {
        if (alive_flags[slot] != 0u) {
            ++flagged;
    return flagged == alive_count;
ParticleGpuMirrorSyncPreflight ParticleGpuMirror::preflightSyncFromCpu(const ParticleSoA& cpu) const {
    ParticleGpuMirrorSyncPreflight preflight{};
    preflight.guard = syncGuardForCpu(cpu);
    preflight.needs_resize = capacity == 0u && cpu.capacity > 0u;
    preflight.can_sync = canSyncFromCpuSoA(cpu);
    preflight.can_write = false;

ParticleGpuMirrorSyncPreflight ParticleGpuMirror::preflightWriteToCpu(const ParticleSoA& cpu) const {
    preflight.guard = writeGuardForCpu(cpu);
    preflight.needs_resize = false;
    preflight.can_sync = false;
    preflight.can_write = canWriteToCpuSoA(cpu);

ParticleGpuSlotGuard ParticleGpuMirror::slotGuard(u32 slot_index) const {
    return ParticleGpuBufferLayout::slotGuard(slot_index, capacity);

bool ParticleGpuMirror::isSlotInRange(u32 slot_index) const {
    return ParticleGpuBufferLayout::isSlotInRange(slot_index, capacity);
MirrorPreflight ParticleGpuMirror::preflightSync(const ParticleSoA& cpu) const {
    MirrorPreflight result{};
    result.sync_guard = syncGuardForCpu(cpu);
    result.write_guard = writeGuardForCpu(cpu);
    result.can_sync = canSyncFromCpuSoA(cpu);
    result.can_write = canWriteToCpuSoA(cpu);
    result.would_resize = capacity == 0u && cpu.capacity > 0u;
    return result;
}

bool ParticleGpuMirror::trySyncFromCpuSoA(const ParticleSoA& cpu) {
    const ParticleGpuSyncGuard guard = syncGuardForCpu(cpu);
    if (guard == ParticleGpuSyncGuard::CpuUninitialized) {
        return false;
    }
    if (guard == ParticleGpuSyncGuard::CapacityMismatch && capacity != 0u) {
        return false;
    }

    syncFromCpuSoA(cpu);
    return true;
}

void ParticleGpuMirror::syncFromCpuSoA(const ParticleSoA& cpu) {
    if (cpu.capacity != capacity) {
        reserve(cpu.capacity);
    }

    positions = cpu.positions;
    velocities = cpu.velocities;
    ages = cpu.ages;
    lifetimes = cpu.lifetimes;
    sizes = cpu.sizes;
    colors = cpu.colors;
    alphas = cpu.alphas;
    alive_flags = cpu.alive_flags;
    alive_count = cpu.count;
}

ParticleGpuMirror ParticleGpuMirror::fromCpuSoA(const ParticleSoA& cpu) {
    ParticleGpuMirror mirror{};
    mirror.reserve(cpu.capacity);
    mirror.positions = cpu.positions;
    mirror.velocities = cpu.velocities;
    mirror.ages = cpu.ages;
    mirror.lifetimes = cpu.lifetimes;
    mirror.sizes = cpu.sizes;
    mirror.colors = cpu.colors;
    mirror.alphas = cpu.alphas;
    mirror.alive_flags = cpu.alive_flags;
    mirror.alive_count = cpu.count;
    return mirror;
}

bool ParticleGpuMirror::writeToCpuSoA(ParticleSoA& cpu) const {
    if (cpu.capacity != capacity) {
        return false;
    }

    cpu.positions = positions;
    cpu.velocities = velocities;
    cpu.ages = ages;
    cpu.lifetimes = lifetimes;
    cpu.sizes = sizes;
    cpu.colors = colors;
    cpu.alphas = alphas;
    cpu.alive_flags = alive_flags;
    cpu.count = alive_count;
    return true;
}

bool ParticleGpuMirror::tryWriteToCpuSoA(ParticleSoA& cpu) const {
    if (!canWriteToCpuSoA(cpu) || !aliveCountMatchesFlags()) {
        return false;
    }
    return writeToCpuSoA(cpu);
}

std::vector<u8> ParticleGpuMirror::tryPackToDeviceLayout() const {
    if (capacity == 0u || !aliveCountMatchesFlags()) {
    if (packGuard() != ParticleGpuPackGuard::Ok) {
        return {};
    }
    return packToDeviceLayout();

ParticleGpuSyncGuard ParticleGpuMirror::unpackGuardForLayout(const std::vector<u8>& bytes, u32 particle_capacity) {
    if (particle_capacity == 0u) {
        return ParticleGpuSyncGuard::CpuUninitialized;
    if (!ParticleGpuBufferLayout::validatePackedLayout(particle_capacity)) {
        return ParticleGpuSyncGuard::CapacityMismatch;
    if (!ParticleGpuBufferLayout::packedBytesFitCapacity(bytes, particle_capacity)) {
        return ParticleGpuSyncGuard::MirrorUninitialized;
    return ParticleGpuSyncGuard::Ok;
bool ParticleGpuMirror::shouldSkipPack() const {
    if (capacity == 0u) {
        return true;
    return !aliveCountMatchesFlags();

bool ParticleGpuMirror::tryPackToDeviceLayout(std::vector<u8>& out) const {
    if (shouldSkipPack()) {
        out.clear();
        return false;
    out = packToDeviceLayout();
    return !out.empty();

std::vector<u8> ParticleGpuMirror::packToDeviceLayout() const {
    const usize total_bytes = ParticleGpuBufferLayout::packedDeviceBytes(capacity);
    std::vector<u8> bytes(total_bytes, 0u);

    for (u32 index = 0; index < ParticleGpuBufferLayout::columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        const usize offset = ParticleGpuBufferLayout::columnDeviceOffset(column, capacity);
        const usize column_bytes = ParticleGpuBufferLayout::columnByteSize(column, capacity);
        const u8* source = nullptr;

        switch (column) {
        case ParticleGpuColumn::Positions:
            source = reinterpret_cast<const u8*>(positions.data());
            break;
        case ParticleGpuColumn::Velocities:
            source = reinterpret_cast<const u8*>(velocities.data());
            break;
        case ParticleGpuColumn::Ages:
            source = reinterpret_cast<const u8*>(ages.data());
            break;
        case ParticleGpuColumn::Lifetimes:
            source = reinterpret_cast<const u8*>(lifetimes.data());
            break;
        case ParticleGpuColumn::Sizes:
            source = reinterpret_cast<const u8*>(sizes.data());
            break;
        case ParticleGpuColumn::Colors:
            source = reinterpret_cast<const u8*>(colors.data());
            break;
        case ParticleGpuColumn::Alphas:
            source = reinterpret_cast<const u8*>(alphas.data());
            break;
        case ParticleGpuColumn::AliveFlags:
            source = reinterpret_cast<const u8*>(alive_flags.data());
            break;
        }

        if (source != nullptr && offset + column_bytes <= bytes.size()) {
            std::memcpy(bytes.data() + offset, source, column_bytes);
        }
    }

    return bytes;
}

std::vector<u8> ParticleGpuMirror::tryPackToDeviceLayout() const {
    const ParticleGpuMirrorPackPreflight preflight = preflightPack();
    if (!preflight.can_pack()) {
        return {};
    }
    return packToDeviceLayout();
}

ParticleGpuMirror ParticleGpuMirror::unpackFromDeviceLayout(const std::vector<u8>& bytes, u32 particle_capacity) {
    ParticleGpuMirror mirror{};
    mirror.reserve(particle_capacity);

    if (bytes.size() < ParticleGpuBufferLayout::packedDeviceBytes(particle_capacity)) {
        return mirror;
    }

    for (u32 index = 0; index < ParticleGpuBufferLayout::columnCount(); ++index) {
        const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
        const usize offset = ParticleGpuBufferLayout::columnDeviceOffset(column, particle_capacity);
        const usize column_bytes = ParticleGpuBufferLayout::columnByteSize(column, particle_capacity);
        u8* destination = nullptr;

        switch (column) {
        case ParticleGpuColumn::Positions:
            destination = reinterpret_cast<u8*>(mirror.positions.data());
            break;
        case ParticleGpuColumn::Velocities:
            destination = reinterpret_cast<u8*>(mirror.velocities.data());
            break;
        case ParticleGpuColumn::Ages:
            destination = reinterpret_cast<u8*>(mirror.ages.data());
            break;
        case ParticleGpuColumn::Lifetimes:
            destination = reinterpret_cast<u8*>(mirror.lifetimes.data());
            break;
        case ParticleGpuColumn::Sizes:
            destination = reinterpret_cast<u8*>(mirror.sizes.data());
            break;
        case ParticleGpuColumn::Colors:
            destination = reinterpret_cast<u8*>(mirror.colors.data());
            break;
        case ParticleGpuColumn::Alphas:
            destination = reinterpret_cast<u8*>(mirror.alphas.data());
            break;
        case ParticleGpuColumn::AliveFlags:
            destination = reinterpret_cast<u8*>(mirror.alive_flags.data());
            break;
        }

        if (destination != nullptr) {
            std::memcpy(destination, bytes.data() + offset, column_bytes);
        }
    }

    mirror.syncAliveCountFromFlags();
    return mirror;
}

void ParticleGpuMirror::syncAliveCountFromFlags() {
    alive_count = 0;
    for (u32 slot = 0; slot < capacity; ++slot) {
        if (alive_flags[slot] != 0u) {
            ++alive_count;
        }
    }
}

bool ParticleGpuMirror::matchesPackedLayout(const std::vector<u8>& bytes) const {
    const std::vector<u8> packed = packToDeviceLayout();
    return packed.size() == bytes.size() &&
           (packed.empty() || std::memcmp(packed.data(), bytes.data(), packed.size()) == 0);
}

bool ParticleGpuMirror::matchesCpuSoA(const ParticleSoA& cpu) const {
    if (cpu.capacity != capacity) {
        return false;
    }
    if (cpu.count != alive_count) {
        return false;
    }

    for (u32 slot = 0; slot < capacity; ++slot) {
        if (cpu.alive_flags[slot] != alive_flags[slot]) {
            return false;
        }
        if (alive_flags[slot] == 0u) {
            continue;
        }
        if (!vec3Equal(cpu.positions[slot], positions[slot]) ||
            !vec3Equal(cpu.velocities[slot], velocities[slot]) || cpu.ages[slot] != ages[slot] ||
            cpu.lifetimes[slot] != lifetimes[slot] || cpu.sizes[slot] != sizes[slot] ||
            !vec3Equal(cpu.colors[slot], colors[slot]) || cpu.alphas[slot] != alphas[slot]) {
            return false;
        }
    }
    return true;
}

bool ParticleGpuMirror::packedBytesFit(const std::vector<u8>& bytes) const {
    return bytes.size() >= ParticleGpuBufferLayout::packedDeviceBytes(capacity);
}

bool ParticleSoAGPU::allColumnPointersBound() const {
    if (capacity == 0u) {
        return false;
    }

    return positions != 0u && velocities != 0u && ages != 0u && lifetimes != 0u && sizes != 0u &&
           colors != 0u && alphas != 0u && alive_flags != 0u;
}

bool ParticleSoAGPU::validateAgainstLayout(u64 packed_base, u32 expected_capacity) const {
    if (capacity != expected_capacity) {
        return false;
    }
    if (capacity == 0u || packed_base == 0u) {
        return !hasDeviceBinding();
    }
    if (!allColumnPointersBound()) {
        return false;
    }

    const u64 expected_positions =
        ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Positions, packed_base, capacity);
    const u64 expected_alive_flags =
        ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::AliveFlags, packed_base, capacity);

    return positions == expected_positions && alive_flags == expected_alive_flags &&
           velocities > positions && alive_flags > alphas;
}

ParticleSoAGPU ParticleGpuMirror::toGpuPointers(u64 packed_device_address) const {
    ParticleSoAGPU gpu{};
    gpu.capacity = capacity;
    gpu.count = alive_count;

    if (packed_device_address == 0u || capacity == 0u) {
        return gpu;
    }

    gpu.positions = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Positions, packed_device_address, capacity);
    gpu.velocities = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Velocities, packed_device_address, capacity);
    gpu.ages = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Ages, packed_device_address, capacity);
    gpu.lifetimes = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Lifetimes, packed_device_address, capacity);
    gpu.sizes = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Sizes, packed_device_address, capacity);
    gpu.colors = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Colors, packed_device_address, capacity);
    gpu.alphas = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::Alphas, packed_device_address, capacity);
    gpu.alive_flags = ParticleGpuBufferLayout::columnDeviceAddress(ParticleGpuColumn::AliveFlags, packed_device_address, capacity);
    return gpu;
}

ParticleGpuFramePlanPreflight ParticleGpuFramePlan::preflightStub(u32 particle_capacity, u32 frame_emit_count,
                                                                   u32 frame_alive_count) {
    return forStub(particle_capacity, frame_emit_count, frame_alive_count).preflight();
}

ParticleGpuFramePlanPreflight ParticleGpuFramePlan::preflight() const {
    ParticleGpuFramePlanPreflight preflight{};
    preflight.capacity = capacity;
    preflight.emit_count = emit_count;
    preflight.alive_count = alive_count;
    preflight.buffers_ok = buffersSizedForCapacity();
    preflight.skip_sim = skipSimLaunch();
    preflight.skip_emit = skipEmitLaunch();
    preflight.sim_covers_capacity = dispatch.simCovers(capacity);
    preflight.emit_covers_count = dispatch.emitCovers(emit_count);
    preflight.sim_padding_threads = simPaddingThreadCount();
    preflight.emit_padding_threads = emitPaddingThreadCount();
    return preflight;
}

ParticleGpuFramePlan ParticleGpuFramePlan::forStub(u32 particle_capacity, u32 frame_emit_count, u32 frame_alive_count) {
    ParticleGpuFramePlan plan{};
    plan.capacity = particle_capacity;
    plan.emit_count = frame_emit_count;
    plan.alive_count = frame_alive_count;
    plan.buffers = ParticleGpuBuffers::forCapacity(particle_capacity);
    plan.dispatch = ParticleGpuDispatch::forFrame(particle_capacity, frame_emit_count);
    return plan;
}

bool ParticleGpuFramePlan::skipSimLaunch() const {
    return dispatch.shouldSkipSimLaunch(capacity);
}

bool ParticleGpuFramePlan::skipEmitLaunch() const {
    return dispatch.shouldSkipEmitLaunch(emit_count);
}

bool ParticleGpuFramePlan::buffersSizedForCapacity() const {
    return buffers.capacity == capacity &&
           buffers.deviceBytes == ParticleGpuBufferLayout::packedDeviceBytes(capacity);
}

u32 ParticleGpuFramePlan::remainingEmitSlots() const {
    if (capacity == 0u || alive_count >= capacity) {
        return 0u;
    }
    return capacity - alive_count;
}

u32 ParticleGpuFramePlan::clampedEmitCount() const {
    const u32 remaining = remainingEmitSlots();
    return emit_count > remaining ? remaining : emit_count;
}

bool ParticleGpuFramePlan::shouldSkipEmitOverflow() const {
    if (emit_count == 0u) {
        return false;
    return alive_count + emit_count > capacity;
    return emit_count <= remaining ? emit_count : remaining;

bool ParticleGpuFramePlan::shouldSkipGpuEmit() const {
    return emit_count == 0u || remainingEmitSlots() == 0u;

bool ParticleGpuFramePlan::shouldSkipGpuSimulate() const {
    return capacity == 0u || (alive_count == 0u && emit_count == 0u);

ParticleGpuEmitPreflight ParticleGpuFramePlan::preflightEmit() const {
    ParticleGpuEmitPreflight preflight{};
    preflight.requested = emit_count;
    preflight.remaining_free = remainingEmitSlots();
    preflight.allowed = clampedEmitCount();
    preflight.would_clamp = preflight.allowed < preflight.requested;
    preflight.skipped = shouldSkipGpuEmit();
    return preflight;

ParticleGpuSimPreflight ParticleGpuFramePlan::preflightSimulate() const {
    ParticleGpuSimPreflight preflight{};
    preflight.capacity = capacity;
    preflight.alive_count = alive_count;
    preflight.has_live_particles = alive_count > 0u;
    preflight.skipped = shouldSkipGpuSimulate();
}

u32 ParticleGpuFramePlan::simPaddingThreadCount() const {
    return dispatch.simPaddingThreads(capacity);
}

u32 ParticleGpuFramePlan::emitPaddingThreadCount() const {
    return dispatch.emitPaddingThreads(emit_count);
}

ParticleGpuDispatchPreflight ParticleGpuFramePlan::dispatchPreflight() const {
    return dispatch.preflightFrame(capacity, emit_count);
}

ParticleGpuFramePreflight ParticleGpuFramePlan::preflight() const {
    ParticleGpuFramePreflight result{};
    result.buffers_ok = buffersSizedForCapacity();
    result.sim_dispatch_ok = dispatch.simCovers(capacity);
    result.emit_dispatch_ok = dispatch.emitCovers(emit_count);
    result.sim_padding_ok = dispatch.simPaddingAccountsFor(capacity);
    result.emit_padding_ok = dispatch.emitPaddingAccountsFor(emit_count);
    result.empty_buffer = buffers.isEmpty();
    result.sim_alive_ok = alive_count <= capacity;
    result.emit_slots_ok = emit_count <= remainingEmitSlots() || emit_count == 0u;
    result.skip_sim_zero_alive = shouldSkipSimWithZeroAlive();
    result.skip_emit_at_capacity = shouldSkipEmitAtCapacity();
    result.dispatch_preflight_ok = dispatchPreflight().ready_for_stub();
    result.skip_sim = skipSimLaunch() || shouldSkipGpuSimulate();
    result.skip_emit = skipEmitLaunch() || shouldSkipGpuEmit();
    result.emit_within_capacity = clampedEmitCount() == emit_count;
    result.sim_has_work = capacity > 0u && !shouldSkipGpuSimulate();
    result.buffers_empty = buffers.isEmpty();
    return result;
}

bool ParticleGpuFramePreflight::ready_for_stub() const {
    return buffers_ok && sim_dispatch_ok && emit_dispatch_ok && sim_padding_ok && emit_padding_ok &&
           dispatch_preflight_ok;
}

bool ParticleGpuFramePreflight::can_simulate() const {
    return sim_dispatch_ok && sim_padding_ok && sim_alive_ok && !skip_sim_zero_alive;
}

bool ParticleGpuFramePreflight::can_emit() const {
    return emit_dispatch_ok && emit_padding_ok && emit_slots_ok && !skip_emit_at_capacity;
}

bool ParticleGpuFramePreflight::can_emit() const {
    return !skip_emit && emit_within_capacity && emit_dispatch_ok;
}

bool ParticleGpuFramePreflight::can_simulate() const {
    return !skip_sim && sim_has_work && sim_dispatch_ok;
}

u32 ParticleGpuFramePlan::freeSlotCount() const {
    return gpu_free_slot_count(capacity, alive_count);
}

u32 ParticleGpuFramePlan::clampedEmitCount() const {
    return clamp_gpu_emit_count(emit_count, capacity, alive_count);
}

ParticleGpuEmitGuard ParticleGpuFramePlan::emitGuard() const {
    return emit_guard_for_frame(emit_count, capacity, alive_count);
}

bool ParticleGpuFramePlan::shouldSkipSimWhenEmpty() const {
    return should_skip_sim_when_empty(alive_count);
}

bool ParticleGpuFramePlan::canSimulate() const {
    return capacity > 0u && !dispatch.shouldSkipSimLaunch(capacity);
}

bool ParticleGpuFramePlan::canEmit() const {
    const ParticleGpuEmitGuard guard = emitGuard();
    return guard.can_emit() && !dispatch.shouldSkipEmitLaunch(clampedEmitCount());
}

bool ParticleGpuFramePlan::buffersBound() const {
    return buffers.isBound();
}

bool ParticleGpuFramePlan::requiresBufferBind() const {
    return capacity > 0u && !buffers.isBound();
}

ParticleGpuFrameLaunchPreflight ParticleGpuFramePlan::launchPreflight() const {
    ParticleGpuFrameLaunchPreflight result{};
    result.frame = preflight();
    result.emit = emitGuard();
    result.buffers = buffers.preflight();
    result.skip_sim_when_empty = shouldSkipSimWhenEmpty();
    result.sim_can_launch = canSimulate() && !result.skip_sim_when_empty;
    result.emit_can_launch = canEmit();
    return result;
}

bool ParticleGpuFrameLaunchPreflight::ready_for_stub() const {
    if (!frame.ready_for_stub()) {
        return false;
    }
    if (buffers.is_empty_capacity()) {
        return true;
    }
    return buffers.bytes_ok;
}

ParticleSoAGPU ParticleGpuFramePlan::gpuPointers(u64 packed_device_address) const {
    if (packed_device_address == 0u || capacity == 0u) {
        ParticleSoAGPU gpu{};
        gpu.capacity = capacity;
        gpu.count = alive_count;
        return gpu;
    }

    ParticleGpuMirror mirror{};
    mirror.reserve(capacity);
    mirror.alive_count = alive_count;
    return mirror.toGpuPointers(packed_device_address);
}

FramePlanPreflight ParticleGpuFramePlan::preflight() const {
    FramePlanPreflight result{};
    result.dispatch = dispatch.preflight(capacity, emit_count);
    result.buffers_sized = buffersSizedForCapacity();
    result.is_idle = isIdle();
    return result;
}

namespace particle_gpu_util {

u32 gridDimX(u32 element_count, u32 block_size) {
    if (element_count == 0u || block_size == 0u) {
        return 0u;
    }
    return (element_count + block_size - 1u) / block_size;
}

u32 coveredThreadCount(u32 block_count, u32 block_size) {
    return block_count * block_size;
}

u32 paddingThreads(u32 element_count, u32 block_count, u32 block_size) {
    if (element_count == 0u || block_count == 0u || block_size == 0u) {
        return 0u;
    }
    const u32 covered = coveredThreadCount(block_count, block_size);
    return covered > element_count ? covered - element_count : 0u;
}

bool threadCoversElement(u32 thread_index, u32 element_count) {
    if (element_count == 0u) {
        return false;
    }
    return thread_index < element_count;
}

bool isPaddingThread(u32 thread_index, u32 element_count) {
    if (element_count == 0u) {
        return false;
    }
    return thread_index >= element_count;
}

u32 blockIndexOf(u32 global_thread_index, u32 block_size) {
    if (block_size == 0u) {
        return 0u;
    }
    return global_thread_index / block_size;

u32 localThreadIndex(u32 global_thread_index, u32 block_size) {
    return global_thread_index % block_size;

u32 globalThreadIndex(u32 block_index, u32 local_thread_index, u32 block_size) {
    return block_index * block_size + local_thread_index;
DispatchPreflight preflight_dispatch(u32 capacity, u32 emit_count) {
    return ParticleGpuDispatch::forFrame(capacity, emit_count).preflight(capacity, emit_count);

FramePlanPreflight preflight_frame_plan(u32 capacity, u32 emit_count, u32 alive_count) {
    return ParticleGpuFramePlan::forStub(capacity, emit_count, alive_count).preflight();

ParticleGpuSlotOffsetPreflight preflight_slot_offset(ParticleGpuColumn column, u32 capacity, u32 slot) {
    ParticleGpuSlotOffsetPreflight result{};
    result.slot = slot;
    result.capacity = capacity;
    result.guard = ParticleGpuBufferLayout::slotOffsetGuard(slot, capacity);
    result.can_access = ParticleGpuBufferLayout::canAccessSlotAtOffset(column, capacity, slot);
    if (result.can_access) {
        result.column_byte_offset = ParticleGpuBufferLayout::columnSlotByteOffset(column, capacity, slot);
    return result;
}

u32 elementIndexForThread(u32 global_thread_index, u32 element_count) {
    return threadCoversElement(global_thread_index, element_count) ? global_thread_index : element_count;
}

bool threadHasValidElement(u32 global_thread_index, u32 element_count) {
    return threadCoversElement(global_thread_index, element_count);
}

u32 slotIndexFromGlobalThread(u32 global_thread_index, u32 element_count) {
    if (!threadCoversElement(global_thread_index, element_count)) {
        return element_count;
    }
    return global_thread_index;
}

bool globalThreadCoversSlot(u32 global_thread_index, u32 element_count) {
    return threadCoversElement(global_thread_index, element_count);
}

} // namespace particle_gpu_util

u32 gpu_free_slot_count(u32 capacity, u32 alive_count) {
    return capacity >= alive_count ? capacity - alive_count : 0u;
}

u32 clamp_gpu_emit_count(u32 emit_count, u32 capacity, u32 alive_count) {
    const u32 free = gpu_free_slot_count(capacity, alive_count);
    if (free == 0u || emit_count == 0u) {
        return 0u;
    }
    return emit_count < free ? emit_count : free;
}

ParticleGpuEmitGuard emit_guard_for_frame(u32 requested, u32 capacity, u32 alive_count) {
    ParticleGpuEmitGuard guard{};
    guard.requested = requested;
    guard.free_slots = gpu_free_slot_count(capacity, alive_count);
    guard.at_capacity = guard.free_slots == 0u;
    guard.clamped = clamp_gpu_emit_count(requested, capacity, alive_count);
    guard.skip_emit = requested == 0u || guard.at_capacity || capacity == 0u;
    return guard;
}

bool should_skip_sim_dispatch(u32 capacity) {
    return capacity == 0u;
}

bool should_skip_emit_dispatch(u32 emit_count) {
    return emit_count == 0u;
}

bool should_skip_empty_buffer(const ParticleGpuBuffers& buffers) {
    return buffers.isEmpty() || !buffers.bytesMatchLayout();
}

bool should_skip_gpu_emit(u32 emit_count, u32 capacity, u32 alive_count) {
    if (emit_count == 0u) {
        return true;
    if (capacity == 0u || alive_count >= capacity) {
    return false;

bool should_skip_gpu_simulate(u32 capacity, u32 alive_count, u32 emit_count) {
    if (capacity == 0u) {
    return alive_count == 0u && emit_count == 0u;
bool should_skip_sim_when_empty(u32 alive_count) {
    return alive_count == 0u;

bool should_skip_pack(u32 capacity) {
    return capacity == 0u;

bool is_empty_device_buffer(const ParticleGpuBuffers& buffers) {
    return buffers.isEmptyCapacity() || buffers.deviceBytes == 0u;
}

bool should_skip_mirror_sync(const ParticleGpuMirror& mirror, const ParticleSoA& cpu) {
    return mirror.shouldSkipSyncFromCpu(cpu);
}

bool should_skip_mirror_write(const ParticleGpuMirror& mirror, const ParticleSoA& cpu) {
    const ParticleGpuMirrorPreflight preflight = mirror.preflightFromCpu(cpu);
    return preflight.can_write_to_cpu() && mirror.matchesCpuSoA(cpu);
}

bool should_skip_frame_sim(const ParticleGpuFramePlan& plan) {
    return plan.skipSimLaunch() || plan.shouldSkipSimWithZeroAlive();
}

bool should_skip_frame_emit(const ParticleGpuFramePlan& plan) {
    return plan.skipEmitLaunch() || plan.shouldSkipEmitAtCapacity() || plan.emit_count == 0u;

bool should_skip_empty_buffer(const ParticleGpuBuffers& buffers) {
    return buffers.isEmpty();
bool should_skip_device_upload(const ParticleGpuMirror& mirror, const ParticleSoA& cpu) {
    const ParticleGpuMirrorUploadPreflight preflight = mirror.preflightDeviceUpload(cpu);
    return preflight.can_upload() && mirror.matchesCpuSoA(cpu);
}

} // namespace fuse::vfx
