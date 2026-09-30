#include <fuse/physics/narrowphase/field_contacts.hpp>

#include <fuse/physics/narrowphase/contact_cluster.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/shapes/sdf_sampler.hpp>
#include <fuse/physics/spatial/svo.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::physics::narrowphase {

namespace {

struct FieldScratch {
    ContactClusterer clusterer;
    std::vector<u32> region;
};

FieldScratch& scratch() {
    thread_local FieldScratch s;
    return s;
}

/// Deepest point of the convex shape along -n (its support in direction -n).
vec3 supportAgainst(const ShapeInstance& shape, vec3 n) {
    switch (shape.type) {
    case CollisionShapeType::Sphere:
        return shape.position - n * shape.params.x;
    case CollisionShapeType::Capsule: {
        const vec3 half = capsuleHalfAxis(shape.orientation, std::max(shape.params.y, 0.f));
        const vec3 end = half.dot(n) > 0.f ? shape.position - half : shape.position + half;
        return end - n * shape.params.x;
    }
    case CollisionShapeType::Box: {
        const vec3 local = inverseRotate(shape.orientation, n);
        const vec3 corner{local.x > 0.f ? -shape.params.x : shape.params.x, local.y > 0.f ? -shape.params.y : shape.params.y,
                          local.z > 0.f ? -shape.params.z : shape.params.z};
        return shape.position + rotate(shape.orientation, corner);
    }
    default:
        return shape.position;
    }
}

void localBounds(const ShapeInstance& shape, vec3& lo, vec3& hi) {
    vec3 half{};
    switch (shape.type) {
    case CollisionShapeType::Sphere:
        half = {shape.params.x, shape.params.x, shape.params.x};
        break;
    case CollisionShapeType::Capsule:
        half = orientedCapsuleHalfExtents(shape.orientation, shape.params);
        break;
    case CollisionShapeType::Box:
        half = orientedBoxHalfExtents(shape.orientation, shape.params);
        break;
    default:
        break;
    }
    lo = shape.position - half;
    hi = shape.position + half;
}

// --- Voxels ------------------------------------------------------------------------------------------

void voxelContacts(const ShapeInstance& shape, const VoxelVolume& volume, f32 margin, FieldScratch& s) {
    vec3 lo;
    vec3 hi;
    localBounds(shape, lo, hi);
    const vec3 pad{margin, margin, margin};
    const ivec3 dims = volume.dims();
    const ivec3 a = volume.voxelAt(lo - pad);
    const ivec3 b = volume.voxelAt(hi + pad);
    const ivec3 v0{std::max(a.x, 0), std::max(a.y, 0), std::max(a.z, 0)};
    const ivec3 v1{std::min(b.x, dims.x - 1), std::min(b.y, dims.y - 1), std::min(b.z, dims.z - 1)};
    if (v0.x > v1.x || v0.y > v1.y || v0.z > v1.z) {
        return;
    }
    // Region with a one-voxel apron for the neighbour tests (the SVO reads out-of-box voxels as empty).
    const ivec3 r0{v0.x - 1, v0.y - 1, v0.z - 1};
    const ivec3 size{v1.x - v0.x + 3, v1.y - v0.y + 3, v1.z - v0.z + 3};
    volume.svo().readBox(scene::ivec3(r0.x, r0.y, r0.z), scene::ivec3(size.x, size.y, size.z), s.region);
    const auto solid = [&](s32 x, s32 y, s32 z) {
        const s32 lx = x - r0.x;
        const s32 ly = y - r0.y;
        const s32 lz = z - r0.z;
        if (lx < 0 || ly < 0 || lz < 0 || lx >= size.x || ly >= size.y || lz >= size.z || x < 0 || y < 0 || z < 0 ||
            x >= dims.x || y >= dims.y || z >= dims.z) {
            return false;
        }
        return s.region[(static_cast<usize>(lz) * static_cast<usize>(size.y) + static_cast<usize>(ly)) *
                            static_cast<usize>(size.x) +
                        static_cast<usize>(lx)] != 0u;
    };
    const f32 h = volume.voxelSize() * 0.5f;
    const vec3 half{h, h, h};
    const quat identity{};

    for (s32 z = v0.z; z <= v1.z; ++z) {
        for (s32 y = v0.y; y <= v1.y; ++y) {
            for (s32 x = v0.x; x <= v1.x; ++x) {
                if (!solid(x, y, z)) {
                    continue;
                }
                const bool exposed = !solid(x + 1, y, z) || !solid(x - 1, y, z) || !solid(x, y + 1, z) ||
                                     !solid(x, y - 1, z) || !solid(x, y, z + 1) || !solid(x, y, z - 1);
                if (!exposed) {
                    continue; // interior voxel: never a surface feature
                }
                const vec3 centre = volume.voxelCenter({x, y, z});
                ContactManifold m{};
                switch (shape.type) {
                case CollisionShapeType::Sphere:
                    m = collideBoxSphere(shape.position, shape.params.x, centre, half, 0u, 1u, margin);
                    break;
                case CollisionShapeType::Capsule:
                    m = hd::collideCapsuleBox(shape.position, shape.orientation, shape.params, centre, identity, half,
                                              0u, 1u, margin);
                    break;
                case CollisionShapeType::Box:
                    m = hd::collideOrientedBoxBox(shape.position, shape.orientation, shape.params, centre, identity,
                                                  half, 0u, 1u, margin);
                    break;
                default:
                    break;
                }
                if (!m.valid || m.pointCount == 0u) {
                    continue;
                }
                vec3 n = m.contactNormal;
                // Internal face: the neighbour across the dominant axis of the normal is solid.
                const f32 ax = std::fabs(n.x);
                const f32 ay = std::fabs(n.y);
                const f32 az = std::fabs(n.z);
                s32 nx = x;
                s32 ny = y;
                s32 nz = z;
                if (ax >= ay && ax >= az) {
                    nx += n.x > 0.f ? 1 : -1;
                } else if (ay >= az) {
                    ny += n.y > 0.f ? 1 : -1;
                } else {
                    nz += n.z > 0.f ? 1 : -1;
                }
                if (!solid(nx, ny, nz)) {
                    for (u32 i = 0; i < m.pointCount; ++i) {
                        s.clusterer.add(m.points[i].point, n, m.points[i].penetration);
                    }
                    continue;
                }
                // Occupancy-gradient normal of this voxel (solid neighbours push it away).
                vec3 g{};
                for (s32 dz = -1; dz <= 1; ++dz) {
                    for (s32 dy = -1; dy <= 1; ++dy) {
                        for (s32 dx = -1; dx <= 1; ++dx) {
                            if ((dx | dy | dz) != 0 && solid(x + dx, y + dy, z + dz)) {
                                const f32 w = 1.f / static_cast<f32>(dx * dx + dy * dy + dz * dz);
                                g -= vec3{static_cast<f32>(dx), static_cast<f32>(dy), static_cast<f32>(dz)} * w;
                            }
                        }
                    }
                }
                const f32 len = g.length();
                if (len < 1e-4f) {
                    continue;
                }
                n = g * (1.f / len);
                const vec3 deep = supportAgainst(shape, n);
                const f32 reach = n.dot(centre) + h * (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z));
                const f32 depth = reach - n.dot(deep);
                if (depth < -margin) {
                    continue;
                }
                const vec3 point = shape.type == CollisionShapeType::Sphere ? deep : deep + n * (0.5f * depth);
                s.clusterer.add(point, n, depth);
            }
        }
    }
}

