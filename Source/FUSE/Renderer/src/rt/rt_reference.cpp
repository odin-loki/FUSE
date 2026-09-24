// WP-6.0 CPU reference (see include/fuse/renderer/rt/rt_reference.hpp).
#include <fuse/renderer/rt/rt_reference.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer::rt {

namespace {

using ecs::vec3;

struct D3 {
    f64 x = 0.0, y = 0.0, z = 0.0;
};
D3 sub(const D3& a, const D3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 cross(const D3& a, const D3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
f64 dot(const D3& a, const D3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// Growth of the leaf / instance bounds so the f32 slab tests never cull a surface the (possibly
/// grown) triangle test would accept.
constexpr f32 kBoundsRelative = 1.0e-3f;
constexpr f32 kBoundsAbsolute = 1.0e-5f;

spatial::AABB grow(spatial::AABB box) {
    const f32 ex = (box.max.x - box.min.x), ey = (box.max.y - box.min.y), ez = (box.max.z - box.min.z);
    const f32 pad = kBoundsAbsolute + kBoundsRelative * std::max(ex, std::max(ey, ez));
    box.min.x -= pad;
    box.min.y -= pad;
    box.min.z -= pad;
    box.max.x += pad;
    box.max.y += pad;
    box.max.z += pad;
    return box;
}

bool invert(const f64 m[3][4], f64 out[3][4]) {
    const f64 a = m[0][0], b = m[0][1], c = m[0][2];
    const f64 d = m[1][0], e = m[1][1], f = m[1][2];
    const f64 g = m[2][0], h = m[2][1], i = m[2][2];
    const f64 det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1.0e-300) {
        return false;
    }
    const f64 r = 1.0 / det;
    f64 l[3][3] = {{(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r},
                   {(f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r},
                   {(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}};
    for (u32 row = 0; row < 3u; ++row) {
        for (u32 col = 0; col < 3u; ++col) {
            out[row][col] = l[row][col];
        }
        out[row][3] = -(l[row][0] * m[0][3] + l[row][1] * m[1][3] + l[row][2] * m[2][3]);
    }
    return true;
}

/// Moller-Trumbore in double; barycentrics (u, v) weight vertices 1 and 2 (Vulkan's convention).
bool intersectTriangle(const D3& o, const D3& d, const D3& v0, const D3& v1, const D3& v2, f64 eps, f64 tMin, f64 tMax,
                       f64& t, f64& u, f64& v) {
    const D3 e1 = sub(v1, v0);
    const D3 e2 = sub(v2, v0);
    const D3 p = cross(d, e2);
    const f64 det = dot(e1, p);
    if (!(std::abs(det) > 1.0e-300)) {
        return false;
    }
    const f64 inv = 1.0 / det;
    const D3 s = sub(o, v0);
    u = dot(s, p) * inv;
    const D3 q = cross(s, e1);
    v = dot(d, q) * inv;
    if (u < -eps || v < -eps || u + v > 1.0 + eps) {
        return false;
    }
    t = dot(e2, q) * inv;
    return t >= tMin && t <= tMax;
}

} // namespace

void RtReferenceScene::setMesh(u32 mesh, const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount) {
    if (mesh >= m_meshes.size()) {
        m_meshes.resize(static_cast<usize>(mesh) + 1u);
    }
    Mesh& m = m_meshes[mesh];
    m.positions.assign(positions, positions + static_cast<usize>(vertexCount) * 3u);
    m.indices.assign(indices, indices + (indexCount / 3u) * 3u);
    m.present = !m.indices.empty() && vertexCount > 0u;
    buildMeshBvh(m, false);
}

bool RtReferenceScene::setMeshFromScene(const gpu_scene::GpuScene& scene, u32 mesh, const u16* vpos) {
    if (mesh >= scene.meshCount() || vpos == nullptr) {
        return false;
    }
    const GpuMesh& gm = scene.mesh(mesh);
    if (gm.indexCount == 0u || static_cast<u64>(gm.firstIndex) + gm.indexCount > scene.indexCount()) {
        return false;
    }
    std::vector<f32> positions(static_cast<usize>(gm.vertexCount) * 3u);
    for (u32 v = 0; v < gm.vertexCount; ++v) {
        rtDecodePosition(gm, vpos, v, &positions[static_cast<usize>(v) * 3u]);
    }
    setMesh(mesh, positions.data(), gm.vertexCount, scene.indexData() + gm.firstIndex, gm.indexCount);
    return true;
}

bool RtReferenceScene::updateMeshPositions(u32 mesh, const f32* positions, u32 vertexCount) {
    if (!hasMesh(mesh) || static_cast<usize>(vertexCount) * 3u != m_meshes[mesh].positions.size()) {
        return false;
    }
    Mesh& m = m_meshes[mesh];
    m.positions.assign(positions, positions + static_cast<usize>(vertexCount) * 3u);
    buildMeshBvh(m, true);
    return true;
}

spatial::AABB RtReferenceScene::triangleBounds(const Mesh& mesh, u32 triangle) const {
    spatial::AABB box{};
    box.min = vec3{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), 0.f};
    box.max = vec3{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), 0.f};
    for (u32 k = 0; k < 3u; ++k) {
        const f32* p = &mesh.positions[static_cast<usize>(mesh.indices[triangle * 3u + k]) * 3u];
        box.min.x = std::min(box.min.x, p[0]);
        box.min.y = std::min(box.min.y, p[1]);
        box.min.z = std::min(box.min.z, p[2]);
        box.max.x = std::max(box.max.x, p[0]);
        box.max.y = std::max(box.max.y, p[1]);
        box.max.z = std::max(box.max.z, p[2]);
    }
    return grow(box);
}

void RtReferenceScene::buildMeshBvh(Mesh& mesh, bool refit) {
    const u32 triangles = static_cast<u32>(mesh.indices.size() / 3u);
    for (u32 c = 0; c < 3u; ++c) {
        mesh.aabbMin[c] = std::numeric_limits<f32>::max();
        mesh.aabbMax[c] = -std::numeric_limits<f32>::max();
    }
    for (usize v = 0; v < mesh.positions.size() / 3u; ++v) {
        for (u32 c = 0; c < 3u; ++c) {
            mesh.aabbMin[c] = std::min(mesh.aabbMin[c], mesh.positions[v * 3u + c]);
            mesh.aabbMax[c] = std::max(mesh.aabbMax[c], mesh.positions[v * 3u + c]);
        }
    }
    if (refit) {
        for (u32 t = 0; t < triangles; ++t) {
            mesh.bvh.update_leaf_aabb(t, triangleBounds(mesh, t));
        }
        mesh.bvh.refit();
        return;
    }
    std::vector<spatial::BVHLeaf> leaves(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        leaves[t].type = spatial::BVHLeafType::Mesh;
        leaves[t].primitive_index = t;
        leaves[t].aabb = triangleBounds(mesh, t);
    }
    spatial::BVHBuildDesc desc{};
    desc.max_leaf_primitives = 4;
    desc.use_sah = true;
    desc.parallel = false;
    mesh.bvh.build(leaves, desc);
}

bool RtReferenceScene::makeInstance(const GpuInstance& instance, const GpuTransform& transform, u32 slot, Instance& out,
                                    spatial::AABB& bounds) const {
    const bool meshOk = instance.mesh < m_meshes.size() && m_meshes[instance.mesh].present;
    out.mask = rtInstanceMask(instance.flags, meshOk ? 1u : 0u);
    if (out.mask == 0u) {
        return false;
    }
    out.slot = slot;
    out.mesh = instance.mesh;
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            out.m[r][c] = transform.rows[r][c];
        }
    }
    if (!invert(out.m, out.inv)) {
        return false;
    }
    const Mesh& mesh = m_meshes[instance.mesh];
    bounds.min = vec3{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), 0.f};
    bounds.max = vec3{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), 0.f};
    for (u32 corner = 0; corner < 8u; ++corner) {
        const f64 p[3] = {(corner & 1u) != 0u ? mesh.aabbMax[0] : mesh.aabbMin[0],
                          (corner & 2u) != 0u ? mesh.aabbMax[1] : mesh.aabbMin[1],
                          (corner & 4u) != 0u ? mesh.aabbMax[2] : mesh.aabbMin[2]};
        f32 w[3];
        for (u32 r = 0; r < 3u; ++r) {
            w[r] = static_cast<f32>(out.m[r][0] * p[0] + out.m[r][1] * p[1] + out.m[r][2] * p[2] + out.m[r][3]);
        }
        bounds.min.x = std::min(bounds.min.x, w[0]);
        bounds.min.y = std::min(bounds.min.y, w[1]);
        bounds.min.z = std::min(bounds.min.z, w[2]);
        bounds.max.x = std::max(bounds.max.x, w[0]);
        bounds.max.y = std::max(bounds.max.y, w[1]);
        bounds.max.z = std::max(bounds.max.z, w[2]);
    }
    bounds = grow(bounds);
    return true;
}

