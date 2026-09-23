#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace fuse::scene {

namespace {

// Brick payload forms (SVOBrick::mode).
constexpr u8 kModeSparse = 0;
constexpr u8 kModeDense = 1;
constexpr u8 kModeUniform = 2;
constexpr u8 kModeMasked = 3;

// Bricks are 8^3 voxels: at depth 10 that stops the octree three levels early, so scattered
// data needs ~1/8 of the interior nodes of per-voxel leaves, while sparse payloads keep a
// lone voxel at 8 bytes. 4^3 bricks would need ~3.5x the interior nodes for scattered data
// and 2^3 bricks gain almost nothing over per-voxel leaves.
constexpr u32 kMaxBrickLog = 3;

} // namespace

void SVO::init(const SVODesc& desc) {
    destroy();
    m_desc = desc;
    m_brickLog = std::min(kMaxBrickLog, m_desc.maxDepth);
    m_brickDepth = m_desc.maxDepth - m_brickLog;
    m_brickVoxels = 1u << (3u * m_brickLog);
    m_maskWords = (m_brickVoxels + 31u) / 32u;
    if (m_brickDepth > 0u) {
        m_nodes.push_back(SVONode{});
    } else {
        SVOBrick brick{};
        brick.payload = allocWords(0u);
        m_bricks.push_back(brick);
        if (m_desc.maxDepth == 0u) {
            writeVoxel(0u, 0u, 0u, defaultSdf(0u)); // a depth-0 root is itself the (empty) leaf
        }
    }
    m_initialized = true;
}

void SVO::destroy() {
    m_desc = {};
    m_nodes = {};
    m_bricks = {};
    m_pool = {};
    for (std::vector<u32>& list : m_freeLists) {
        list = {};
    }
    m_voxelCount = 0;
    m_initialized = false;
}

usize SVO::memoryBytes() const {
    usize bytes = m_nodes.capacity() * sizeof(SVONode) + m_bricks.capacity() * sizeof(SVOBrick) +
                  m_pool.capacity() * sizeof(u32);
    for (const std::vector<u32>& list : m_freeLists) {
        bytes += list.capacity() * sizeof(u32);
    }
    return bytes;
}

bool SVO::inBounds(ivec3 coord) const {
    const s32 resolution = static_cast<s32>(1u << m_desc.maxDepth);
    return coord.x >= 0 && coord.y >= 0 && coord.z >= 0 && coord.x < resolution && coord.y < resolution &&
           coord.z < resolution;
}

f32 SVO::leafSize() const {
    return m_desc.rootSize / static_cast<f32>(1u << m_desc.maxDepth);
}

vec3 SVO::voxelCenterWorld(ivec3 coord) const {
    const f32 size = leafSize();
    return m_desc.origin + toVec3(coord) * size + vec3(size * 0.5f, size * 0.5f, size * 0.5f);
}

f32 SVO::defaultSdf(u32 material) const {
    const f32 half = leafSize() * 0.5f;
    return material != 0u ? -half : half;
}

u32 SVO::localIndex(ivec3 coord) const {
    const u32 mask = (1u << m_brickLog) - 1u;
    return (static_cast<u32>(coord.x) & mask) | ((static_cast<u32>(coord.y) & mask) << m_brickLog) |
           ((static_cast<u32>(coord.z) & mask) << (2u * m_brickLog));
}

// ---------------------------------------------------------------------------------------------
// Payload pool: one word arena with a free list per size class.

u32 SVO::classWords(u32 sizeClass) const {
    if (sizeClass < kSparseClasses) {
        return 2u << sizeClass; // (localIndex, material) pairs
    }
    if (sizeClass == kDenseClass) {
        return m_brickVoxels + m_maskWords + 1u; // materials, written mask, sdf plane offset
    }
    if (sizeClass == kMaskedClass) {
        return 1u + m_maskWords; // shared material, written mask
    }
    return m_brickVoxels; // sdf plane
}

