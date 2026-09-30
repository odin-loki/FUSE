// Revolute, distance and prismatic joints: velocity constraints solved with sequential impulses (warm
// started from the previous step's accumulated impulses) plus non-linear position correction.

#include "rigid_world_2d.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::physics::p2d {

void RigidWorld2D::initJointVelocity(Joint2D& j, f32 dt) {
    Body2D& bA = m_bodies[j.bodyA];
    Body2D& bB = m_bodies[j.bodyB];
    j.localCenterA = bA.localCenter;
    j.localCenterB = bB.localCenter;
    j.mA = bA.invMass;
    j.mB = bB.invMass;
    j.iA = bA.invI;
    j.iB = bB.invI;
    const f32 mA = j.mA;
    const f32 mB = j.mB;
    const f32 iA = j.iA;
    const f32 iB = j.iB;
    const Rot qA(bA.a);
    const Rot qB(bB.a);
    j.rA = mul(qA, j.localAnchorA - j.localCenterA);
    j.rB = mul(qB, j.localAnchorB - j.localCenterB);
    const f32 ratio = settings.warmStarting ? m_dtRatio : 0.f;

    switch (j.type) {
    case JointType2D::Revolute: {
        const vec2 rA = j.rA;
        const vec2 rB = j.rB;
        j.k22.ex.x = mA + mB + rA.y * rA.y * iA + rB.y * rB.y * iB;
        j.k22.ey.x = -rA.y * rA.x * iA - rB.y * rB.x * iB;
        j.k22.ex.y = j.k22.ey.x;
        j.k22.ey.y = mA + mB + rA.x * rA.x * iA + rB.x * rB.x * iB;
        j.axialMass = iA + iB;
        const bool fixedRotation = j.axialMass == 0.f;
        j.axialMass = j.axialMass > 0.f ? 1.f / j.axialMass : 0.f;
        j.angle = bB.a - bA.a - j.referenceAngle;
        if (!j.enableLimit || fixedRotation) {
            j.lowerImpulse = 0.f;
            j.upperImpulse = 0.f;
        }
        if (!j.enableMotor || fixedRotation) {
            j.motorImpulse = 0.f;
        }
        j.pointImpulse = ratio * j.pointImpulse;
        j.motorImpulse *= ratio;
        j.lowerImpulse *= ratio;
        j.upperImpulse *= ratio;
        const f32 axialImpulse = j.motorImpulse + j.lowerImpulse - j.upperImpulse;
        const vec2 p = j.pointImpulse;
        bA.v -= mA * p;
        bA.w -= iA * (cross(rA, p) + axialImpulse);
        bB.v += mB * p;
        bB.w += iB * (cross(rB, p) + axialImpulse);
        break;
    }
    case JointType2D::Distance: {
        j.u = bB.c + j.rB - bA.c - j.rA;
        const f32 len = length(j.u);
        if (len > kLinearSlop) {
            j.u = (1.f / len) * j.u;
        } else {
            j.u = {0.f, 0.f};
        }
        const f32 crAu = cross(j.rA, j.u);
        const f32 crBu = cross(j.rB, j.u);
        f32 invMass = mA + iA * crAu * crAu + mB + iB * crBu * crBu;
        j.mass = invMass != 0.f ? 1.f / invMass : 0.f;
        j.gamma = 0.f;
        j.bias = 0.f;
        if (j.frequencyHz > 0.f) {
            const f32 c = len - j.length;
            const f32 omega = 2.f * 3.14159265359f * j.frequencyHz;
            const f32 d = 2.f * j.mass * j.dampingRatio * omega;
            const f32 k = j.mass * omega * omega;
            j.gamma = dt * (d + dt * k);
            j.gamma = j.gamma != 0.f ? 1.f / j.gamma : 0.f;
            j.bias = c * dt * k * j.gamma;
            invMass += j.gamma;
            j.mass = invMass != 0.f ? 1.f / invMass : 0.f;
        }
        j.axialImpulse *= ratio;
        const vec2 p = j.axialImpulse * j.u;
        bA.v -= mA * p;
        bA.w -= iA * cross(j.rA, p);
        bB.v += mB * p;
        bB.w += iB * cross(j.rB, p);
        break;
    }
    case JointType2D::Prismatic: {
        const vec2 d = (bB.c - bA.c) + j.rB - j.rA;
        j.axis = mul(qA, j.localXAxisA);
        j.a1 = cross(d + j.rA, j.axis);
        j.a2 = cross(j.rB, j.axis);
        j.axialMass = mA + mB + iA * j.a1 * j.a1 + iB * j.a2 * j.a2;
        j.axialMass = j.axialMass > 0.f ? 1.f / j.axialMass : 0.f;
        j.perp = mul(qA, j.localYAxisA);
        j.s1 = cross(d + j.rA, j.perp);
        j.s2 = cross(j.rB, j.perp);
        const f32 k11 = mA + mB + iA * j.s1 * j.s1 + iB * j.s2 * j.s2;
        const f32 k12 = iA * j.s1 + iB * j.s2;
        f32 k22 = iA + iB;
        if (k22 == 0.f) {
            k22 = 1.f; // both bodies have fixed rotation
        }
        j.k22.ex = {k11, k12};
        j.k22.ey = {k12, k22};
        if (j.enableLimit) {
            j.translation = dot(j.axis, d);
        } else {
            j.lowerImpulse = 0.f;
            j.upperImpulse = 0.f;
        }
        if (!j.enableMotor) {
            j.motorImpulse = 0.f;
        }
        j.pointImpulse = ratio * j.pointImpulse;
        j.motorImpulse *= ratio;
        j.lowerImpulse *= ratio;
        j.upperImpulse *= ratio;
        const f32 axialImpulse = j.motorImpulse + j.lowerImpulse - j.upperImpulse;
        const vec2 p = j.pointImpulse.x * j.perp + axialImpulse * j.axis;
        const f32 lA = j.pointImpulse.x * j.s1 + j.pointImpulse.y + axialImpulse * j.a1;
        const f32 lB = j.pointImpulse.x * j.s2 + j.pointImpulse.y + axialImpulse * j.a2;
        bA.v -= mA * p;
        bA.w -= iA * lA;
        bB.v += mB * p;
        bB.w += iB * lB;
        break;
    }
    }
}