void RtReferenceScene::buildTop(const GpuInstance* instances, const GpuTransform* transforms, u32 count) {
    m_instances.clear();
    m_leafSlots.clear();
    std::vector<spatial::BVHLeaf> leaves;
    for (u32 slot = 0; slot < count; ++slot) {
        Instance inst{};
        spatial::AABB bounds{};
        if (!makeInstance(instances[slot], transforms[slot], slot, inst, bounds)) {
            continue;
        }
        spatial::BVHLeaf leaf{};
        leaf.type = spatial::BVHLeafType::Mesh;
        leaf.primitive_index = static_cast<u32>(m_instances.size());
        leaf.aabb = bounds;
        leaves.push_back(leaf);
        m_instances.push_back(inst);
        m_leafSlots.push_back(slot);
    }
    spatial::BVHBuildDesc desc{};
    desc.max_leaf_primitives = 2;
    desc.use_sah = true;
    desc.parallel = false;
    m_top.build(leaves, desc);
}

void RtReferenceScene::setInstances(const GpuInstance* instances, const GpuTransform* transforms, u32 count) {
    buildTop(instances, transforms, count);
}

void RtReferenceScene::setInstances(const gpu_scene::GpuScene& scene) {
    const u32 count = scene.instanceHighWater();
    std::vector<GpuInstance> instances(count);
    std::vector<GpuTransform> transforms(count);
    for (u32 i = 0; i < count; ++i) {
        instances[i] = scene.instance(i);
        transforms[i] = scene.transform(i);
    }
    buildTop(instances.data(), transforms.data(), count);
}

