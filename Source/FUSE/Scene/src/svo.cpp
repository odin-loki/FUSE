#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace fuse::scene {

namespace {

// Brick payload forms (SVOBrick::mode) and the brick size are shared with the ray-cast kernel.
using svo_kernel::kModeDense;
using svo_kernel::kModeMasked;
using svo_kernel::kModeSparse;
using svo_kernel::kModeUniform;

// Bricks are 8^3 voxels: at depth 10 that stops the octree three levels early, so scattered
// data needs ~1/8 of the interior nodes of per-voxel leaves, while sparse payloads keep a
// lone voxel at 8 bytes. 4^3 bricks would need ~3.5x the interior nodes for scattered data
// and 2^3 bricks gain almost nothing over per-voxel leaves.
constexpr u32 kMaxBrickLog = svo_kernel::kMaxBrickLog;

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

svo_kernel::SvoView SVO::view() const {
    svo_kernel::SvoView v{};
    v.nodes = kernel::make_span(m_nodes.data(), static_cast<u32>(m_nodes.size()));
    v.bricks = kernel::make_span(m_bricks.data(), static_cast<u32>(m_bricks.size()));
    v.pool = kernel::make_span(m_pool.data(), static_cast<u32>(m_pool.size()));
    v.origin[0] = m_desc.origin.x;
    v.origin[1] = m_desc.origin.y;
    v.origin[2] = m_desc.origin.z;
    v.leaf_size = leafSize();
    v.max_depth = m_desc.maxDepth;
    v.brick_log = m_brickLog;
    v.brick_depth = m_brickDepth;
    v.brick_voxels = m_brickVoxels;
    v.mask_words = m_maskWords;
    v.initialized = m_initialized ? 1u : 0u;
    v.has_voxels = m_voxelCount != 0u ? 1u : 0u;
    return v;
}

bool SVO::inBounds(ivec3 coord) const {
    return svo_kernel::in_bounds(view(), coord.x, coord.y, coord.z);
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
    return svo_kernel::local_index(view(), coord.x, coord.y, coord.z);
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
    return svo_kernel::find_brick(view(), coord.x, coord.y, coord.z);
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

using svo_kernel::sparse_lower_bound;

} // namespace

SVO::VoxelState SVO::readVoxel(u32 brick, u32 local) const {
    VoxelState state{};
    state.written = svo_kernel::read_voxel(view(), brick, local, state.material);
    const SVOBrick& b = m_bricks[brick];
    if (b.mode == kModeDense) {
        const u32 plane = m_pool[denseSdfSlot(brick)];
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
        const u32 slot = sparse_lower_bound(m_pool.data() + b.payload, b.count, local);
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
    return svo_kernel::voxel_material(view(), voxelCoord.x, voxelCoord.y, voxelCoord.z);
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
    const SvoRay ray{{rayOrigin.x, rayOrigin.y, rayOrigin.z}, {rayDirection.x, rayDirection.y, rayDirection.z},
                     maxDistance};
    SvoRayHit hit{};
    if (!svo_kernel::ray_cast(view(), ray, hit)) {
        return false;
    }
    hitVoxel = {hit.voxel[0], hit.voxel[1], hit.voxel[2]};
    hitNormal = vec3(hit.normal[0], hit.normal[1], hit.normal[2]);
    hitDistance = hit.distance;
    return true;
}

} // namespace fuse::scene