void RigidWorld2D::solveJointVelocity(Joint2D& j, f32 dt) {
    Body2D& bA = m_bodies[j.bodyA];
    Body2D& bB = m_bodies[j.bodyB];
    vec2 vA = bA.v;
    f32 wA = bA.w;
    vec2 vB = bB.v;
    f32 wB = bB.w;
    const f32 mA = j.mA;
    const f32 mB = j.mB;
    const f32 iA = j.iA;
    const f32 iB = j.iB;
    const f32 invH = dt > 0.f ? 1.f / dt : 0.f;

    switch (j.type) {
    case JointType2D::Revolute: {
        const bool fixedRotation = iA + iB == 0.f;
        if (j.enableMotor && !fixedRotation) {
            const f32 cdot = wB - wA - j.motorSpeed;
            f32 impulse = -j.axialMass * cdot;
            const f32 oldImpulse = j.motorImpulse;
            const f32 maxImpulse = dt * j.maxMotor;
            j.motorImpulse = std::clamp(oldImpulse + impulse, -maxImpulse, maxImpulse);
            impulse = j.motorImpulse - oldImpulse;
            wA -= iA * impulse;
            wB += iB * impulse;
        }
        if (j.enableLimit && !fixedRotation) {
            {
                const f32 c = j.angle - j.lower;
                const f32 cdot = wB - wA;
                f32 impulse = -j.axialMass * (cdot + std::max(c, 0.f) * invH);
                const f32 newImpulse = std::max(j.lowerImpulse + impulse, 0.f);
                impulse = newImpulse - j.lowerImpulse;
                j.lowerImpulse = newImpulse;
                wA -= iA * impulse;
                wB += iB * impulse;
            }
            {
                const f32 c = j.upper - j.angle;
                const f32 cdot = wA - wB;
                f32 impulse = -j.axialMass * (cdot + std::max(c, 0.f) * invH);
                const f32 newImpulse = std::max(j.upperImpulse + impulse, 0.f);
                impulse = newImpulse - j.upperImpulse;
                j.upperImpulse = newImpulse;
                wA += iA * impulse;
                wB -= iB * impulse;
            }
        }
        const vec2 cdot = vB + cross(wB, j.rB) - vA - cross(wA, j.rA);
        const vec2 impulse = j.k22.solve(-cdot);
        j.pointImpulse += impulse;
        vA -= mA * impulse;
        wA -= iA * cross(j.rA, impulse);
        vB += mB * impulse;
        wB += iB * cross(j.rB, impulse);
        j.lastImpulse = length(j.pointImpulse);
        break;
    }
    case JointType2D::Distance: {
        const vec2 vpA = vA + cross(wA, j.rA);
        const vec2 vpB = vB + cross(wB, j.rB);
        const f32 cdot = dot(j.u, vpB - vpA);
        const f32 impulse = -j.mass * (cdot + j.bias + j.gamma * j.axialImpulse);
        j.axialImpulse += impulse;
        const vec2 p = impulse * j.u;
        vA -= mA * p;
        wA -= iA * cross(j.rA, p);
        vB += mB * p;
        wB += iB * cross(j.rB, p);
        j.lastImpulse = std::fabs(j.axialImpulse);
        break;
    }
    case JointType2D::Prismatic: {
        if (j.enableMotor) {
            const f32 cdot = dot(j.axis, vB - vA) + j.a2 * wB - j.a1 * wA;
            f32 impulse = j.axialMass * (j.motorSpeed - cdot);
            const f32 oldImpulse = j.motorImpulse;
            const f32 maxImpulse = dt * j.maxMotor;
            j.motorImpulse = std::clamp(oldImpulse + impulse, -maxImpulse, maxImpulse);
            impulse = j.motorImpulse - oldImpulse;
            const vec2 p = impulse * j.axis;
            vA -= mA * p;
            wA -= iA * impulse * j.a1;
            vB += mB * p;
            wB += iB * impulse * j.a2;
        }
        if (j.enableLimit) {
            {
                const f32 c = j.translation - j.lower;
                const f32 cdot = dot(j.axis, vB - vA) + j.a2 * wB - j.a1 * wA;
                f32 impulse = -j.axialMass * (cdot + std::max(c, 0.f) * invH);
                const f32 newImpulse = std::max(j.lowerImpulse + impulse, 0.f);
                impulse = newImpulse - j.lowerImpulse;
                j.lowerImpulse = newImpulse;
                const vec2 p = impulse * j.axis;
                vA -= mA * p;
                wA -= iA * impulse * j.a1;
                vB += mB * p;
                wB += iB * impulse * j.a2;
            }
            {
                const f32 c = j.upper - j.translation;
                const f32 cdot = dot(j.axis, vA - vB) + j.a1 * wA - j.a2 * wB;
                f32 impulse = -j.axialMass * (cdot + std::max(c, 0.f) * invH);
                const f32 newImpulse = std::max(j.upperImpulse + impulse, 0.f);
                impulse = newImpulse - j.upperImpulse;
                j.upperImpulse = newImpulse;
                const vec2 p = impulse * j.axis;
                vA += mA * p;
                wA += iA * impulse * j.a1;
                vB -= mB * p;
                wB -= iB * impulse * j.a2;
            }
        }
        {
            const vec2 cdot{dot(j.perp, vB - vA) + j.s2 * wB - j.s1 * wA, wB - wA};
            const vec2 df = j.k22.solve(-cdot);
            j.pointImpulse += df;
            const vec2 p = df.x * j.perp;
            const f32 lA = df.x * j.s1 + df.y;
            const f32 lB = df.x * j.s2 + df.y;
            vA -= mA * p;
            wA -= iA * lA;
            vB += mB * p;
            wB += iB * lB;
        }
        j.lastImpulse = length(j.pointImpulse);
        break;
    }
    }
    bA.v = vA;
    bA.w = wA;
    bB.v = vB;
    bB.w = wB;
}