bool RtReferenceScene::updateInstances(const gpu_scene::GpuScene& scene) {
    const u32 count = scene.instanceHighWater();
    std::vector<GpuInstance> instances(count);
    std::vector<GpuTransform> transforms(count);
    for (u32 i = 0; i < count; ++i) {
        instances[i] = scene.instance(i);
        transforms[i] = scene.transform(i);
    }
    return updateInstances(instances.data(), transforms.data(), count);
}

bool RtReferenceScene::updateInstances(const GpuInstance* instances, const GpuTransform* transforms, u32 count) {
    // Same traced slot set (and meshes / masks) -> refit, like a TLAS UPDATE.
    u32 traced = 0;
    bool same = true;
    for (u32 slot = 0; slot < count && same; ++slot) {
        Instance inst{};
        spatial::AABB bounds{};
        if (!makeInstance(instances[slot], transforms[slot], slot, inst, bounds)) {
            continue;
        }
        if (traced >= m_instances.size() || m_leafSlots[traced] != slot || m_instances[traced].mesh != inst.mesh ||
            m_instances[traced].mask != inst.mask) {
            same = false;
            break;
        }
        ++traced;
    }
    if (!same || traced != m_instances.size()) {
        buildTop(instances, transforms, count);
        return false;
    }
    for (u32 k = 0; k < traced; ++k) {
        const u32 slot = m_leafSlots[k];
        Instance inst{};
        spatial::AABB bounds{};
        makeInstance(instances[slot], transforms[slot], slot, inst, bounds);
        m_instances[k] = inst;
        m_top.update_leaf_aabb(k, bounds);
    }
    m_top.refit();
    ++m_topRefits;
    return true;
}

