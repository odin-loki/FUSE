#include <fuse/physics/spatial/svo.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {

namespace {

ivec3 add(ivec3 a, ivec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

ivec3 unit(int axis) {
    return {axis == 0 ? 1 : 0, axis == 1 ? 1 : 0, axis == 2 ? 1 : 0};
}

} // namespace

void VoxelVolume::init(vec3 origin, f32 voxelSize, ivec3 dims) {
    m_origin = origin;
    m_size = voxelSize > 0.f ? voxelSize : 1.f;
    m_dims = {std::max(dims.x, 0), std::max(dims.y, 0), std::max(dims.z, 0)};
    m_voxels.assign(static_cast<usize>(m_dims.x) * m_dims.y * m_dims.z, 0u);
    m_solid = 0;
}

bool VoxelVolume::inBounds(ivec3 v) const {
    return v.x >= 0 && v.y >= 0 && v.z >= 0 && v.x < m_dims.x && v.y < m_dims.y && v.z < m_dims.z;
}

usize VoxelVolume::linear(ivec3 v) const {
    return (static_cast<usize>(v.z) * m_dims.y + v.y) * m_dims.x + v.x;
}

u8 VoxelVolume::get(ivec3 v) const {
    return inBounds(v) ? m_voxels[linear(v)] : 0u;
}

void VoxelVolume::set(ivec3 v, u8 material) {
    if (!inBounds(v)) {
        return;
    }
    u8& slot = m_voxels[linear(v)];
    m_solid += (material != 0u ? 1u : 0u) - (slot != 0u ? 1u : 0u);
    slot = material;
}

void VoxelVolume::fill(ivec3 minCorner, ivec3 maxCorner, u8 material) {
    for (s32 z = std::max(minCorner.z, 0); z <= std::min(maxCorner.z, m_dims.z - 1); ++z) {
        for (s32 y = std::max(minCorner.y, 0); y <= std::min(maxCorner.y, m_dims.y - 1); ++y) {
            for (s32 x = std::max(minCorner.x, 0); x <= std::min(maxCorner.x, m_dims.x - 1); ++x) {
                set({x, y, z}, material);
            }
        }
    }
}

vec3 VoxelVolume::voxelCenter(ivec3 v) const {
    return {m_origin.x + (static_cast<f32>(v.x) + 0.5f) * m_size, m_origin.y + (static_cast<f32>(v.y) + 0.5f) * m_size,
            m_origin.z + (static_cast<f32>(v.z) + 0.5f) * m_size};
}

ivec3 VoxelVolume::voxelAt(vec3 world) const {
    return {static_cast<s32>(std::floor((world.x - m_origin.x) / m_size)),
            static_cast<s32>(std::floor((world.y - m_origin.y) / m_size)),
            static_cast<s32>(std::floor((world.z - m_origin.z) / m_size))};
}

u32 VoxelVolume::carve(vec3 center, f32 radius) {
    if (radius <= 0.f) {
        return 0;
    }
    const ivec3 lo = voxelAt(center - vec3{radius, radius, radius});
    const ivec3 hi = voxelAt(center + vec3{radius, radius, radius});
    u32 removed = 0;
    for (s32 z = std::max(lo.z, 0); z <= std::min(hi.z, m_dims.z - 1); ++z) {
        for (s32 y = std::max(lo.y, 0); y <= std::min(hi.y, m_dims.y - 1); ++y) {
            for (s32 x = std::max(lo.x, 0); x <= std::min(hi.x, m_dims.x - 1); ++x) {
                const ivec3 v{x, y, z};
                if (get(v) != 0u && (voxelCenter(v) - center).length() < radius) {
                    set(v, 0u);
                    ++removed;
                }
            }
        }
    }
    return removed;
}

void VoxelVolume::extractSurface(std::vector<vec3>& vertices, std::vector<u32>& indices) const {
    vertices.clear();
    indices.clear();
    // Dual cell c (per axis in [-1, dims - 1]) has the voxel centres c + {0, 1}^3 as corners.
    const ivec3 cells{m_dims.x + 1, m_dims.y + 1, m_dims.z + 1};
    std::vector<u32> cellVertex(static_cast<usize>(cells.x) * cells.y * cells.z, ~0u);
    const auto cellIndex = [&](ivec3 c) {
        return (static_cast<usize>(c.z + 1) * cells.y + (c.y + 1)) * cells.x + (c.x + 1);
    };
    const auto solid = [&](ivec3 v) { return get(v) != 0u; };
    const auto vertexFor = [&](ivec3 c) {
        u32& slot = cellVertex[cellIndex(c)];
        if (slot != ~0u) {
            return slot;
        }
        // Mass point of the sign-changing cell edges (edge crossings sit at edge midpoints for
        // a binary field): the dual-contouring vertex without the QEF refinement.
        vec3 sum{};
        u32 crossings = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const int b = (axis + 1) % 3;
            const int d = (axis + 2) % 3;
            for (int i = 0; i < 2; ++i) {
                for (int j = 0; j < 2; ++j) {
                    const ivec3 p = add(add(c, i != 0 ? unit(b) : ivec3{}), j != 0 ? unit(d) : ivec3{});
                    const ivec3 q = add(p, unit(axis));
                    if (solid(p) != solid(q)) {
                        sum += (voxelCenter(p) + voxelCenter(q)) * 0.5f;
                        ++crossings;
                    }
                }
            }
        }
        slot = static_cast<u32>(vertices.size());
        vertices.push_back(sum * (1.f / static_cast<f32>(std::max(crossings, 1u))));
        return slot;
    };

