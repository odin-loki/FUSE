// Narrowphase dispatch for pairs with a pooled shape (convex hull, triangle mesh, SDF, voxel volume).
// CPU only: the resident CUDA narrowphase refuses scenes with pooled shapes (ResidentPhysics::uploadScene),
// so these pairs always run here, while primitive pairs keep the shared FUSE_HOST_DEVICE dispatch.

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/field_contacts.hpp>
#include <fuse/physics/narrowphase/hull_contacts.hpp>
#include <fuse/physics/narrowphase/mesh_contacts.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>

namespace fuse::physics::narrowphase {

namespace {

using T = CollisionShapeType;

/// Hull view of a shape instance: a pooled hull, or a box as an allocation-free BoxHull.
bool hullPose(const ShapeInstance& shape, BoxHull& boxStorage, HullPose& out) {
    out.position = shape.position;
    out.orientation = shape.orientation;
    if (shape.type == T::Box) {
        initBoxHull(boxStorage, shape.params);
        out.hull = boxStorage.view();
        return true;
    }
    if (shape.type == T::ConvexHull) {
        const ConvexHull* hull = ShapePool::global().hull(shape.shapeRef);
        if (hull == nullptr || hull->empty()) {
            return false;
        }
        out.hull = hull->view();
        return true;
    }
    return false;
}

MeshPose meshPose(const ShapeInstance& shape) {
    MeshPose pose{};
    pose.mesh = ShapePool::global().mesh(shape.shapeRef);
    pose.position = shape.position;
    pose.orientation = shape.orientation;
    return pose;
}

FieldPose fieldPose(const ShapeInstance& shape) {
    FieldPose pose{};
    if (shape.type == T::Voxel) {
        pose.voxel = ShapePool::global().voxel(shape.shapeRef);
    } else {
        pose.sdf = ShapePool::global().sdf(shape.shapeRef);
    }
    pose.position = shape.position;
    pose.orientation = shape.orientation;
    return pose;
}

ContactManifold flip(ContactManifold manifold, u32 a, u32 b) {
    return hd::flipped(manifold, a, b);
}

/// The convex part: pairs with a hull (and a primitive or another hull), A first.
ContactManifold collideConvexHullPair(const ShapeInstance& a, const ShapeInstance& b, u32 idxA, u32 idxB, f32 margin) {
    BoxHull boxA;
    BoxHull boxB;
    if (a.type == T::ConvexHull) {
        HullPose hullA{};
        if (!hullPose(a, boxA, hullA)) {
            return invalidContactManifold();
        }
        switch (b.type) {
        case T::Sphere:
            return flip(collideSphereHull(b.position, b.params.x, hullA, idxB, idxA, margin), idxA, idxB);
        case T::Capsule:
            return flip(collideCapsuleHull(b.position, b.orientation, b.params, hullA, idxB, idxA, margin), idxA, idxB);
        case T::Plane:
            return collideHullPlane(hullA, b.params, b.scalar, idxA, idxB, margin);
        case T::Box:
        case T::ConvexHull: {
            HullPose hullB{};
            if (!hullPose(b, boxB, hullB)) {
                return invalidContactManifold();
            }
            return collideHullHull(hullA, hullB, idxA, idxB, margin);
        }
        default:
            return invalidContactManifold();
        }
    }
    // b is the hull.
    return flip(collideConvexHullPair(b, a, idxB, idxA, margin), idxA, idxB);
}

u32 collideConcave(const ShapeInstance& a, const ShapeInstance& b, u32 idxA, u32 idxB, f32 margin,
                   ContactManifold* out, u32 maxOut) {
    // Concave shape as B (the convex shape moves against it); flip the results back when it was A.
    if (isConcaveShape(a.type)) {
        const u32 count = collideConcave(b, a, idxB, idxA, margin, out, maxOut);
        for (u32 i = 0; i < count; ++i) {
            out[i] = flip(out[i], idxA, idxB);
        }
        return count;
    }
    if (b.type == T::TriMesh) {
        const MeshPose mesh = meshPose(b);
        if (mesh.mesh == nullptr) {
            return 0u;
        }
        if (a.type == T::ConvexHull) {
            BoxHull box;
            HullPose hull{};
            if (!hullPose(a, box, hull)) {
                return 0u;
            }
            return collideHullMesh(hull, mesh, idxA, idxB, margin, out, maxOut);
        }
        return collideConvexMesh(a, mesh, idxA, idxB, margin, out, maxOut);
    }
    const FieldPose field = fieldPose(b);
    if (field.voxel == nullptr && field.sdf == nullptr) {
        return 0u;
    }
    return collideConvexField(a, field, idxA, idxB, margin, out, maxOut);
}

} // namespace

bool isConcaveShape(CollisionShapeType type) {
    return type == T::TriMesh || type == T::Voxel || type == T::SdfMesh;
}

u32 collideShapesMulti(const ShapeInstance& a, const ShapeInstance& b, u32 idxA, u32 idxB, f32 margin,
                       ContactManifold* out, u32 maxOut) {
    if (out == nullptr || maxOut == 0u) {
        return 0u;
    }
    if (!isConcaveShape(a.type) && !isConcaveShape(b.type)) {
        out[0] = collideShapes(a, b, idxA, idxB, margin);
        return out[0].valid ? 1u : 0u;
    }
    if (isConcaveShape(a.type) && isConcaveShape(b.type)) {
        return 0u; // static level geometry never collides with itself
    }
    const T other = isConcaveShape(a.type) ? b.type : a.type;
    if (other == T::Plane || (other == T::ConvexHull && (a.type != T::TriMesh && b.type != T::TriMesh))) {
        return 0u; // no hull-vs-field / plane-vs-concave narrowphase
    }
    return collideConcave(a, b, idxA, idxB, margin, out, maxOut);
}

ContactManifold collidePooledShapes(const ShapeInstance& a, const ShapeInstance& b, u32 idxA, u32 idxB, f32 margin) {
    if (isConcaveShape(a.type) || isConcaveShape(b.type)) {
        ContactManifold clusters[kMaxManifoldsPerPair];
        const u32 count = collideShapesMulti(a, b, idxA, idxB, margin, clusters, kMaxManifoldsPerPair);
        return count > 0u ? clusters[0] : invalidContactManifold();
    }
    if (a.type == T::ConvexHull || b.type == T::ConvexHull) {
        return collideConvexHullPair(a, b, idxA, idxB, margin);
    }
    return invalidContactManifold();
}

} // namespace fuse::physics::narrowphase