RtRefHit RtReferenceScene::trace(const RtProbeRay& ray, u32 cullMask, f64 edgeEpsilon) const {
    RtRefHit best{};
    if (m_top.node_count() == 0u) {
        return best;
    }
    const D3 wo{ray.origin[0], ray.origin[1], ray.origin[2]};
    const D3 wd{ray.direction[0], ray.direction[1], ray.direction[2]};
    const f64 tMin = ray.tMin;
    const vec3 origin{ray.origin[0], ray.origin[1], ray.origin[2], 0.f};
    const vec3 direction{ray.direction[0], ray.direction[1], ray.direction[2], 0.f};

    auto instanceHit = [&](const spatial::BVHLeaf& leaf, f32 maxT, f32& outT) -> bool {
        const Instance& inst = m_instances[leaf.primitive_index];
        if ((inst.mask & cullMask & 0xFFu) == 0u) {
            return false;
        }
        const Mesh& mesh = m_meshes[inst.mesh];
        const D3 o{inst.inv[0][0] * wo.x + inst.inv[0][1] * wo.y + inst.inv[0][2] * wo.z + inst.inv[0][3],
                   inst.inv[1][0] * wo.x + inst.inv[1][1] * wo.y + inst.inv[1][2] * wo.z + inst.inv[1][3],
                   inst.inv[2][0] * wo.x + inst.inv[2][1] * wo.y + inst.inv[2][2] * wo.z + inst.inv[2][3]};
        const D3 d{inst.inv[0][0] * wd.x + inst.inv[0][1] * wd.y + inst.inv[0][2] * wd.z,
                   inst.inv[1][0] * wd.x + inst.inv[1][1] * wd.y + inst.inv[1][2] * wd.z,
                   inst.inv[2][0] * wd.x + inst.inv[2][1] * wd.y + inst.inv[2][2] * wd.z};
        const f64 limit = best.hit ? std::min<f64>(best.t, ray.tMax) : static_cast<f64>(ray.tMax);
        RtRefHit local{};
        local.t = limit;
        auto triangleHit = [&](const spatial::BVHLeaf& tri, f32 triMax, f32& triT) -> bool {
            const u32 t = tri.primitive_index;
            const f32* p0 = &mesh.positions[static_cast<usize>(mesh.indices[t * 3u + 0u]) * 3u];
            const f32* p1 = &mesh.positions[static_cast<usize>(mesh.indices[t * 3u + 1u]) * 3u];
            const f32* p2 = &mesh.positions[static_cast<usize>(mesh.indices[t * 3u + 2u]) * 3u];
            f64 th = 0.0, u = 0.0, v = 0.0;
            const f64 upper = std::min<f64>(local.t, static_cast<f64>(triMax) * (1.0 + 1.0e-6) + 1.0e-6);
            if (!intersectTriangle(o, d, D3{p0[0], p0[1], p0[2]}, D3{p1[0], p1[1], p1[2]}, D3{p2[0], p2[1], p2[2]},
                                   edgeEpsilon, tMin, upper, th, u, v)) {
                return false;
            }
            if (local.hit && th >= local.t) {
                return false;
            }
            local.hit = true;
            local.instance = inst.slot;
            local.primitive = t;
            local.t = th;
            local.u = u;
            local.v = v;
            triT = static_cast<f32>(th);
            return true;
        };
        spatial::BVHLeaf triLeaf{};
        f32 triT = 0.f;
        const vec3 oo{static_cast<f32>(o.x), static_cast<f32>(o.y), static_cast<f32>(o.z), 0.f};
        const vec3 od{static_cast<f32>(d.x), static_cast<f32>(d.y), static_cast<f32>(d.z), 0.f};
        const f32 innerMax = static_cast<f32>(std::min<f64>(limit, static_cast<f64>(maxT)) * (1.0 + 1.0e-6) + 1.0e-6);
        mesh.bvh.ray_cast_exact(oo, od, innerMax, triangleHit, triLeaf, triT);
        if (!local.hit || (best.hit && local.t >= best.t)) {
            return false;
        }
        best = local;
        outT = static_cast<f32>(local.t);
        return true;
    };
    spatial::BVHLeaf leaf{};
    f32 t = 0.f;
    m_top.ray_cast_exact(origin, direction, ray.tMax, instanceHit, leaf, t);
    return best;
}

RtRefClassified RtReferenceScene::traceClassified(const RtProbeRay& ray, u32 cullMask, f64 edgeEpsilon) const {
    RtRefClassified out{};
    out.hit = trace(ray, cullMask, 0.0);
    const RtRefHit grown = trace(ray, cullMask, edgeEpsilon);
    const RtRefHit shrunk = trace(ray, cullMask, -edgeEpsilon);
    auto same = [](const RtRefHit& a, const RtRefHit& b) {
        return a.hit == b.hit && (!a.hit || (a.instance == b.instance && a.primitive == b.primitive));
    };
    out.robust = same(out.hit, grown) && same(out.hit, shrunk);
    return out;
}

} // namespace fuse::renderer::rt