// --- Signed distance fields ----------------------------------------------------------------------------

void sdfSample(const SdfSampler& sdf, vec3 p, f32 radius, f32 margin, bool pointOnSurface, FieldScratch& s) {
    const f32 d = sdf.distance(p);
    if (d > radius + margin) {
        return;
    }
    const vec3 g = sdf.gradient(p);
    const f32 len = g.length();
    if (len < 1e-12f) {
        return;
    }
    const vec3 n = g * (1.f / len);
    const f32 depth = radius - d;
    // Rounded shapes: the point on their surface; box samples: midway between the surfaces.
    const vec3 point = pointOnSurface ? p - n * radius : p + n * (0.5f * depth);
    s.clusterer.add(point, n, depth);
}

void sdfContacts(const ShapeInstance& shape, const SdfSampler& sdf, f32 margin, FieldScratch& s) {
    vec3 lo;
    vec3 hi;
    localBounds(shape, lo, hi);
    const vec3 bmin = sdf.boundsMin();
    const vec3 bmax = sdf.boundsMax();
    if (lo.x > bmax.x + margin || hi.x < bmin.x - margin || lo.y > bmax.y + margin || hi.y < bmin.y - margin ||
        lo.z > bmax.z + margin || hi.z < bmin.z - margin) {
        return;
    }
    switch (shape.type) {
    case CollisionShapeType::Sphere:
        sdfSample(sdf, shape.position, shape.params.x, margin, true, s);
        break;
    case CollisionShapeType::Capsule: {
        const vec3 half = capsuleHalfAxis(shape.orientation, std::max(shape.params.y, 0.f));
        const vec3 p0 = shape.position - half;
        const vec3 p1 = shape.position + half;
        // Deepest axis point: coarse samples, then a golden-section refinement around the best one.
        constexpr u32 kSamples = 9u;
        u32 best = 0;
        f32 bestD = sdf.distance(p0);
        for (u32 i = 1; i < kSamples; ++i) {
            const f32 d = sdf.distance(p0 + (p1 - p0) * (static_cast<f32>(i) / static_cast<f32>(kSamples - 1u)));
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        f32 lo01 = static_cast<f32>(best > 0u ? best - 1u : 0u) / static_cast<f32>(kSamples - 1u);
        f32 hi01 = static_cast<f32>(std::min(best + 1u, kSamples - 1u)) / static_cast<f32>(kSamples - 1u);
        for (int it = 0; it < 20; ++it) {
            const f32 m1 = lo01 + (hi01 - lo01) * 0.381966f;
            const f32 m2 = lo01 + (hi01 - lo01) * 0.618034f;
            if (sdf.distance(p0 + (p1 - p0) * m1) <= sdf.distance(p0 + (p1 - p0) * m2)) {
                hi01 = m2;
            } else {
                lo01 = m1;
            }
        }
        const f32 t = 0.5f * (lo01 + hi01);
        sdfSample(sdf, p0 + (p1 - p0) * t, shape.params.x, margin, true, s);
        if (t > 1e-3f) {
            sdfSample(sdf, p0, shape.params.x, margin, true, s);
        }
        if (t < 1.f - 1e-3f) {
            sdfSample(sdf, p1, shape.params.x, margin, true, s);
        }
        break;
    }
    case CollisionShapeType::Box: {
        // Corners, edge midpoints and face centres of the box surface.
        for (s32 i = -1; i <= 1; ++i) {
            for (s32 j = -1; j <= 1; ++j) {
                for (s32 k = -1; k <= 1; ++k) {
                    if (i == 0 && j == 0 && k == 0) {
                        continue;
                    }
                    const vec3 local{shape.params.x * static_cast<f32>(i), shape.params.y * static_cast<f32>(j),
                                     shape.params.z * static_cast<f32>(k)};
                    sdfSample(sdf, shape.position + rotate(shape.orientation, local), 0.f, margin, false, s);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

u32 collideConvexField(const ShapeInstance& convex, const FieldPose& field, u32 idxA, u32 idxB, f32 margin,
                       ContactManifold* out, u32 maxOut) {
    if ((field.voxel == nullptr && field.sdf == nullptr) || out == nullptr || maxOut == 0u) {
        return 0u;
    }
    FieldScratch& s = scratch();
    s.clusterer.reset();
    ShapeInstance local = convex;
    local.position = inverseRotate(field.orientation, convex.position - field.position);
    local.orientation = quatMul(quatConjugate(field.orientation), convex.orientation);
    if (field.voxel != nullptr) {
        voxelContacts(local, *field.voxel, margin, s);
    } else {
        sdfContacts(local, *field.sdf, margin, s);
    }
    const u32 count = s.clusterer.build(idxA, idxB, out, maxOut);
    for (u32 m = 0; m < count; ++m) {
        ContactManifold& manifold = out[m];
        manifold.contactNormal = rotate(field.orientation, manifold.contactNormal);
        for (u32 i = 0; i < manifold.pointCount; ++i) {
            manifold.points[i].point = field.position + rotate(field.orientation, manifold.points[i].point);
        }
        manifold.syncLegacyFields();
    }
    return count;
}

} // namespace fuse::physics::narrowphase