    for (int axis = 0; axis < 3; ++axis) {
        const int b = (axis + 1) % 3;
        const int d = (axis + 2) % 3;
        const ivec3 e = unit(axis);
        const ivec3 eb = unit(b);
        const ivec3 ed = unit(d);
        for (s32 z = (axis == 2 ? -1 : 0); z < m_dims.z; ++z) {
            for (s32 y = (axis == 1 ? -1 : 0); y < m_dims.y; ++y) {
                for (s32 x = (axis == 0 ? -1 : 0); x < m_dims.x; ++x) {
                    const ivec3 p{x, y, z};
                    const bool inside = solid(p);
                    if (inside == solid(add(p, e))) {
                        continue;
                    }
                    // The four dual cells around the edge p -> p + e.
                    const ivec3 base{p.x - eb.x - ed.x, p.y - eb.y - ed.y, p.z - eb.z - ed.z};
                    const u32 v00 = vertexFor(base);
                    const u32 v10 = vertexFor(add(base, eb));
                    const u32 v11 = vertexFor(add(add(base, eb), ed));
                    const u32 v01 = vertexFor(add(base, ed));
                    // e_b x e_d = e_axis: this winding faces +axis, i.e. out of a solid at p.
                    if (inside) {
                        indices.insert(indices.end(), {v00, v10, v11, v00, v11, v01});
                    } else {
                        indices.insert(indices.end(), {v00, v11, v10, v00, v01, v11});
                    }
                }
            }
        }
    }
}

std::vector<VoxelFragment> VoxelVolume::detachFloating() {
    std::vector<VoxelFragment> fragments;
    std::vector<s32> label(m_voxels.size(), -1);
    std::vector<ivec3> stack;
    std::vector<u8> anchored;
    s32 next = 0;
    std::vector<std::vector<ivec3>> members;
    for (s32 z = 0; z < m_dims.z; ++z) {
        for (s32 y = 0; y < m_dims.y; ++y) {
            for (s32 x = 0; x < m_dims.x; ++x) {
                const ivec3 seed{x, y, z};
                if (get(seed) == 0u || label[linear(seed)] >= 0) {
                    continue;
                }
                const s32 id = next++;
                members.emplace_back();
                anchored.push_back(0u);
                label[linear(seed)] = id;
                stack.push_back(seed);
                while (!stack.empty()) {
                    const ivec3 v = stack.back();
                    stack.pop_back();
                    members[id].push_back(v);
                    anchored[id] = anchored[id] != 0u || v.y == 0 ? 1u : 0u;
                    for (int axis = 0; axis < 3; ++axis) {
                        for (int sign = -1; sign <= 1; sign += 2) {
                            const ivec3 n = add(v, {axis == 0 ? sign : 0, axis == 1 ? sign : 0, axis == 2 ? sign : 0});
                            if (get(n) != 0u && label[linear(n)] < 0) {
                                label[linear(n)] = id;
                                stack.push_back(n);
                            }
                        }
                    }
                }
            }
        }
    }
    for (s32 id = 0; id < next; ++id) {
        if (anchored[id] != 0u) {
            continue;
        }
        VoxelFragment fragment{};
        fragment.voxels = std::move(members[id]);
        fragment.min = fragment.voxels.front();
        fragment.max = fragment.voxels.front();
        vec3 sum{};
        for (const ivec3& v : fragment.voxels) {
            fragment.min = {std::min(fragment.min.x, v.x), std::min(fragment.min.y, v.y), std::min(fragment.min.z, v.z)};
            fragment.max = {std::max(fragment.max.x, v.x), std::max(fragment.max.y, v.y), std::max(fragment.max.z, v.z)};
            sum += voxelCenter(v);
            set(v, 0u);
        }
        fragment.centerOfMass = sum * (1.f / static_cast<f32>(fragment.voxels.size()));
        fragments.push_back(std::move(fragment));
    }
    return fragments;
}

} // namespace fuse::physics
