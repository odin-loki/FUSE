#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {

/// Rigid-body rotation helpers: quaternion algebra, oriented shape bounds and the diagonal
/// body-frame inverse inertia used by the XPBD solver (orientation-aware generalized masses).

FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE bool isIdentity(const quat& q) {
    return q.x == 0.f && q.y == 0.f && q.z == 0.f;
}

FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE quat quatMul(const quat& a, const quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE quat quatConjugate(const quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE quat quatNormalize(const quat& q) {
    const f32 lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq < 1e-20f) {
        return {};
    }
    const f32 inv = 1.f / std::sqrt(lenSq);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

/// Rotation of `angle` radians about the unit `axis`.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE quat quatFromAxisAngle(vec3 axis, f32 angle) {
    const vec3 n = axis.normalized();
    const f32 s = std::sin(0.5f * angle);
    return {n.x * s, n.y * s, n.z * s, std::cos(0.5f * angle)};
}

/// v' = q v q*.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 rotate(const quat& q, vec3 v) {
    if (isIdentity(q)) {
        return v;
    }
    const vec3 u{q.x, q.y, q.z};
    const vec3 t = u.cross(v) * 2.f;
    return v + t * q.w + u.cross(t);
}

/// v' = q* v q (world -> body frame).
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 inverseRotate(const quat& q, vec3 v) {
    return rotate(quatConjugate(q), v);
}

/// Integrates q by the rotation vector `theta` (angular velocity * dt): q + 0.5 [theta, 0] q.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE quat integrateRotation(const quat& q, vec3 theta) {
    const quat spin = quatMul({theta.x, theta.y, theta.z, 0.f}, q);
    return quatNormalize({q.x + 0.5f * spin.x, q.y + 0.5f * spin.y, q.z + 0.5f * spin.z, q.w + 0.5f * spin.w});
}

/// Rotates q by the rotation vector `theta` exactly (exponential map): exp(theta / 2) q.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE quat applyRotationVector(const quat& q, vec3 theta) {
    const f32 angle = theta.length();
    if (angle < 1e-12f) {
        return q;
    }
    const f32 s = std::sin(0.5f * angle) / angle;
    return quatNormalize(quatMul({theta.x * s, theta.y * s, theta.z * s, std::cos(0.5f * angle)}, q));
}

/// Angular velocity carrying `from` to `to` over `dt` along the shortest arc (logarithm map, the
/// exact inverse of `applyRotationVector`).
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 angularVelocityBetween(const quat& from, const quat& to, f32 dt) {
    quat delta = quatMul(to, quatConjugate(from));
    if (delta.w < 0.f) {
        delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    }
    const f32 sinHalf = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (sinHalf < 1e-12f) {
        return {};
    }
    const f32 angle = 2.f * std::atan2(sinHalf, delta.w);
    const f32 scale = angle / (sinHalf * dt);
    return {delta.x * scale, delta.y * scale, delta.z * scale};
}

/// World-space inverse inertia applied to `v`: R diag(invInertiaLocal) R^T v.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 applyInverseInertia(const quat& q, vec3 invInertiaLocal, vec3 v) {
    if (isIdentity(q)) {
        return {v.x * invInertiaLocal.x, v.y * invInertiaLocal.y, v.z * invInertiaLocal.z};
    }
    const vec3 local = inverseRotate(q, v);
    return rotate(q, {local.x * invInertiaLocal.x, local.y * invInertiaLocal.y, local.z * invInertiaLocal.z});
}

/// Generalized inverse mass of a unit direction `n` applied at arm `r`: 1/m + (r x n)^T I^-1 (r x n).
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE f32 generalizedInverseMass(f32 invMass, const quat& q, vec3 invInertiaLocal, vec3 r, vec3 n) {
    if (invInertiaLocal.x == 0.f && invInertiaLocal.y == 0.f && invInertiaLocal.z == 0.f) {
        return invMass;
    }
    const vec3 rn = r.cross(n);
    return invMass + rn.dot(applyInverseInertia(q, invInertiaLocal, rn));
}

/// World half extents of the AABB enclosing an oriented box.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 orientedBoxHalfExtents(const quat& q, vec3 halfExtents) {
    if (isIdentity(q)) {
        return halfExtents;
    }
    const vec3 ax = rotate(q, {halfExtents.x, 0.f, 0.f});
    const vec3 ay = rotate(q, {0.f, halfExtents.y, 0.f});
    const vec3 az = rotate(q, {0.f, 0.f, halfExtents.z});
    return {std::fabs(ax.x) + std::fabs(ay.x) + std::fabs(az.x), std::fabs(ax.y) + std::fabs(ay.y) + std::fabs(az.y),
            std::fabs(ax.z) + std::fabs(ay.z) + std::fabs(az.z)};
}

