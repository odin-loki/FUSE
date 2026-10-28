#include <fuse/fx/missile_descriptor.hpp>

#include <fuse/fx/residual_effects.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::fx {

namespace {

using fuse::math::Vec3;

constexpr float kLenSqEpsilon = 1e-12f;
constexpr float kAxisEpsilon = 1e-8f;
constexpr float kTSlop = 1e-5f;
constexpr float kUnitLenSqEpsilon = 1e-16f;

bool finiteVec(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Vec3 safeUnit(const Vec3& value) {
    const float lenSq = value.dot(value);
    if (!(lenSq > kUnitLenSqEpsilon) || !std::isfinite(lenSq)) {
        return Vec3{0.f, 1.f, 0.f};
    }
    const Vec3 normal = value * (1.f / std::sqrt(lenSq));
    if (!finiteVec(normal)) {
        return Vec3{0.f, 1.f, 0.f};
    }
    return normal;
}

Vec3 axisNormal(int axis, float sign) {
    Vec3 normal;
    if (axis == 0) {
        normal.x = sign;
    } else if (axis == 1) {
        normal.y = sign;
    } else {
        normal.z = sign;
    }
    return normal;
}

Vec3 nearestAabbNormal(const Vec3& point, const Vec3& boxMin, const Vec3& boxMax) {
    const float distances[6] = {
        point.x - boxMin.x,
        boxMax.x - point.x,
        point.y - boxMin.y,
        boxMax.y - point.y,
        point.z - boxMin.z,
        boxMax.z - point.z,
    };
    int best = 0;
    float bestDistance = distances[0];
    for (int face = 1; face < 6; ++face) {
        if (distances[face] < bestDistance) {
            bestDistance = distances[face];
            best = face;
        }
    }
    if (!std::isfinite(bestDistance)) {
        return Vec3{0.f, 1.f, 0.f};
    }
    constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
    constexpr float kSign[6] = {-1.f, 1.f, -1.f, 1.f, -1.f, 1.f};
    return axisNormal(kAxis[best], kSign[best]);
}

struct SweepHit {
    float t = 0.f;
    Vec3 point{};
    Vec3 normal{};
};

bool acceptHit(const SweepHit& hit) {
    return std::isfinite(hit.t) && hit.t >= 0.f && hit.t <= 1.f && finiteVec(hit.point) && finiteVec(hit.normal);
}

/// Point sample. Used for zero-length steps so the segment length is never a divisor.
bool sweepSpherePoint(const Vec3& point, const Vec3& center, float radius, SweepHit& hit) {
    const Vec3 offset = point - center;
    const float distSq = offset.dot(offset);
    const float radiusSq = radius * radius;
    if (!std::isfinite(distSq) || !std::isfinite(radiusSq) || distSq > radiusSq) {
        return false;
    }
    hit.t = 0.f;
    hit.point = point;
    hit.normal = safeUnit(offset);
    return acceptHit(hit);
}

bool sweepSphere(const Vec3& start, const Vec3& delta, const Vec3& center, float radius, SweepHit& hit) {
    if (!finiteVec(start) || !finiteVec(delta) || !finiteVec(center) || !(radius >= 0.f) || !std::isfinite(radius)) {
        return false;
    }

    const float a = delta.dot(delta);
    if (!(a > kLenSqEpsilon)) {
        return sweepSpherePoint(start, center, radius, hit);
    }

    // |start + t delta - center|^2 = radius^2  →  a t^2 + 2 b t + c = 0, b = oc · delta.
    const Vec3 oc = start - center;
    const float b = oc.dot(delta);
    const float c = oc.dot(oc) - radius * radius;
    float disc = b * b - a * c;
    if (!std::isfinite(disc)) {
        return false;
    }
    if (disc < 0.f) {
        const float scale = std::max(b * b, std::fabs(a * c));
        const float tolerance = 1e-6f * std::max(scale, 1.f);
        if (disc < -tolerance) {
            return false;
        }
        disc = 0.f;
    }

    const float sqrtDisc = std::sqrt(disc);
    const float invA = 1.f / a;
    const float t0 = (-b - sqrtDisc) * invA;
    const float t1 = (-b + sqrtDisc) * invA;
    if (!std::isfinite(t0) || !std::isfinite(t1)) {
        return false;
    }
    if (t1 < -kTSlop || t0 > 1.f + kTSlop) {
        return false;
    }

    if (t0 < 0.f) {
        hit.t = 0.f;
        hit.point = start;
        hit.normal = safeUnit(start - center);
    } else {
        hit.t = (t0 > 1.f) ? 1.f : t0;
        hit.point = start + delta * hit.t;
        hit.normal = safeUnit(hit.point - center);
    }
    return acceptHit(hit);
}

bool sweepAabb(const Vec3& start, const Vec3& delta, const Vec3& boxMin, const Vec3& boxMax, SweepHit& hit) {
    if (!finiteVec(start) || !finiteVec(delta) || !finiteVec(boxMin) || !finiteVec(boxMax)) {
        return false;
    }

    const float origin[3] = {start.x, start.y, start.z};
    const float deltaAxis[3] = {delta.x, delta.y, delta.z};
    const float minCorner[3] = {boxMin.x, boxMin.y, boxMin.z};
    const float maxCorner[3] = {boxMax.x, boxMax.y, boxMax.z};

    float tEnter = 0.f;
    float tExit = 1.f;
    int enterAxis = -1;
    float enterSign = 1.f;

    for (int axis = 0; axis < 3; ++axis) {
        if (minCorner[axis] > maxCorner[axis]) {
            return false;
        }

        if (!(std::fabs(deltaAxis[axis]) > kAxisEpsilon)) {
            if (origin[axis] < minCorner[axis] || origin[axis] > maxCorner[axis]) {
                return false;
            }
            continue;
        }

        const float inv = 1.f / deltaAxis[axis];
        float tNear = (minCorner[axis] - origin[axis]) * inv;
        float tFar = (maxCorner[axis] - origin[axis]) * inv;
        float outward = -1.f;
        if (tNear > tFar) {
            const float swapped = tNear;
            tNear = tFar;
            tFar = swapped;
            outward = 1.f;
        }
        if (!std::isfinite(tNear) || !std::isfinite(tFar)) {
            return false;
        }

        // Strict > keeps the lower axis when two faces share an entry time.
        if (tNear > tEnter) {
            tEnter = tNear;
            enterAxis = axis;
            enterSign = outward;
        }
        if (tFar < tExit) {
            tExit = tFar;
        }
        if (tEnter > tExit + kTSlop) {
            return false;
        }
    }

    if (!std::isfinite(tEnter) || !std::isfinite(tExit) || tExit < -kTSlop || tEnter > 1.f + kTSlop) {
        return false;
    }

    float hitT = tEnter;
    if (hitT < 0.f) {
        hitT = 0.f;
    } else if (hitT > 1.f) {
        hitT = 1.f;
    }

    hit.t = hitT;
    hit.point = start + delta * hitT;
    if (enterAxis < 0 || tEnter < 0.f) {
        hit.normal = nearestAabbNormal(hit.point, boxMin, boxMax);
    } else {
        hit.normal = axisNormal(enterAxis, enterSign);
    }
    return acceptHit(hit);
}

bool sweepCollider(const MissileCollider& collider, const Vec3& start, const Vec3& delta, SweepHit& hit) {
    if (collider.shape == MissileColliderShape::Sphere) {
        return sweepSphere(start, delta, collider.center, collider.radius, hit);
    }
    if (collider.shape == MissileColliderShape::Aabb) {
        return sweepAabb(start, delta, collider.aabbMin, collider.aabbMax, hit);
    }
    return false;
}

bool findEarliestHit(const Vec3& start,
                     const Vec3& delta,
                     std::span<const MissileCollider> colliders,
                     u32& colliderIndex,
                     SweepHit& hit) {
    bool found = false;
    for (std::size_t index = 0; index < colliders.size(); ++index) {
        SweepHit candidate;
        if (!sweepCollider(colliders[index], start, delta, candidate)) {
            continue;
        }
        // Equal t keeps the earlier index.
        if (!found || candidate.t < hit.t) {
            found = true;
            hit = candidate;
            colliderIndex = static_cast<u32>(index);
        }
    }
    return found;
}

void enqueueImpactResidual(ResidualEffectQueue& residuals, const MissileInstance& instance) {
    ResidualEntry residue;
    residue.kind = ResidualKind::Zodiac;
    residue.assetId = instance.descriptor.id;
    residue.duration = instance.descriptor.fadeDelay;
    residue.fadeDuration = instance.descriptor.fadeDelay;
    residue.x = instance.impactPoint.x;
    residue.y = instance.impactPoint.y;
    residue.z = instance.impactPoint.z;
    residuals.enqueue(residue);
}

} // namespace

MissileDescriptor MissileDescriptor::makeFireballMissile() {
    MissileDescriptor desc;
    desc.id = "fireball_missile";
    desc.muzzleVelocity = 25.f;
    desc.isBallistic = true;
    desc.gravityMod = 9.81f;
    desc.lifetime = 3.f;
    desc.fadeDelay = 0.25f;
    return desc;
}

void MissilePipeline::fire(const MissileDescriptor& descriptor,
                           const fuse::math::Vec3& origin,
                           const fuse::math::Vec3& direction) {
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                direction.z * direction.z);
    if (len < 1e-6f) {
        return;
    }

