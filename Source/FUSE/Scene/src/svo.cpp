#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::scene {

void SVO::init(const SVODesc& desc) {
    destroy();
    m_desc = desc;
    m_nodes.push_back(SVONode{});
    m_nodes[0].isLeaf = m_desc.maxDepth == 0;
    m_nodes[0].sdfValue = leafSize() * 0.5f;
    m_initialized = true;
}

void SVO::destroy() {
    m_desc = {};
    m_nodes.clear();
    m_voxelCount = 0;
    m_initialized = false;
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

u32 SVO::findLeaf(ivec3 coord) const {
    if (m_nodes.empty() || !inBounds(coord)) {
        return kNoNode;
    }
    u32 nodeIndex = 0;
    for (u32 depth = 0; depth < m_desc.maxDepth; ++depth) {
        const u32 mask = 1u << (m_desc.maxDepth - depth - 1u);
        const u32 octant = ((static_cast<u32>(coord.x) & mask) ? 1u : 0u) |
                           ((static_cast<u32>(coord.y) & mask) ? 2u : 0u) |
                           ((static_cast<u32>(coord.z) & mask) ? 4u : 0u);
        const u32 child = m_nodes[nodeIndex].children[octant];
        if (child == 0u) {
            return kNoNode; // index 0 is the root, so it never appears as a child
        }
        nodeIndex = child;
    }
    return nodeIndex;
}

u32 SVO::ensureLeafNode(ivec3 coord) {
    u32 nodeIndex = 0;
    for (u32 depth = 0; depth < m_desc.maxDepth; ++depth) {
        const u32 mask = 1u << (m_desc.maxDepth - depth - 1u);
        const u32 octant = ((static_cast<u32>(coord.x) & mask) ? 1u : 0u) |
                           ((static_cast<u32>(coord.y) & mask) ? 2u : 0u) |
                           ((static_cast<u32>(coord.z) & mask) ? 4u : 0u);
        u32 child = m_nodes[nodeIndex].children[octant];
        if (child == 0u) {
            child = static_cast<u32>(m_nodes.size());
            SVONode node{};
            node.parent = nodeIndex;
            node.depth = static_cast<u8>(depth + 1u);
            node.isLeaf = depth + 1u == m_desc.maxDepth;
            node.sdfValue = leafSize() * 0.5f; // empty until written
            m_nodes.push_back(node);
            m_nodes[nodeIndex].children[octant] = child;
            m_nodes[nodeIndex].isLeaf = false;
        }
        nodeIndex = child;
    }
    return nodeIndex;
}

f32 SVO::sdfAtVoxel(ivec3 coord) const {
    const f32 half = leafSize() * 0.5f;
    const u32 leaf = findLeaf(coord);
    if (leaf == kNoNode) {
        return half; // never written: empty
    }
    if (m_desc.storeSdf) {
        return m_nodes[leaf].sdfValue;
    }
    return m_nodes[leaf].voxelData != 0u ? -half : half;
}

void SVO::set(ivec3 voxelCoord, u32 material) {
    if (!m_initialized || !inBounds(voxelCoord)) {
        return;
    }

    const f32 half = leafSize() * 0.5f;
    if (material == 0u) {
        const u32 leaf = findLeaf(voxelCoord);
        if (leaf == kNoNode) {
            return;
        }
        if (m_nodes[leaf].voxelData != 0u) {
            --m_voxelCount;
        }
        m_nodes[leaf].voxelData = 0u;
        m_nodes[leaf].sdfValue = half;
        return;
    }

    const u32 leaf = ensureLeafNode(voxelCoord);
    if (m_nodes[leaf].voxelData == 0u) {
        ++m_voxelCount;
    }
    m_nodes[leaf].voxelData = material;
    m_nodes[leaf].sdfValue = -half;
}

u32 SVO::get(ivec3 voxelCoord) const {
    if (!m_initialized) {
        return 0u;
    }
    const u32 leaf = findLeaf(voxelCoord);
    return leaf == kNoNode ? 0u : m_nodes[leaf].voxelData;
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
                const u32 leaf = findLeaf(coord);
                if (leaf == kNoNode) {
                    continue; // empty space stays empty
                }

                const f32 distance = (voxelCenterWorld(coord) - center).length();
                if (distance > radius + size) {
                    continue;
                }

                // CSG subtraction: sdf = max(sdf, -sphere) where sphere = distance - radius.
                SVONode& node = m_nodes[leaf];
                const f32 current = m_desc.storeSdf ? node.sdfValue : (node.voxelData != 0u ? -size * 0.5f : size * 0.5f);
                const f32 carved = std::max(current, radius - distance);
                if (m_desc.storeSdf) {
                    node.sdfValue = carved;
                }
                if (carved > 0.f && node.voxelData != 0u) {
                    node.voxelData = 0u; // surface -> empty transition
                    --m_voxelCount;
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

    // Amanatides & Woo 3D-DDA from the entry point.
    s32 cell[3];
    s32 step[3];
    f32 tMax[3];
    f32 tDelta[3];
    for (int a = 0; a < 3; ++a) {
        const f32 p = org[a] + dir[a] * tEnter;
        cell[a] = std::clamp(static_cast<s32>(std::floor(p / size)), 0, resolution - 1);
        if (dir[a] > 0.f) {
            step[a] = 1;
            tMax[a] = (static_cast<f32>(cell[a] + 1) * size - org[a]) / dir[a];
            tDelta[a] = size / dir[a];
        } else if (dir[a] < 0.f) {
            step[a] = -1;
            tMax[a] = (static_cast<f32>(cell[a]) * size - org[a]) / dir[a];
            tDelta[a] = -size / dir[a];
        } else {
            step[a] = 0;
            tMax[a] = std::numeric_limits<f32>::max();
            tDelta[a] = std::numeric_limits<f32>::max();
        }
    }

    f32 t = tEnter;
    int axis = enterAxis;
    while (t <= maxDistance) {
        const ivec3 coord{cell[0], cell[1], cell[2]};
        if (get(coord) != 0u) {
            hitVoxel = coord;
            hitDistance = t;
            hitNormal = vec3(0.f, 0.f, 0.f);
            if (axis >= 0) {
                const f32 n = dir[axis] > 0.f ? -1.f : 1.f; // face the ray entered through
                hitNormal = vec3(axis == 0 ? n : 0.f, axis == 1 ? n : 0.f, axis == 2 ? n : 0.f);
            }
            return true;
        }
        axis = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
        t = tMax[axis];
        cell[axis] += step[axis];
        if (cell[axis] < 0 || cell[axis] >= resolution) {
            return false;
        }
        tMax[axis] += tDelta[axis];
    }
    return false;
}

} // namespace fuse::scene