/// World half extents of the AABB enclosing a capsule (local Y axis, radius params.x, half height params.y).
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 orientedCapsuleHalfExtents(const quat& q, vec3 params) {
    const vec3 axis = rotate(q, {0.f, params.y, 0.f});
    return {std::fabs(axis.x) + params.x, std::fabs(axis.y) + params.x, std::fabs(axis.z) + params.x};
}

/// Capsule segment half-axis in world space (centre +- this vector are the cap centres).
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 capsuleHalfAxis(const quat& q, f32 halfHeight) {
    return rotate(q, {0.f, halfHeight, 0.f});
}

/// One implicit (backward Euler, single Newton step) update of the torque-free gyroscopic term
/// w x (I w) for a body with diagonal body-frame inertia. Unlike the explicit form it is
/// dissipative rather than energy-pumping, so free tumbling stays bounded. `invInertiaLocal` must be
/// non-zero.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 implicitGyroscopicStep(const quat& q, vec3 invInertiaLocal, vec3 omega, f32 dt) {
    const vec3 inertia{1.f / invInertiaLocal.x, 1.f / invInertiaLocal.y, 1.f / invInertiaLocal.z};
    const f32 spread = std::max(inertia.x, std::max(inertia.y, inertia.z)) -
                       std::min(inertia.x, std::min(inertia.y, inertia.z));
    if (spread <= 1e-6f * inertia.x) {
        return omega; // isotropic: w x (I w) = 0
    }
    const vec3 w = inverseRotate(q, omega);
    const vec3 iw{inertia.x * w.x, inertia.y * w.y, inertia.z * w.z};
    const vec3 f = w.cross(iw) * dt;
    // Jacobian J = I + dt (skew(w) I - skew(I w)); column j = I_j e_j + dt (I_j (w x e_j) - (I w) x e_j).
    const vec3 e[3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    const f32 moments[3] = {inertia.x, inertia.y, inertia.z};
    vec3 col[3]{};
    for (int j = 0; j < 3; ++j) {
        col[j] = e[j] * moments[j] + (w.cross(e[j]) * moments[j] - iw.cross(e[j])) * dt;
    }
    const f32 det = col[0].dot(col[1].cross(col[2]));
    if (std::fabs(det) < 1e-20f) {
        return omega;
    }
    // Cramer's rule: x_j = det(J with column j replaced by f) / det.
    const vec3 x{f.dot(col[1].cross(col[2])) / det, col[0].dot(f.cross(col[2])) / det,
                 col[0].dot(col[1].cross(f)) / det};
    return rotate(q, w - x);
}

/// Diagonal body-frame inverse inertia of a solid shape with inverse mass `invMass`
/// (sphere radius params.x; box half extents params; capsule radius params.x, half height params.y
/// along local Y). Zero for massless bodies and for planes.
FUSE_HOST_DEVICE FUSE_PHYSICS_INLINE vec3 shapeInverseInertia(CollisionShapeType type, vec3 params, f32 invMass) {
    if (invMass <= 0.f) {
        return {};
    }
    const f32 mass = 1.f / invMass;
    const auto invert = [](f32 moment) { return moment > 1e-12f ? 1.f / moment : 0.f; };
    switch (type) {
    case CollisionShapeType::Box: {
        const f32 x2 = params.x * params.x;
        const f32 y2 = params.y * params.y;
        const f32 z2 = params.z * params.z;
        return {invert(mass * (y2 + z2) / 3.f), invert(mass * (x2 + z2) / 3.f), invert(mass * (x2 + y2) / 3.f)};
    }
    case CollisionShapeType::Capsule: {
        const f32 r = params.x;
        const f32 h = 2.f * params.y; // cylinder length
        const f32 cylinderVolume = 3.14159265f * r * r * h;
        const f32 capsVolume = 4.f / 3.f * 3.14159265f * r * r * r;
        const f32 total = cylinderVolume + capsVolume;
        if (total <= 1e-12f) {
            return {};
        }
        const f32 mc = mass * cylinderVolume / total;
        const f32 ms = mass * capsVolume / total; // both hemispheres
        const f32 axial = mc * r * r * 0.5f + ms * r * r * 0.4f;
        const f32 transverse = mc * (h * h / 12.f + r * r * 0.25f) + ms * (0.4f * r * r + h * h * 0.25f + 0.375f * h * r);
        return {invert(transverse), invert(axial), invert(transverse)};
    }
    case CollisionShapeType::Plane:
        return {};
    default: {
        const f32 moment = 0.4f * mass * params.x * params.x;
        const f32 inv = invert(moment);
        return {inv, inv, inv};
    }
    }
}

} // namespace fuse::physics
