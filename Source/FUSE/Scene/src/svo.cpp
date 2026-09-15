#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::scene {
namespace {

u32 packCoord(ivec3 coord) {
    return (static_cast<u32>(coord.x) & 0x3FFu) | ((static_cast<u32>(coord.y) & 0x3FFu) << 10u) |
           ((static_cast<u32>(coord.z) & 0x3FFu) << 20u);
}

} // namespace

void SVO::init(const SVODesc& desc) {
    destroy();
    m_desc = desc;
    m_nodes.clear();
    m_nodes.push_back(SVONode{});
    m_nodes[0].depth = 0;
    m_nodes[0].isLeaf = true;
    m_voxelKeys.clear();
    m_voxelMaterials.clear();
    m_initialized = true;
}

void SVO::destroy() {
    m_desc = {};
    m_nodes.clear();
    m_voxelKeys.clear();
    m_voxelMaterials.clear();
    m_initialized = false;
}

bool SVO::inBounds(ivec3 coord) const {
    const s32 resolution = static_cast<s32>(1u << m_desc.maxDepth);
    return coord.x >= 0 && coord.y >= 0 && coord.z >= 0 && coord.x < resolution && coord.y < resolution &&
           coord.z < resolution;
}

f32 SVO::leafSize() const {
    if (m_desc.maxDepth == 0) {
        return m_desc.rootSize;
    }
    return m_desc.rootSize / static_cast<f32>(1u << m_desc.maxDepth);
}

vec3 SVO::voxelCenterWorld(ivec3 coord) const {
    const f32 size = leafSize();
    return m_desc.origin + toVec3(coord) * size + vec3(size * 0.5f, size * 0.5f, size * 0.5f);
}

f32 SVO::sdfAtVoxel(ivec3 coord) const {
    const u32 key = packCoord(coord);
    for (usize i = 0; i < m_voxelKeys.size(); ++i) {
        if (m_voxelKeys[i] == key) {
            return m_voxelMaterials[i] != 0u ? -leafSize() * 0.5f : leafSize() * 0.5f;
        }
    }
    return leafSize() * 0.5f;
}

void SVO::updateSdfAt(ivec3 coord) {
    if (!m_desc.storeSdf) {
        return;
    }

    const u32 nodeIndex = ensureLeafNode(coord, static_cast<u8>(m_desc.maxDepth));
    if (nodeIndex < m_nodes.size()) {
        m_nodes[nodeIndex].sdfValue = sdfAtVoxel(coord);
    }
}

u32 SVO::ensureLeafNode(ivec3 coord, u8 depth) {
    if (m_nodes.empty()) {
        m_nodes.push_back(SVONode{});
    }

    u32 nodeIndex = 0;
    if (depth == 0) {
        return nodeIndex;
    }

    for (u8 currentDepth = 0; currentDepth < depth; ++currentDepth) {
        SVONode& node = m_nodes[nodeIndex];
        const u32 shift = m_desc.maxDepth - currentDepth - 1;
        const u32 mask = 1u << shift;
        const u32 octant = ((coord.x & mask) ? 1u : 0u) | ((coord.y & mask) ? 2u : 0u) |
                           ((coord.z & mask) ? 4u : 0u);

        if (node.children[octant] == 0) {
            const u32 childIndex = static_cast<u32>(m_nodes.size());
            m_nodes.push_back(SVONode{});
            m_nodes[childIndex].parent = nodeIndex;
            m_nodes[childIndex].depth = static_cast<u8>(currentDepth + 1);
            m_nodes[childIndex].isLeaf = (currentDepth + 1) == depth;
            node.children[octant] = childIndex;
            node.isLeaf = false;
        }

        nodeIndex = node.children[octant];
    }

    return nodeIndex;
}

