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

u32 ParticleGpuDispatch::simPaddingThreads(u32 capacity) const {
    return particle_gpu_util::paddingThreads(capacity, simBlockCount, simThreadCount);
}

u32 ParticleGpuDispatch::emitPaddingThreads(u32 emit_count) const {
    return particle_gpu_util::paddingThreads(emit_count, emitBlockCount, emitThreadCount);
}

ParticleGpuBuffers ParticleGpuBuffers::forCapacity(u32 particle_capacity) {
    ParticleGpuBuffers buffers{};
    buffers.capacity = particle_capacity;
    buffers.deviceBytes = ParticleGpuBufferLayout::packedDeviceBytes(particle_capacity);
    return buffers;
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

} // namespace particle_gpu_util

} // namespace fuse::vfx