u32 SVO::allocWords(u32 sizeClass) {
    std::vector<u32>& list = m_freeLists[sizeClass];
    if (!list.empty()) {
        const u32 offset = list.back();
        list.pop_back();
        return offset;
    }
    const u32 offset = static_cast<u32>(m_pool.size());
    m_pool.resize(m_pool.size() + classWords(sizeClass), 0u);
    return offset;
}

void SVO::freeWords(u32 offset, u32 sizeClass) {
    m_freeLists[sizeClass].push_back(offset);
}

// ---------------------------------------------------------------------------------------------
// Octree walks.

u32 SVO::findBrick(ivec3 coord) const {
    if (!m_initialized || !inBounds(coord)) {
        return kNoNode;
    }
    if (m_brickDepth == 0u) {
        return 0u;
    }
    u32 nodeIndex = 0;
    for (u32 depth = 0; depth < m_brickDepth; ++depth) {
        const u32 mask = 1u << (m_desc.maxDepth - depth - 1u);
        const u32 octant = ((static_cast<u32>(coord.x) & mask) ? 1u : 0u) |
                           ((static_cast<u32>(coord.y) & mask) ? 2u : 0u) |
                           ((static_cast<u32>(coord.z) & mask) ? 4u : 0u);
        nodeIndex = m_nodes[nodeIndex].children[octant];
        if (nodeIndex == kNoNode) {
            return kNoNode;
        }
    }
    return nodeIndex; // the last hop lands on a brick index
}

u32 SVO::locate(ivec3 coord, u32& emptyCellSize) const {
    emptyCellSize = 1u;
    if (m_brickDepth == 0u) {
        return 0u;
    }
    u32 nodeIndex = 0;
    for (u32 depth = 0; depth < m_brickDepth; ++depth) {
        const u32 shift = m_desc.maxDepth - depth - 1u;
        const u32 mask = 1u << shift;
        const u32 octant = ((static_cast<u32>(coord.x) & mask) ? 1u : 0u) |
                           ((static_cast<u32>(coord.y) & mask) ? 2u : 0u) |
                           ((static_cast<u32>(coord.z) & mask) ? 4u : 0u);
        nodeIndex = m_nodes[nodeIndex].children[octant];
        if (nodeIndex == kNoNode) {
            emptyCellSize = mask; // the missing child's cube, in voxels
            return kNoNode;
        }
    }
    return nodeIndex;
}

u32 SVO::ensureBrick(ivec3 coord) {
    if (m_brickDepth == 0u) {
        return 0u;
    }
    u32 nodeIndex = 0;
    for (u32 depth = 0; depth < m_brickDepth; ++depth) {
        const u32 mask = 1u << (m_desc.maxDepth - depth - 1u);
        const u32 octant = ((static_cast<u32>(coord.x) & mask) ? 1u : 0u) |
                           ((static_cast<u32>(coord.y) & mask) ? 2u : 0u) |
                           ((static_cast<u32>(coord.z) & mask) ? 4u : 0u);
        u32 child = m_nodes[nodeIndex].children[octant];
        if (child == kNoNode) {
            if (depth + 1u == m_brickDepth) {
                SVOBrick brick{};
                brick.payload = allocWords(0u);
                child = static_cast<u32>(m_bricks.size());
                m_bricks.push_back(brick);
            } else {
                child = static_cast<u32>(m_nodes.size());
                m_nodes.push_back(SVONode{});
            }
            m_nodes[nodeIndex].children[octant] = child;
        }
        nodeIndex = child;
    }
    return nodeIndex;
}

// ---------------------------------------------------------------------------------------------
// Brick payload access.