void SVO::set(ivec3 voxelCoord, u32 material) {
    if (!m_initialized || !inBounds(voxelCoord)) {
        return;
    }

    const u32 key = packCoord(voxelCoord);
    for (usize i = 0; i < m_voxelKeys.size(); ++i) {
        if (m_voxelKeys[i] == key) {
            if (material == 0) {
                m_voxelKeys.erase(m_voxelKeys.begin() + static_cast<std::ptrdiff_t>(i));
                m_voxelMaterials.erase(m_voxelMaterials.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                m_voxelMaterials[i] = material;
            }
            updateSdfAt(voxelCoord);
            return;
        }
    }

    if (material == 0) {
        return;
    }

    m_voxelKeys.push_back(key);
    m_voxelMaterials.push_back(material);

    const u32 nodeIndex = ensureLeafNode(voxelCoord, static_cast<u8>(m_desc.maxDepth));
    if (nodeIndex < m_nodes.size()) {
        m_nodes[nodeIndex].voxelData = material;
    }
    updateSdfAt(voxelCoord);
}

u32 SVO::get(ivec3 voxelCoord) const {
    if (!m_initialized || !inBounds(voxelCoord)) {
        return 0;
    }

    const u32 key = packCoord(voxelCoord);
    for (usize i = 0; i < m_voxelKeys.size(); ++i) {
        if (m_voxelKeys[i] == key) {
            return m_voxelMaterials[i];
        }
    }
    return 0;
}

void SVO::fill(ivec3 minCoord, ivec3 maxCoord, u32 material) {
    const s32 x0 = std::min(minCoord.x, maxCoord.x);
    const s32 y0 = std::min(minCoord.y, maxCoord.y);
    const s32 z0 = std::min(minCoord.z, maxCoord.z);
    const s32 x1 = std::max(minCoord.x, maxCoord.x);
    const s32 y1 = std::max(minCoord.y, maxCoord.y);
    const s32 z1 = std::max(minCoord.z, maxCoord.z);

    for (s32 z = z0; z <= z1; ++z) {
        for (s32 y = y0; y <= y1; ++y) {
            for (s32 x = x0; x <= x1; ++x) {
                set({x, y, z}, material);
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

    for (s32 dz = -extent; dz <= extent; ++dz) {
        for (s32 dy = -extent; dy <= extent; ++dy) {
            for (s32 dx = -extent; dx <= extent; ++dx) {
                const ivec3 coord{centerVoxel.x + dx, centerVoxel.y + dy, centerVoxel.z + dz};
                if (!inBounds(coord)) {
                    continue;
                }

                const vec3 voxelCenter = voxelCenterWorld(coord);
                const f32 distance = (voxelCenter - center).length();
                if (distance > radius + size * 0.5f) {
                    continue;
                }

                const f32 sphereSdf = distance - radius;
                const bool wasSolid = get(coord) != 0u;

                if (sphereSdf <= 0.f && wasSolid) {
                    set(coord, 0);
                } else if (sphereSdf > 0.f && !wasSolid && m_desc.storeSdf && sphereSdf <= size) {
                    // Surface band — scaffold marks boundary voxels for dual contour follow-up.
                    set(coord, 1);
                } else if (m_desc.storeSdf) {
                    updateSdfAt(coord);
                }
            }
        }
    }
}

f32 SVO::sdfQuery(vec3 worldPos) const {
    if (!m_initialized) {
        return std::numeric_limits<f32>::max();
    }

    const f32 size = leafSize();
    const vec3 local = worldPos - m_desc.origin;
    const ivec3 base{
        static_cast<s32>(std::floor(local.x / size)),
        static_cast<s32>(std::floor(local.y / size)),
        static_cast<s32>(std::floor(local.z / size)),
    };

    f32 best = std::numeric_limits<f32>::max();
    for (s32 dz = 0; dz <= 1; ++dz) {
        for (s32 dy = 0; dy <= 1; ++dy) {
            for (s32 dx = 0; dx <= 1; ++dx) {
                const ivec3 coord{base.x + dx, base.y + dy, base.z + dz};
                if (!inBounds(coord)) {
                    continue;
                }
                const vec3 samplePos = voxelCenterWorld(coord);
                const f32 weight = 1.f / (1.f + (samplePos - worldPos).length());
                const f32 sampleSdf = m_desc.storeSdf ? sdfAtVoxel(coord) : sdfAtVoxel(coord);
                best = std::min(best, sampleSdf + (1.f - weight) * size);
            }
        }
    }
    return best;
}

bool SVO::rayCast(vec3 rayOrigin, vec3 rayDirection, f32 maxDistance, ivec3& hitVoxel, vec3& hitNormal,
                  f32& hitDistance) const {
    if (!m_initialized || m_voxelMaterials.empty()) {
        return false;
    }

    const vec3 direction = rayDirection.normalized();
    const f32 step = leafSize() * 0.5f;
    f32 traveled = 0.f;

    while (traveled <= maxDistance) {
        const vec3 sample = rayOrigin + direction * traveled;
        const vec3 local = sample - m_desc.origin;
        const ivec3 coord{
            static_cast<s32>(std::floor(local.x / leafSize())),
            static_cast<s32>(std::floor(local.y / leafSize())),
            static_cast<s32>(std::floor(local.z / leafSize())),
        };

        if (inBounds(coord) && get(coord) != 0u) {
            hitVoxel = coord;
            hitDistance = traveled;
            const vec3 center = voxelCenterWorld(coord);
            hitNormal = (center - rayOrigin).normalized();
            return true;
        }

        traveled += step;
    }

    return false;
}

} // namespace fuse::scene
