// Sequential-impulse contact solver with warm starting and non-linear position correction.

#include "rigid_world_2d.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::physics::p2d {

namespace {

Xf bodyXf(const Body2D& b, vec2 localCenter) {
    Xf xf;
    xf.q = Rot(b.a);
    xf.p = b.c - mul(xf.q, localCenter);
    return xf;
}

struct PositionSolverManifold {
    vec2 normal{0.f, 0.f};
    vec2 point{0.f, 0.f};
    f32 separation = 0.f;
};

PositionSolverManifold positionManifold(const Manifold& m, const Xf& xfA, f32 radiusA, const Xf& xfB, f32 radiusB,
                                        u32 index) {
    PositionSolverManifold out;
    switch (m.type) {
    case ManifoldType::Circles: {
        const vec2 pointA = mul(xfA, m.localPoint);
        const vec2 pointB = mul(xfB, m.points[0].localPoint);
        out.normal = pointB - pointA;
        if (normalize(out.normal) == 0.f) {
            out.normal = {1.f, 0.f};
        }
        out.point = 0.5f * (pointA + pointB);
        out.separation = dot(pointB - pointA, out.normal) - radiusA - radiusB;
        break;
    }
    case ManifoldType::FaceA: {
        out.normal = mul(xfA.q, m.localNormal);
        const vec2 planePoint = mul(xfA, m.localPoint);
        const vec2 clipPoint = mul(xfB, m.points[index].localPoint);
        out.separation = dot(clipPoint - planePoint, out.normal) - radiusA - radiusB;
        out.point = clipPoint;
        break;
    }
    case ManifoldType::FaceB: {
        out.normal = mul(xfB.q, m.localNormal);
        const vec2 planePoint = mul(xfB, m.localPoint);
        const vec2 clipPoint = mul(xfA, m.points[index].localPoint);
        out.separation = dot(clipPoint - planePoint, out.normal) - radiusA - radiusB;
        out.point = clipPoint;
        out.normal = -out.normal;
        break;
    }
    }
    return out;
}

} // namespace

void RigidWorld2D::initContactConstraints(f32 dtRatio) {
    m_constraints.clear();
    for (u32 ci = 0; ci < m_contacts.size(); ++ci) {
        Contact2D& contact = m_contacts[ci];
        if (!contact.touching || contact.sensor || contact.manifold.pointCount == 0u) {
            continue;
        }
        const Shape2D& sa = m_shapes[contact.shapeA];
        const Shape2D& sb = m_shapes[contact.shapeB];
        const Body2D& ba = m_bodies[sa.body];
        const Body2D& bb = m_bodies[sb.body];

        ContactConstraint cc;
        cc.contactIndex = ci;
        cc.bodyA = sa.body;
        cc.bodyB = sb.body;
        cc.friction = contact.friction;
        cc.restitution = contact.restitution;
        cc.invMassA = ba.invMass;
        cc.invMassB = bb.invMass;
        cc.invIA = ba.invI;
        cc.invIB = bb.invI;
        cc.radiusA = sa.radius;
        cc.radiusB = sb.radius;
        cc.localCenterA = ba.localCenter;
        cc.localCenterB = bb.localCenter;
        cc.pointCount = contact.manifold.pointCount;

        WorldManifold wm;
        computeWorldManifold(contact.manifold, bodyXf(ba, ba.localCenter), sa.radius, bodyXf(bb, bb.localCenter),
                             sb.radius, wm);
        cc.normal = wm.normal;
        const vec2 tangent = cross(cc.normal, 1.f);
        for (u32 j = 0; j < cc.pointCount; ++j) {
            VelocityConstraintPoint& vcp = cc.points[j];
            const ManifoldPoint& mp = contact.manifold.points[j];
            vcp.normalImpulse = settings.warmStarting ? dtRatio * mp.normalImpulse : 0.f;
            vcp.tangentImpulse = settings.warmStarting ? dtRatio * mp.tangentImpulse : 0.f;
            vcp.rA = wm.points[j] - ba.c;
            vcp.rB = wm.points[j] - bb.c;
            const f32 rnA = cross(vcp.rA, cc.normal);
            const f32 rnB = cross(vcp.rB, cc.normal);
            const f32 kNormal = cc.invMassA + cc.invMassB + cc.invIA * rnA * rnA + cc.invIB * rnB * rnB;
            vcp.normalMass = kNormal > 0.f ? 1.f / kNormal : 0.f;
            const f32 rtA = cross(vcp.rA, tangent);
            const f32 rtB = cross(vcp.rB, tangent);
            const f32 kTangent = cc.invMassA + cc.invMassB + cc.invIA * rtA * rtA + cc.invIB * rtB * rtB;
            vcp.tangentMass = kTangent > 0.f ? 1.f / kTangent : 0.f;
            vcp.velocityBias = 0.f;
            const f32 vRel = dot(cc.normal, bb.v + cross(bb.w, vcp.rB) - ba.v - cross(ba.w, vcp.rA));
            if (vRel < -kVelocityThreshold) {
                vcp.velocityBias = -cc.restitution * vRel;
            }
        }
        m_constraints.push_back(cc);
    }
}