namespace {

/// Binary search over sorted (localIndex, material) pairs; returns the insertion slot.
u32 sparseLowerBound(const u32* entries, u32 count, u32 local) {
    u32 lo = 0;
    u32 hi = count;
    while (lo < hi) {
        const u32 mid = (lo + hi) >> 1u;
        if (entries[mid * 2u] < local) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

} // namespace

SVO::VoxelState SVO::readVoxel(u32 brick, u32 local) const {
    const SVOBrick& b = m_bricks[brick];
    VoxelState state{};
    if (b.mode == kModeUniform) {
        state.written = true;
        state.material = b.payload;
    } else if (b.mode == kModeSparse) {
        const u32* entries = m_pool.data() + b.payload;
        const u32 slot = sparseLowerBound(entries, b.count, local);
        if (slot < b.count && entries[slot * 2u] == local) {
            state.written = true;
            state.material = entries[slot * 2u + 1u];
        }
    } else if (b.mode == kModeMasked) {
        const u32* words = m_pool.data() + b.payload;
        state.written = (words[1u + (local >> 5u)] >> (local & 31u)) & 1u;
        state.material = state.written ? words[0] : 0u;
    } else {
        const u32* words = m_pool.data() + b.payload;
        state.written = (words[m_brickVoxels + (local >> 5u)] >> (local & 31u)) & 1u;
        state.material = words[local];
        const u32 plane = words[m_brickVoxels + m_maskWords];
        if (plane != kNoNode) {
            f32 value = 0.f;
            std::memcpy(&value, m_pool.data() + plane + local, sizeof(f32));
            state.sdf = value;
            return state;
        }
    }
    state.sdf = defaultSdf(state.material);
    return state;
}

void SVO::toDense(u32 brick) {
    const u32 dense = allocWords(kDenseClass); // may grow the pool: index, never hold pointers
    SVOBrick& b = m_bricks[brick];
    u32* words = m_pool.data() + dense;
    std::fill(words, words + m_brickVoxels + m_maskWords, 0u);
    words[m_brickVoxels + m_maskWords] = kNoNode;
    if (b.mode == kModeUniform) {
        std::fill(words, words + m_brickVoxels, b.payload);
        for (u32 i = 0; i < m_brickVoxels; ++i) {
            words[m_brickVoxels + (i >> 5u)] |= 1u << (i & 31u);
        }
    } else if (b.mode == kModeSparse) {
        const u32* entries = m_pool.data() + b.payload;
        for (u32 i = 0; i < b.count; ++i) {
            const u32 local = entries[i * 2u];
            words[local] = entries[i * 2u + 1u];
            words[m_brickVoxels + (local >> 5u)] |= 1u << (local & 31u);
        }
        freeWords(b.payload, b.sizeClass);
    } else if (b.mode == kModeMasked) {
        const u32 material = m_pool[b.payload];
        for (u32 i = 0; i < m_maskWords; ++i) {
            const u32 bits = m_pool[b.payload + 1u + i];
            words[m_brickVoxels + i] = bits;
            for (u32 bit = 0; bit < 32u; ++bit) {
                if ((bits >> bit) & 1u) {
                    words[i * 32u + bit] = material;
                }
            }
        }
        freeWords(b.payload, kMaskedClass);
    }
    b.mode = kModeDense;
    b.payload = dense;
    b.count = 0;
    b.sizeClass = static_cast<u8>(kDenseClass);
}

void SVO::ensureSdfPlane(u32 brick) {
    if (m_pool[denseSdfSlot(brick)] != kNoNode) {
        return;
    }
    const u32 plane = allocWords(kSdfClass);
    const u32 dense = m_bricks[brick].payload;
    for (u32 i = 0; i < m_brickVoxels; ++i) {
        const f32 value = defaultSdf(m_pool[dense + i]); // unwritten voxels hold material 0 -> +half
        std::memcpy(&m_pool[plane + i], &value, sizeof(f32));
    }
    m_pool[denseSdfSlot(brick)] = plane;
}

void SVO::writeVoxel(u32 brick, u32 local, u32 material, f32 sdf) {
    const bool needsSdf = m_desc.storeSdf && sdf != defaultSdf(material);
    SVOBrick& b = m_bricks[brick];
    u32 previous = 0u;

    if (b.mode == kModeUniform) {
        if (!needsSdf && material == b.payload) {
            return;
        }
        toDense(brick);
    }

    if (b.mode == kModeSparse && !needsSdf) {
        const u32 slot = sparseLowerBound(m_pool.data() + b.payload, b.count, local);
        if (slot < b.count && m_pool[b.payload + slot * 2u] == local) {
            previous = m_pool[b.payload + slot * 2u + 1u];
            m_pool[b.payload + slot * 2u + 1u] = material;
            m_voxelCount = m_voxelCount + (material != 0u ? 1u : 0u) - (previous != 0u ? 1u : 0u);
            return;
        }
        if (b.count < kSparseMax) {
            if (b.count == (1u << b.sizeClass)) {
                const u32 grown = allocWords(b.sizeClass + 1u);
                std::copy_n(m_pool.data() + b.payload, b.count * 2u, m_pool.data() + grown);
                freeWords(b.payload, b.sizeClass);
                b.payload = grown;
                ++b.sizeClass;
            }
            u32* entries = m_pool.data() + b.payload;
            std::copy_backward(entries + slot * 2u, entries + b.count * 2u, entries + b.count * 2u + 2u);
            entries[slot * 2u] = local;
            entries[slot * 2u + 1u] = material;
            ++b.count;
            m_voxelCount += material != 0u ? 1u : 0u;
            return;
        }
    }

    if (b.mode == kModeSparse && !needsSdf && material != 0u) {
        // Full sparse brick: keep it compact when every voxel shares the new voxel's material.
        bool shared = true;
        for (u32 i = 0; i < b.count && shared; ++i) {
            shared = m_pool[b.payload + i * 2u + 1u] == material;
        }
        if (shared) {
            const u32 masked = allocWords(kMaskedClass);
            u32* words = m_pool.data() + masked;
            std::fill(words, words + 1u + m_maskWords, 0u);
            words[0] = material;
            for (u32 i = 0; i < b.count; ++i) {
                const u32 entry = m_pool[b.payload + i * 2u];
                words[1u + (entry >> 5u)] |= 1u << (entry & 31u);
            }
            freeWords(b.payload, b.sizeClass);
            b.mode = kModeMasked;
            b.payload = masked;
            b.count = 0;
            b.sizeClass = static_cast<u8>(kMaskedClass);
        }
    }

    if (b.mode == kModeMasked) {
        if (!needsSdf && material == m_pool[b.payload]) {
            u32& bits = m_pool[b.payload + 1u + (local >> 5u)];
            const u32 bit = 1u << (local & 31u);
            m_voxelCount += (bits & bit) != 0u ? 0u : 1u;
            bits |= bit;
            return;
        }
        toDense(brick);
    }

    if (b.mode == kModeSparse) {
        toDense(brick); // full, or a voxel needs a stored distance
    }

    const u32 dense = b.payload;
    previous = m_pool[dense + local];
    m_pool[dense + local] = material;
    m_pool[dense + m_brickVoxels + (local >> 5u)] |= 1u << (local & 31u);
    if (needsSdf) {
        ensureSdfPlane(brick);
    }
    const u32 plane = m_pool[denseSdfSlot(brick)];
    if (plane != kNoNode) {
        std::memcpy(&m_pool[plane + local], &sdf, sizeof(f32));
    }
    m_voxelCount = m_voxelCount + (material != 0u ? 1u : 0u) - (previous != 0u ? 1u : 0u);
}

void SVO::makeUniform(u32 brick, u32 material) {
    SVOBrick& b = m_bricks[brick];
    usize solid = 0;
    if (b.mode == kModeUniform) {
        solid = b.payload != 0u ? m_brickVoxels : 0u;
    } else if (b.mode == kModeSparse) {
        for (u32 i = 0; i < b.count; ++i) {
            solid += m_pool[b.payload + i * 2u + 1u] != 0u ? 1u : 0u;
        }
        freeWords(b.payload, b.sizeClass);
    } else if (b.mode == kModeMasked) {
        for (u32 i = 0; i < m_maskWords; ++i) {
            solid += static_cast<usize>(std::popcount(m_pool[b.payload + 1u + i]));
        }
        freeWords(b.payload, kMaskedClass);
    } else {
        for (u32 i = 0; i < m_brickVoxels; ++i) {
            solid += m_pool[b.payload + i] != 0u ? 1u : 0u;
        }
        const u32 plane = m_pool[denseSdfSlot(brick)];
        if (plane != kNoNode) {
            freeWords(plane, kSdfClass);
        }
        freeWords(b.payload, kDenseClass);
    }
    b.mode = kModeUniform;
    b.payload = material;
    b.count = 0;
    b.sizeClass = 0;
    m_voxelCount = m_voxelCount - solid + (material != 0u ? m_brickVoxels : 0u);
}

bool SVO::solidMask(u32 brick, u64* mask) const {
    const SVOBrick& b = m_bricks[brick];
    const u32 words64 = (m_brickVoxels + 63u) / 64u;
    std::fill(mask, mask + words64, 0ull);
    bool any = false;
    if (b.mode == kModeUniform) {
        if (b.payload == 0u) {
            return false;
        }
        for (u32 i = 0; i < m_brickVoxels; ++i) {
            mask[i >> 6u] |= 1ull << (i & 63u);
        }
        return true;
    }
    if (b.mode == kModeSparse) {
        for (u32 i = 0; i < b.count; ++i) {
            if (m_pool[b.payload + i * 2u + 1u] != 0u) {
                const u32 local = m_pool[b.payload + i * 2u];
                mask[local >> 6u] |= 1ull << (local & 63u);
                any = true;
            }
        }
        return any;
    }
    if (b.mode == kModeMasked) {
        for (u32 i = 0; i < m_maskWords; ++i) {
            const u64 bits = m_pool[b.payload + 1u + i];
            mask[i >> 1u] |= bits << ((i & 1u) * 32u);
            any = any || bits != 0u;
        }
        return any;
    }
    for (u32 i = 0; i < m_brickVoxels; ++i) {
        if (m_pool[b.payload + i] != 0u) {
            mask[i >> 6u] |= 1ull << (i & 63u);
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------------------------
// Public API.

f32 SVO::sdfAtVoxel(ivec3 coord) const {
    const u32 brick = findBrick(coord);
    if (brick == kNoNode) {
        return leafSize() * 0.5f; // never written: empty
    }
    return readVoxel(brick, localIndex(coord)).sdf;
}

void SVO::set(ivec3 voxelCoord, u32 material) {
    if (!m_initialized || !inBounds(voxelCoord)) {
        return;
    }
    if (material == 0u) {
        const u32 brick = findBrick(voxelCoord);
        if (brick == kNoNode) {
            return;
        }
        const u32 local = localIndex(voxelCoord);
        if (readVoxel(brick, local).written) {
            writeVoxel(brick, local, 0u, defaultSdf(0u));
        }
        return;
    }
    writeVoxel(ensureBrick(voxelCoord), localIndex(voxelCoord), material, defaultSdf(material));
}

u32 SVO::get(ivec3 voxelCoord) const {
    const u32 brick = findBrick(voxelCoord);
    return brick == kNoNode ? 0u : readVoxel(brick, localIndex(voxelCoord)).material;
}

void SVO::fill(ivec3 minCoord, ivec3 maxCoord, u32 material) {
    if (!m_initialized) {
        return;
    }
    const s32 last = static_cast<s32>(1u << m_desc.maxDepth) - 1;
    const s32 lo[3] = {std::max(std::min(minCoord.x, maxCoord.x), 0), std::max(std::min(minCoord.y, maxCoord.y), 0),
                       std::max(std::min(minCoord.z, maxCoord.z), 0)};
    const s32 hi[3] = {std::min(std::max(minCoord.x, maxCoord.x), last),
                       std::min(std::max(minCoord.y, maxCoord.y), last),
                       std::min(std::max(minCoord.z, maxCoord.z), last)};
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) {
        return;
    }

    // Brick by brick: fully covered bricks collapse to one uniform record.
    const s32 edge = static_cast<s32>(1u << m_brickLog);
    const s32 log = static_cast<s32>(m_brickLog);
    for (s32 bz = lo[2] >> log; bz <= hi[2] >> log; ++bz) {
        for (s32 by = lo[1] >> log; by <= hi[1] >> log; ++by) {
            for (s32 bx = lo[0] >> log; bx <= hi[0] >> log; ++bx) {
                const ivec3 base{bx * edge, by * edge, bz * edge};
                const s32 x0 = std::max(lo[0], base.x);
                const s32 y0 = std::max(lo[1], base.y);
                const s32 z0 = std::max(lo[2], base.z);
                const s32 x1 = std::min(hi[0], base.x + edge - 1);
                const s32 y1 = std::min(hi[1], base.y + edge - 1);
                const s32 z1 = std::min(hi[2], base.z + edge - 1);
                const bool covered = x1 - x0 + 1 == edge && y1 - y0 + 1 == edge && z1 - z0 + 1 == edge;

                u32 brick = kNoNode;
                if (material != 0u) {
                    brick = ensureBrick(base);
                    if (covered) {
                        makeUniform(brick, material);
                        continue;
                    }
                } else {
                    brick = findBrick(base);
                    if (brick == kNoNode) {
                        continue; // clearing never-written space is a no-op
                    }
                }
                for (s32 z = z0; z <= z1; ++z) {
                    for (s32 y = y0; y <= y1; ++y) {
                        for (s32 x = x0; x <= x1; ++x) {
                            const u32 local = localIndex({x, y, z});
                            if (material != 0u || readVoxel(brick, local).written) {
                                writeVoxel(brick, local, material, defaultSdf(material));
                            }
                        }
                    }
                }
            }
        }
    }
}

void SVO::carve(vec3 center, f32 radius) {
    if (!m_initialized || radius <= 0.f) {
        return;
    }

    const f32 size = leafSize();
    const s32 extent = static_cast<s32>(std::ceil(radius / size)) + 1;
    const vec3 local = center - m_desc.origin;
    const ivec3 centerVoxel{
        static_cast<s32>(std::floor(local.x / size)),
        static_cast<s32>(std::floor(local.y / size)),
        static_cast<s32>(std::floor(local.z / size)),
    };
    const s32 last = static_cast<s32>(1u << m_desc.maxDepth) - 1;
    const s32 lo[3] = {std::max(centerVoxel.x - extent, 0), std::max(centerVoxel.y - extent, 0),
                       std::max(centerVoxel.z - extent, 0)};
    const s32 hi[3] = {std::min(centerVoxel.x + extent, last), std::min(centerVoxel.y + extent, last),
                       std::min(centerVoxel.z + extent, last)};
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) {
        return;
    }

    const s32 edge = static_cast<s32>(1u << m_brickLog);
    const s32 log = static_cast<s32>(m_brickLog);
    for (s32 bz = lo[2] >> log; bz <= hi[2] >> log; ++bz) {
        for (s32 by = lo[1] >> log; by <= hi[1] >> log; ++by) {
            for (s32 bx = lo[0] >> log; bx <= hi[0] >> log; ++bx) {
                const ivec3 base{bx * edge, by * edge, bz * edge};
                const u32 brick = findBrick(base);
                if (brick == kNoNode) {
                    continue; // empty space stays empty
                }
                for (s32 z = std::max(lo[2], base.z); z <= std::min(hi[2], base.z + edge - 1); ++z) {
                    for (s32 y = std::max(lo[1], base.y); y <= std::min(hi[1], base.y + edge - 1); ++y) {
                        for (s32 x = std::max(lo[0], base.x); x <= std::min(hi[0], base.x + edge - 1); ++x) {
                            const ivec3 coord{x, y, z};
                            const u32 index = localIndex(coord);
                            const VoxelState state = readVoxel(brick, index);
                            if (!state.written) {
                                continue; // never-written voxels stay untouched
                            }
                            const f32 distance = (voxelCenterWorld(coord) - center).length();
                            if (distance > radius + size) {
                                continue;
                            }

                            // CSG subtraction: sdf = max(sdf, -sphere) where sphere = distance - radius.
                            const f32 carved = std::max(state.sdf, radius - distance);
                            const u32 material = carved > 0.f ? 0u : state.material; // surface -> empty
                            if (material == state.material && (!m_desc.storeSdf || carved == state.sdf)) {
                                continue;
                            }
                            writeVoxel(brick, index, material, m_desc.storeSdf ? carved : defaultSdf(material));
                        }
                    }
                }
            }
        }
    }
}

f32 SVO::sdfQuery(vec3 worldPos) const {
    if (!m_initialized) {
        return std::numeric_limits<f32>::max();
    }

    // Samples sit at voxel centres; interpolate the 8 around worldPos (continuous everywhere).
    const f32 size = leafSize();
    const vec3 grid = (worldPos - m_desc.origin) * (1.f / size) - vec3(0.5f, 0.5f, 0.5f);
    const f32 fx = std::floor(grid.x);
    const f32 fy = std::floor(grid.y);
    const f32 fz = std::floor(grid.z);
    const ivec3 base{static_cast<s32>(fx), static_cast<s32>(fy), static_cast<s32>(fz)};
    const f32 tx = grid.x - fx;
    const f32 ty = grid.y - fy;
    const f32 tz = grid.z - fz;

    f32 result = 0.f;
    for (s32 dz = 0; dz <= 1; ++dz) {
        for (s32 dy = 0; dy <= 1; ++dy) {
            for (s32 dx = 0; dx <= 1; ++dx) {
                const f32 w = (dx != 0 ? tx : 1.f - tx) * (dy != 0 ? ty : 1.f - ty) * (dz != 0 ? tz : 1.f - tz);
                result += w * sdfAtVoxel({base.x + dx, base.y + dy, base.z + dz});
            }
        }
    }
    return result;
}

bool SVO::rayCast(vec3 rayOrigin, vec3 rayDirection, f32 maxDistance, ivec3& hitVoxel, vec3& hitNormal,
                  f32& hitDistance) const {
    if (!m_initialized || m_voxelCount == 0u) {
        return false;
    }
    const f32 length = rayDirection.length();
    if (length < 1e-8f) {
        return false;
    }
    const vec3 d = rayDirection * (1.f / length);
    const f32 size = leafSize();
    const s32 resolution = static_cast<s32>(1u << m_desc.maxDepth);
    const f32 dir[3] = {d.x, d.y, d.z};
    const f32 org[3] = {rayOrigin.x - m_desc.origin.x, rayOrigin.y - m_desc.origin.y, rayOrigin.z - m_desc.origin.z};
    const f32 extent = size * static_cast<f32>(resolution);

    // Slab test against the grid bounds; remember which face the ray enters through.
    f32 tEnter = 0.f;
    f32 tExit = std::numeric_limits<f32>::max();
    int enterAxis = -1;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(dir[a]) < 1e-12f) {
            if (org[a] < 0.f || org[a] > extent) {
                return false;
            }
            continue;
        }
        f32 t0 = (0.f - org[a]) / dir[a];
        f32 t1 = (extent - org[a]) / dir[a];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        if (t0 > tEnter) {
            tEnter = t0;
            enterAxis = a;
        }
        tExit = std::min(tExit, t1);
    }
    if (tEnter > tExit || tEnter > maxDistance) {
        return false;
    }

    // Amanatides & Woo 3D-DDA from the entry point. Boundary crossings are evaluated in closed
    // form so that jumps over empty octree cells and bricks land exactly on the voxel grid.
    constexpr f32 kInf = std::numeric_limits<f32>::max();
    s32 cell[3];
    s32 step[3];
    f32 tMax[3];
    const auto nextBoundary = [&](int a) {
        if (step[a] == 0) {
            return kInf;
        }
        const s32 plane = step[a] > 0 ? cell[a] + 1 : cell[a];
        return (static_cast<f32>(plane) * size - org[a]) / dir[a];
    };
    for (int a = 0; a < 3; ++a) {
        const f32 p = org[a] + dir[a] * tEnter;
        cell[a] = std::clamp(static_cast<s32>(std::floor(p / size)), 0, resolution - 1);
        step[a] = dir[a] > 0.f ? 1 : (dir[a] < 0.f ? -1 : 0);
        tMax[a] = nextBoundary(a);
    }

    // Brick cache: consecutive voxels usually share a brick, so walk the tree once per brick.
    const s32 log = static_cast<s32>(m_brickLog);
    const s32 edge = 1 << log;
    u32 brick = kNoNode;
    ivec3 brickCoord{-1, -1, -1};
    bool brickEmpty = false;
    u64 mask[8] = {};

    f32 t = tEnter;
    int axis = enterAxis;
    while (t <= maxDistance) {
        const ivec3 coord{cell[0], cell[1], cell[2]};
        const ivec3 bc{coord.x >> log, coord.y >> log, coord.z >> log};
        u32 emptySize = 1u;
        if (bc.x != brickCoord.x || bc.y != brickCoord.y || bc.z != brickCoord.z) {
            brick = locate(coord, emptySize);
            if (brick != kNoNode) {
                brickCoord = bc;
                const u8 mode = m_bricks[brick].mode;
                brickEmpty = mode == kModeUniform ? m_bricks[brick].payload == 0u
                                                  : (mode != kModeDense && !solidMask(brick, mask));
            } else {
                brickCoord = {-1, -1, -1};
            }
        }

        if (brick != kNoNode) {
            const SVOBrick& b = m_bricks[brick];
            bool solid = false;
            if (brickEmpty) {
                emptySize = static_cast<u32>(edge);
            } else if (b.mode == kModeUniform) {
                solid = true;
            } else {
                const u32 local = localIndex(coord);
                solid = b.mode != kModeDense ? ((mask[local >> 6u] >> (local & 63u)) & 1ull) != 0u
                                             : m_pool[b.payload + local] != 0u;
            }
            if (solid) {
                hitVoxel = coord;
                hitDistance = t;
                hitNormal = vec3(0.f, 0.f, 0.f);
                if (axis >= 0) {
                    const f32 n = dir[axis] > 0.f ? -1.f : 1.f; // face the ray entered through
                    hitNormal = vec3(axis == 0 ? n : 0.f, axis == 1 ? n : 0.f, axis == 2 ? n : 0.f);
                }
                return true;
            }
        }

        if (emptySize == 1u) {
            axis = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
            t = tMax[axis];
            cell[axis] += step[axis];
            if (cell[axis] < 0 || cell[axis] >= resolution) {
                return false;
            }
            tMax[axis] = nextBoundary(axis);
            continue;
        }

        // Jump out of the empty aligned cube containing the current cell.
        const s32 cubeSize = static_cast<s32>(emptySize);
        s32 cubeMin[3];
        f32 faceT[3];
        for (int a = 0; a < 3; ++a) {
            cubeMin[a] = cell[a] & ~(cubeSize - 1);
            faceT[a] = step[a] == 0 ? kInf
                                    : (static_cast<f32>(step[a] > 0 ? cubeMin[a] + cubeSize : cubeMin[a]) * size -
                                       org[a]) /
                                          dir[a];
        }
        axis = faceT[0] < faceT[1] ? (faceT[0] < faceT[2] ? 0 : 2) : (faceT[1] < faceT[2] ? 1 : 2);
        t = faceT[axis];
        for (int a = 0; a < 3; ++a) {
            if (a == axis || step[a] == 0) {
                continue;
            }
            const s32 inside = static_cast<s32>(std::floor((org[a] + dir[a] * t) / size));
            cell[a] = std::max(cell[a] * step[a], std::clamp(inside, cubeMin[a], cubeMin[a] + cubeSize - 1) * step[a]) *
                      step[a]; // never step backwards
            tMax[a] = nextBoundary(a);
        }
        cell[axis] = step[axis] > 0 ? cubeMin[axis] + cubeSize : cubeMin[axis] - 1;
        if (cell[axis] < 0 || cell[axis] >= resolution) {
            return false;
        }
        tMax[axis] = nextBoundary(axis);
    }
    return false;
}

} // namespace fuse::scene