    MissileInstance instance;
    instance.descriptor = descriptor;
    instance.position = origin;
    instance.velocity.x = (direction.x / len) * descriptor.muzzleVelocity;
    instance.velocity.y = (direction.y / len) * descriptor.muzzleVelocity;
    instance.velocity.z = (direction.z / len) * descriptor.muzzleVelocity;
    m_instances.push_back(instance);
}

void MissilePipeline::tick(float dt, std::span<const MissileCollider> colliders, ResidualEffectQueue* residuals) {
    if (dt <= 0.f) {
        return;
    }

    for (MissileInstance& instance : m_instances) {
        if (instance.finished) {
            continue;
        }

        instance.elapsed += dt;
        const Vec3 start = instance.position;
        const Vec3 delta{instance.velocity.x * dt, instance.velocity.y * dt, instance.velocity.z * dt};
        const Vec3 end = start + delta;

        SweepHit hit;
        u32 hitIndex = 0;
        if (findEarliestHit(start, delta, colliders, hitIndex, hit)) {
            instance.position = hit.point;
            instance.impacted = true;
            instance.impactColliderIndex = hitIndex;
            instance.impactPoint = hit.point;
            instance.impactNormal = hit.normal;
            instance.finished = true;
            ++m_completedCount;
            if (residuals != nullptr) {
                enqueueImpactResidual(*residuals, instance);
            }
            continue;
        }

        instance.position = end;
        if (instance.descriptor.isBallistic) {
            instance.velocity.z -= instance.descriptor.gravityMod * dt;
        }
        if (instance.elapsed >= instance.descriptor.lifetime) {
            instance.finished = true;
            ++m_completedCount;
        }
    }
}

u32 MissilePipeline::activeCount() const {
    u32 count = 0;
    for (const MissileInstance& instance : m_instances) {
        if (!instance.finished) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::fx