void RigidWorld2D::warmStartContacts() {
    for (const ContactConstraint& cc : m_constraints) {
        Body2D& ba = m_bodies[cc.bodyA];
        Body2D& bb = m_bodies[cc.bodyB];
        const vec2 tangent = cross(cc.normal, 1.f);
        for (u32 j = 0; j < cc.pointCount; ++j) {
            const VelocityConstraintPoint& vcp = cc.points[j];
            const vec2 p = vcp.normalImpulse * cc.normal + vcp.tangentImpulse * tangent;
            ba.w -= cc.invIA * cross(vcp.rA, p);
            ba.v -= cc.invMassA * p;
            bb.w += cc.invIB * cross(vcp.rB, p);
            bb.v += cc.invMassB * p;
        }
    }
}

void RigidWorld2D::solveContactVelocities() {
    for (ContactConstraint& cc : m_constraints) {
        Body2D& ba = m_bodies[cc.bodyA];
        Body2D& bb = m_bodies[cc.bodyB];
        vec2 vA = ba.v;
        f32 wA = ba.w;
        vec2 vB = bb.v;
        f32 wB = bb.w;
        const vec2 normal = cc.normal;
        const vec2 tangent = cross(normal, 1.f);

        // Friction first: non-penetration matters more.
        for (u32 j = 0; j < cc.pointCount; ++j) {
            VelocityConstraintPoint& vcp = cc.points[j];
            const vec2 dv = vB + cross(wB, vcp.rB) - vA - cross(wA, vcp.rA);
            const f32 vt = dot(dv, tangent);
            f32 lambda = vcp.tangentMass * (-vt);
            const f32 maxFriction = cc.friction * vcp.normalImpulse;
            const f32 newImpulse = std::clamp(vcp.tangentImpulse + lambda, -maxFriction, maxFriction);
            lambda = newImpulse - vcp.tangentImpulse;
            vcp.tangentImpulse = newImpulse;
            const vec2 p = lambda * tangent;
            vA -= cc.invMassA * p;
            wA -= cc.invIA * cross(vcp.rA, p);
            vB += cc.invMassB * p;
            wB += cc.invIB * cross(vcp.rB, p);
        }
        for (u32 j = 0; j < cc.pointCount; ++j) {
            VelocityConstraintPoint& vcp = cc.points[j];
            const vec2 dv = vB + cross(wB, vcp.rB) - vA - cross(wA, vcp.rA);
            const f32 vn = dot(dv, normal);
            f32 lambda = -vcp.normalMass * (vn - vcp.velocityBias);
            const f32 newImpulse = std::max(vcp.normalImpulse + lambda, 0.f);
            lambda = newImpulse - vcp.normalImpulse;
            vcp.normalImpulse = newImpulse;
            const vec2 p = lambda * normal;
            vA -= cc.invMassA * p;
            wA -= cc.invIA * cross(vcp.rA, p);
            vB += cc.invMassB * p;
            wB += cc.invIB * cross(vcp.rB, p);
        }
        ba.v = vA;
        ba.w = wA;
        bb.v = vB;
        bb.w = wB;
    }
}

void RigidWorld2D::storeContactImpulses() {
    for (const ContactConstraint& cc : m_constraints) {
        Manifold& m = m_contacts[cc.contactIndex].manifold;
        for (u32 j = 0; j < cc.pointCount; ++j) {
            m.points[j].normalImpulse = cc.points[j].normalImpulse;
            m.points[j].tangentImpulse = cc.points[j].tangentImpulse;
        }
    }
}

bool RigidWorld2D::solveContactPositions() {
    f32 minSeparation = 0.f;
    for (const ContactConstraint& cc : m_constraints) {
        const Manifold& m = m_contacts[cc.contactIndex].manifold;
        Body2D& ba = m_bodies[cc.bodyA];
        Body2D& bb = m_bodies[cc.bodyB];
        const f32 mA = cc.invMassA;
        const f32 iA = cc.invIA;
        const f32 mB = cc.invMassB;
        const f32 iB = cc.invIB;
        for (u32 j = 0; j < cc.pointCount; ++j) {
            const Xf xfA = bodyXf(ba, cc.localCenterA);
            const Xf xfB = bodyXf(bb, cc.localCenterB);
            const PositionSolverManifold psm = positionManifold(m, xfA, cc.radiusA, xfB, cc.radiusB, j);
            const vec2 rA = psm.point - ba.c;
            const vec2 rB = psm.point - bb.c;
            minSeparation = std::min(minSeparation, psm.separation);
            const f32 c = std::clamp(kBaumgarte * (psm.separation + kLinearSlop), -kMaxLinearCorrection, 0.f);
            const f32 rnA = cross(rA, psm.normal);
            const f32 rnB = cross(rB, psm.normal);
            const f32 k = mA + mB + iA * rnA * rnA + iB * rnB * rnB;
            const f32 impulse = k > 0.f ? -c / k : 0.f;
            const vec2 p = impulse * psm.normal;
            ba.c -= mA * p;
            ba.a -= iA * cross(rA, p);
            bb.c += mB * p;
            bb.a += iB * cross(rB, p);
        }
    }
    return minSeparation >= -3.f * kLinearSlop;
}

} // namespace fuse::physics::p2d