bool RigidWorld2D::solveJointPosition(Joint2D& j) {
    Body2D& bA = m_bodies[j.bodyA];
    Body2D& bB = m_bodies[j.bodyB];
    vec2 cA = bA.c;
    f32 aA = bA.a;
    vec2 cB = bB.c;
    f32 aB = bB.a;
    const f32 mA = j.mA;
    const f32 mB = j.mB;
    const f32 iA = j.iA;
    const f32 iB = j.iB;
    bool ok = true;

    switch (j.type) {
    case JointType2D::Revolute: {
        f32 angularError = 0.f;
        const bool fixedRotation = iA + iB == 0.f;
        if (j.enableLimit && !fixedRotation) {
            const f32 angle = aB - aA - j.referenceAngle;
            f32 c = 0.f;
            if (std::fabs(j.upper - j.lower) < 2.f * kAngularSlop) {
                c = std::clamp(angle - j.lower, -kMaxAngularCorrection, kMaxAngularCorrection);
            } else if (angle <= j.lower) {
                c = std::clamp(angle - j.lower + kAngularSlop, -kMaxAngularCorrection, 0.f);
            } else if (angle >= j.upper) {
                c = std::clamp(angle - j.upper - kAngularSlop, 0.f, kMaxAngularCorrection);
            }
            const f32 limitImpulse = -j.axialMass * c;
            aA -= iA * limitImpulse;
            aB += iB * limitImpulse;
            angularError = std::fabs(c);
        }
        const Rot qA(aA);
        const Rot qB(aB);
        const vec2 rA = mul(qA, j.localAnchorA - j.localCenterA);
        const vec2 rB = mul(qB, j.localAnchorB - j.localCenterB);
        const vec2 c = cB + rB - cA - rA;
        const f32 positionError = length(c);
        Mat22 k;
        k.ex.x = mA + mB + iA * rA.y * rA.y + iB * rB.y * rB.y;
        k.ex.y = -iA * rA.x * rA.y - iB * rB.x * rB.y;
        k.ey.x = k.ex.y;
        k.ey.y = mA + mB + iA * rA.x * rA.x + iB * rB.x * rB.x;
        const vec2 impulse = -k.solve(c);
        cA -= mA * impulse;
        aA -= iA * cross(rA, impulse);
        cB += mB * impulse;
        aB += iB * cross(rB, impulse);
        ok = positionError <= kLinearSlop && angularError <= kAngularSlop;
        break;
    }
    case JointType2D::Distance: {
        if (j.frequencyHz > 0.f) {
            ok = true; // soft constraint: no position correction
            break;
        }
        const Rot qA(aA);
        const Rot qB(aB);
        const vec2 rA = mul(qA, j.localAnchorA - j.localCenterA);
        const vec2 rB = mul(qB, j.localAnchorB - j.localCenterB);
        vec2 u = cB + rB - cA - rA;
        const f32 len = normalize(u);
        const f32 c = std::clamp(len - j.length, -kMaxLinearCorrection, kMaxLinearCorrection);
        const f32 impulse = -j.mass * c;
        const vec2 p = impulse * u;
        cA -= mA * p;
        aA -= iA * cross(rA, p);
        cB += mB * p;
        aB += iB * cross(rB, p);
        ok = std::fabs(c) < kLinearSlop;
        break;
    }
    case JointType2D::Prismatic: {
        const Rot qA(aA);
        const Rot qB(aB);
        const vec2 rA = mul(qA, j.localAnchorA - j.localCenterA);
        const vec2 rB = mul(qB, j.localAnchorB - j.localCenterB);
        const vec2 d = cB + rB - cA - rA;
        const vec2 axis = mul(qA, j.localXAxisA);
        const f32 a1 = cross(d + rA, axis);
        const f32 a2 = cross(rB, axis);
        const vec2 perp = mul(qA, j.localYAxisA);
        const f32 s1 = cross(d + rA, perp);
        const f32 s2 = cross(rB, perp);
        const vec2 c1{dot(perp, d), aB - aA - j.referenceAngle};
        f32 linearError = std::fabs(c1.x);
        const f32 angularError = std::fabs(c1.y);
        bool active = false;
        f32 c2 = 0.f;
        if (j.enableLimit) {
            const f32 translation = dot(axis, d);
            if (std::fabs(j.upper - j.lower) < 2.f * kLinearSlop) {
                c2 = translation;
                linearError = std::max(linearError, std::fabs(translation));
                active = true;
            } else if (translation <= j.lower) {
                c2 = std::min(translation - j.lower, 0.f);
                linearError = std::max(linearError, j.lower - translation);
                active = true;
            } else if (translation >= j.upper) {
                c2 = std::max(translation - j.upper, 0.f);
                linearError = std::max(linearError, translation - j.upper);
                active = true;
            }
        }
        Vec3f impulse;
        const f32 k11 = mA + mB + iA * s1 * s1 + iB * s2 * s2;
        const f32 k12 = iA * s1 + iB * s2;
        f32 k22 = iA + iB;
        if (k22 == 0.f) {
            k22 = 1.f;
        }
        if (active) {
            const f32 k13 = iA * s1 * a1 + iB * s2 * a2;
            const f32 k23 = iA * a1 + iB * a2;
            const f32 k33 = mA + mB + iA * a1 * a1 + iB * a2 * a2;
            Mat33 k;
            k.ex = {k11, k12, k13};
            k.ey = {k12, k22, k23};
            k.ez = {k13, k23, k33};
            impulse = k.solve33({-c1.x, -c1.y, -c2});
        } else {
            Mat33 k;
            k.ex = {k11, k12, 0.f};
            k.ey = {k12, k22, 0.f};
            const vec2 i2 = k.solve22(-c1);
            impulse = {i2.x, i2.y, 0.f};
        }
        const vec2 p = impulse.x * perp + impulse.z * axis;
        const f32 lA = impulse.x * s1 + impulse.y + impulse.z * a1;
        const f32 lB = impulse.x * s2 + impulse.y + impulse.z * a2;
        cA -= mA * p;
        aA -= iA * lA;
        cB += mB * p;
        aB += iB * lB;
        ok = linearError <= kLinearSlop && angularError <= kAngularSlop;
        break;
    }
    }
    bA.c = cA;
    bA.a = aA;
    bB.c = cB;
    bB.a = aB;
    return ok;
}

} // namespace fuse::physics::p2d
