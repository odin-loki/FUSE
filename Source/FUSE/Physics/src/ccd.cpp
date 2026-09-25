#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/rotation.hpp>

#include <atomic>

namespace fuse::physics {

namespace {

/// Every advancement step certifies the separation stays above this fraction of the tolerance, so
/// the reported TOI is never in contact; the search stops once the separation is <= tolerance.
constexpr f32 kTargetFraction = 0.25f;

CollisionShapeType shapeType(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return CollisionShapeType::Sphere;
    }
    return static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
}

u32 findShapeForBody(const CollisionShapeSoA& shapes, u32 bodyIndex, CollisionShapeType preferred) {
    // Common layout: one shape per body, added in body order. Multi-shape bodies (adjacent
    // shapes of the same body) take the scan below so `preferred` still wins.
    if (bodyIndex < shapes.count() && shapes.bodyIndices[bodyIndex] == bodyIndex) {
        const bool soleShape = (bodyIndex == 0u || shapes.bodyIndices[bodyIndex - 1u] != bodyIndex) &&
                               (bodyIndex + 1u >= shapes.count() || shapes.bodyIndices[bodyIndex + 1u] != bodyIndex);
        if (soleShape || static_cast<CollisionShapeType>(shapes.types[bodyIndex]) == preferred) {
            return bodyIndex;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex &&
            static_cast<CollisionShapeType>(shapes.types[i]) == preferred) {
            return i;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex) {
            return i;
        }
    }
    return shapes.count();
}

bool bodyNeedsCcd(const RigidBodySoA& bodies, u32 bodyIndex) {
    return bodyIndex < bodies.count() && (bodies.flags[bodyIndex] & RB_CCD) != 0u;
}

/// A shape as an oriented box "core" (degenerate for spheres and capsules) swept by a ball of
/// `radius`: the rotation-dependent part is the core alone.
struct CcdCore {
    vec3 centre{};
    vec3 axis[3]{};
    f32 extent[3]{};
    f32 radius = 0.f;
};

CcdCore makeCore(CollisionShapeType type, vec3 params, vec3 position, const quat& orientation) {
    CcdCore core{};
    core.centre = position;
    core.axis[0] = rotate(orientation, {1.f, 0.f, 0.f});
    core.axis[1] = rotate(orientation, {0.f, 1.f, 0.f});
    core.axis[2] = rotate(orientation, {0.f, 0.f, 1.f});
    switch (type) {
    case CollisionShapeType::Box:
        core.extent[0] = params.x;
        core.extent[1] = params.y;
        core.extent[2] = params.z;
        break;
    case CollisionShapeType::Capsule:
        core.extent[1] = params.y;
        core.radius = params.x;
        break;
    default:
        core.radius = params.x;
        break;
    }
    return core;
}

/// Distance from the centre to the furthest point of the core (what rotation can move).
f32 coreReach(CollisionShapeType type, vec3 params) {
    switch (type) {
    case CollisionShapeType::Box:
        return params.length();
    case CollisionShapeType::Capsule:
        return params.y;
    default:
        return 0.f;
    }
}

f32 projectedCoreRadius(const CcdCore& core, vec3 axis) {
    return std::fabs(core.axis[0].dot(axis)) * core.extent[0] + std::fabs(core.axis[1].dot(axis)) * core.extent[1] +
           std::fabs(core.axis[2].dot(axis)) * core.extent[2];
}

bool isPointCore(const CcdCore& core) {
    return core.extent[0] == 0.f && core.extent[1] == 0.f && core.extent[2] == 0.f;
}

bool isSegmentCore(const CcdCore& core) {
    return core.extent[0] == 0.f && core.extent[2] == 0.f;
}

/// Lower bound on the distance between two cores (exact for point/segment pairs and point-box;
/// the separating-axis bound over the 15 box axes otherwise). Negative when they overlap.
/// `axisBtoA` receives the unit direction (B towards A) along which that separation is measured, or
/// zero when there is none (degenerate closest points); the separation is exactly
/// n.(cA - cB) - hA(n) - hB(n) along it, which conservative advancement bounds linearly in time.
f32 coreSeparationLowerBound(const CcdCore& a, const CcdCore& b, vec3& axisBtoA) {
    axisBtoA = {};
    if (isSegmentCore(a) && isSegmentCore(b)) {
        const vec3 ha = a.axis[1] * a.extent[1];
        const vec3 hb = b.axis[1] * b.extent[1];
        vec3 onA{};
        vec3 onB{};
        narrowphase::closestPointsSegmentSegment(a.centre - ha, a.centre + ha, b.centre - hb, b.centre + hb, onA, onB);
        const vec3 gap = onA - onB;
        const f32 length = gap.length();
        if (length > 1e-6f) {
            axisBtoA = gap * (1.f / length);
        }
        return length;
    }
    if (isPointCore(a) || isPointCore(b)) {
        const bool pointIsA = isPointCore(a);
        const CcdCore& point = pointIsA ? a : b;
        const CcdCore& box = pointIsA ? b : a;
        const vec3 d = point.centre - box.centre;
        vec3 outside{};
        f32 inside = -1e30f;
        for (u32 k = 0; k < 3u; ++k) {
            const f32 coordinate = d.dot(box.axis[k]);
            const f32 excess = std::fabs(coordinate) - box.extent[k];
            inside = std::max(inside, excess);
            if (excess > 0.f) {
                outside += box.axis[k] * (coordinate > 0.f ? excess : -excess);
            }
        }
        if (inside <= 0.f) {
            return inside;
        }
        const f32 length = outside.length();
        if (length > 1e-6f) {
            axisBtoA = outside * ((pointIsA ? 1.f : -1.f) / length);
        }
        return length;
    }
    const vec3 d = a.centre - b.centre;
    f32 separation = -1e30f;
    const auto test = [&](vec3 axis) {
        const f32 along = d.dot(axis);
        const f32 candidate = std::fabs(along) - projectedCoreRadius(a, axis) - projectedCoreRadius(b, axis);
        if (candidate > separation) {
            separation = candidate;
            axisBtoA = along >= 0.f ? axis : axis * -1.f;
        }
    };
    for (u32 i = 0; i < 3u; ++i) {
        test(a.axis[i]);
        test(b.axis[i]);
    }
    for (u32 i = 0; i < 3u; ++i) {
        for (u32 j = 0; j < 3u; ++j) {
            const vec3 axis = a.axis[i].cross(b.axis[j]);
            const f32 length = axis.length();
            if (length > 1e-4f) {
                test(axis * (1.f / length));
            }
        }
    }
    // Segment vs box: the SAT value can badly under-estimate the distance (edge/vertex regions),
    // which throttles conservative advancement. When they are disjoint the exact distance is the
    // minimum over the segment's endpoints against the box and the segment against the 12 box edges.
    if (separation > 0.f && (isSegmentCore(a) != isSegmentCore(b))) {
        const bool segmentIsA = isSegmentCore(a);
        const CcdCore& segment = segmentIsA ? a : b;
        const CcdCore& box = segmentIsA ? b : a;
        const vec3 hs = segment.axis[1] * segment.extent[1];
        const vec3 s0 = segment.centre - hs;
        const vec3 s1 = segment.centre + hs;
        f32 best = 1e30f;
        vec3 bestSeg{};
        vec3 bestBox{};
        const auto closestOnBox = [&](vec3 p) {
            const vec3 local = p - box.centre;
            vec3 q = box.centre;
            for (u32 k = 0; k < 3u; ++k) {
                q += box.axis[k] * std::clamp(local.dot(box.axis[k]), -box.extent[k], box.extent[k]);
            }
            return q;
        };
        for (const vec3& p : {s0, s1}) {
            const vec3 q = closestOnBox(p);
            const f32 dist = (p - q).length();
            if (dist < best) {
                best = dist;
                bestSeg = p;
                bestBox = q;
            }
        }
        for (u32 k = 0; k < 3u; ++k) { // edges parallel to box axis k
            const u32 i = (k + 1u) % 3u;
            const u32 j = (k + 2u) % 3u;
            for (int si = -1; si <= 1; si += 2) {
                for (int sj = -1; sj <= 1; sj += 2) {
                    const vec3 mid = box.centre + box.axis[i] * (box.extent[i] * static_cast<f32>(si)) +
                                     box.axis[j] * (box.extent[j] * static_cast<f32>(sj));
                    const vec3 he = box.axis[k] * box.extent[k];
                    vec3 onSeg{};
                    vec3 onEdge{};
                    narrowphase::closestPointsSegmentSegment(s0, s1, mid - he, mid + he, onSeg, onEdge);
                    const f32 dist = (onSeg - onEdge).length();
                    if (dist < best) {
                        best = dist;
                        bestSeg = onSeg;
                        bestBox = onEdge;
                    }
                }
            }
        }
        if (best > separation && best > 1e-6f) {
            separation = best;
            const vec3 gap = segmentIsA ? bestSeg - bestBox : bestBox - bestSeg;
            axisBtoA = gap * (1.f / best);
        }
    }
    return separation;
}

/// Signed distance from a plane to the lowest point of a core (minus its radius).
f32 corePlaneSeparation(const CcdCore& core, vec3 normal, f32 planeDistance) {
    return core.centre.dot(normal) - planeDistance - projectedCoreRadius(core, normal) - core.radius;
}

f32 shapeSeparationLowerBound(const narrowphase::ShapeInstance& a,
                              const narrowphase::ShapeInstance& b,
                              vec3& axisBtoA) {
    if (b.type == CollisionShapeType::Plane) {
        axisBtoA = b.params;
        return corePlaneSeparation(makeCore(a.type, a.params, a.position, a.orientation), b.params, b.scalar);
    }
    if (a.type == CollisionShapeType::Plane) {
        axisBtoA = a.params * -1.f;
        return corePlaneSeparation(makeCore(b.type, b.params, b.position, b.orientation), a.params, a.scalar);
    }
    const CcdCore coreA = makeCore(a.type, a.params, a.position, a.orientation);
    const CcdCore coreB = makeCore(b.type, b.params, b.position, b.orientation);
    return coreSeparationLowerBound(coreA, coreB, axisBtoA) - coreA.radius - coreB.radius;
}

/// Second-order (Taylor) safe step along a fixed axis n (B towards A) for rotating cores.
/// A core point at offset r from its centre, rotating about a fixed world axis by theta per frame,
/// satisfies m.(R(tau) r) <= m.r + tau m.(theta x r) + tau^2 / 2 |theta|^2 |r| for any unit m, so
/// each core's support is bounded by the max over its vertices v of (a_v + tau b_v) plus that
/// curvature term. The resulting lower bound on s_n(t + tau) is, for every vertex pair, a concave
/// quadratic whose first crossing of tolerance / 2 is closed form; the smallest over the pairs is a
/// certified step. The first-order term is the exact support velocity (not |theta x n| r), so this
/// behaves like a safeguarded Newton step: it converges in a few iterations even when the far tip
/// of a spinning bar sweeps fast but the contact point moves slowly or tangentially.
struct CoreVertices {
    vec3 offsets[8]{};
    u32 count = 0;
};

CoreVertices coreVertices(const CcdCore& core) {
    CoreVertices out;
    out.offsets[0] = {};
    out.count = 1;
    for (u32 k = 0; k < 3u; ++k) {
        if (core.extent[k] == 0.f) {
            continue;
        }
        const vec3 half = core.axis[k] * core.extent[k];
        const u32 existing = out.count;
        for (u32 i = 0; i < existing; ++i) {
            out.offsets[existing + i] = out.offsets[i] - half;
            out.offsets[i] = out.offsets[i] + half;
        }
        out.count = existing * 2u;
    }
    return out;
}

/// One side of a Taylor step: a (piece of a) core rotating about `pivot`, or a static plane.
struct TaylorSide {
    const CcdCore* core = nullptr; ///< null for a plane
    vec3 pivot{};
    vec3 rotation{};
    vec3 planeNormal{};
    f32 planeScalar = 0.f;
};

/// Rotation of `v` by the rotation vector `theta` (Rodrigues).
vec3 rotateBy(vec3 theta, vec3 v) {
    const f32 angle = theta.length();
    if (angle <= 0.f) {
        return v;
    }
    const vec3 u = theta * (1.f / angle);
    const f32 c = std::cos(angle);
    const f32 sn = std::sin(angle);
    return v * c + u.cross(v) * sn + u * (u.dot(v) * (1.f - c));
}

/// Vertices of one side as offsets from its rotation pivot.
struct SupportVertices {
    vec3 offsets[8]{};
    u32 count = 0;
    vec3 rotation{};
    f32 curvature = 0.f; ///< bound on |d^2/dtau^2 (m(tau).R(tau) r)| over the vertices
};

SupportVertices supportVertices(const TaylorSide& side, vec3 frameRotation) {
    SupportVertices out;
    const CoreVertices vertices = coreVertices(*side.core);
    out.count = vertices.count;
    out.rotation = side.rotation;
    const f32 spin = side.rotation.length();
    const vec3 axis = spin > 0.f ? side.rotation * (1.f / spin) : vec3{};
    f32 radial = 0.f;
    f32 reach = 0.f;
    for (u32 i = 0; i < vertices.count; ++i) {
        const vec3 r = side.core->centre + vertices.offsets[i] - side.pivot;
        out.offsets[i] = r;
        radial = std::max(radial, (r - axis * axis.dot(r)).length());
        reach = std::max(reach, r.length());
    }
    const f32 frameSpin = frameRotation.length();
    if (frameSpin == 0.f) {
        out.curvature = spin * spin * radial; // |w x (w x x)| <= |w|^2 dist-to-axis
    } else {
        // d/dtau (m.x) = m.((theta - phi) x x); differentiating again (dm = phi x m, dx = theta x x)
        // gives at most |theta - phi| (|phi| + |theta|) |x|.
        out.curvature = (side.rotation - frameRotation).length() * (frameSpin + spin) * reach;
    }
    return out;
}

/// Support value sign * m(tau).(R(theta tau) r) and its rate for every vertex, m(tau) = R(phi tau) n.
void supportAt(const SupportVertices& side, vec3 frameRotation, vec3 m, f32 sign, f32 tau, f32 (&value)[8], f32 (&rate)[8]) {
    const vec3 relativeSpin = side.rotation - frameRotation;
    const vec3 turn = side.rotation * tau;
    for (u32 i = 0; i < side.count; ++i) {
        const vec3 x = rotateBy(turn, side.offsets[i]);
        value[i] = sign * m.dot(x);
        rate[i] = sign * m.dot(relativeSpin.cross(x));
    }
}

/// Certified advance (fraction of the frame) along the separating direction n rotating with the
/// frame rotation vector `frame` (zero: a fixed world axis; theta_A or theta_B: an axis attached to
/// that body, which removes that body's own tilt from the bound). For such an axis
///   s(tau) = m(tau).(pA - pB)(tau) - hA(-m(tau)) - hB(m(tau)) - radii,  m(tau) = R(frame tau) n,
/// is a lower bound on the distance (projection onto any unit direction), with each support the
/// max over the core's vertices, rotated exactly (Rodrigues). Each inner step expands that exact
/// function at the certified time reached so far (value + rate + curvature remainder) and takes
/// the first crossing of tolerance / 2 of the resulting concave quadratic, so the certificate
/// tracks the separation itself rather than a frame-wide worst-case speed. Inner steps cost a few
/// trig calls on <= 16 vertices, not a geometry query; `TOIResult::iterations` counts the outer
/// geometric evaluations.
f32 taylorStep(const TaylorSide& a, const TaylorSide& b, vec3 n, vec3 frame, vec3 relative, f32 tolerance, u32 maxInner) {
    const bool anyPlane = a.core == nullptr || b.core == nullptr;
    if (anyPlane && (frame.x != 0.f || frame.y != 0.f || frame.z != 0.f)) {
        return 0.f; // a plane's support is only known along its own (fixed) normal
    }
    f32 radii = 0.f;
    f32 planeOffset = 0.f;
    vec3 pivotGap{};
    SupportVertices sideA{};
    SupportVertices sideB{};
    sideA.count = 1;
    sideB.count = 1;
    if (a.core != nullptr) {
        pivotGap = pivotGap + a.pivot;
        radii += a.core->radius;
        sideA = supportVertices(a, frame);
    } else {
        planeOffset -= a.planeScalar; // n = -planeNormal: n.x_A over the half-space top is -scalar
    }
    if (b.core != nullptr) {
        pivotGap = pivotGap - b.pivot;
        radii += b.core->radius;
        sideB = supportVertices(b, frame);
    } else {
        planeOffset -= b.planeScalar;
    }
    const f32 frameSpin = frame.length();
    const f32 relativeLength = relative.length();
    const f32 curvature = sideA.curvature + sideB.curvature +
                          frameSpin * frameSpin * (pivotGap.length() + relativeLength) +
                          2.f * frameSpin * relativeLength;
    const f32 half = kTargetFraction * tolerance;
    f32 tau = 0.f;
    for (u32 inner = 0; inner < maxInner; ++inner) {
        const vec3 m = rotateBy(frame * tau, n);
        const vec3 gap = pivotGap + relative * tau;
        const f32 centre = m.dot(gap) + planeOffset;
        const f32 centreRate = frame.cross(m).dot(gap) + m.dot(relative);
        f32 valueA[8] = {};
        f32 rateA[8] = {};
        f32 valueB[8] = {};
        f32 rateB[8] = {};
        if (a.core != nullptr) {
            supportAt(sideA, frame, m, -1.f, tau, valueA, rateA);
        }
        if (b.core != nullptr) {
            supportAt(sideB, frame, m, 1.f, tau, valueB, rateB);
        }
        f32 best = 2.f;
        for (u32 i = 0; i < sideA.count; ++i) {
            for (u32 j = 0; j < sideB.count; ++j) {
                const f32 margin = centre - valueA[i] - valueB[j] - radii - half;
                if (margin <= 0.f) {
                    return tau; // the certificate along this axis ends here
                }
                const f32 d = centreRate - rateA[i] - rateB[j];
                const f32 root = std::sqrt(d * d + 2.f * curvature * margin);
                const f32 denominator = root - d; // h = 2 margin / (sqrt(d^2 + 2 K margin) - d)
                if (denominator > 1e-12f) {
                    best = std::min(best, 2.f * margin / denominator);
                }
            }
        }
        tau += best;
        if (tau >= 1.f || best < 1e-3f * tau) {
            break; // past the frame, or stalled against the target
        }
    }
    return tau;
}

/// Best certified advance over a fixed world axis and axes attached to either rotating body.
f32 taylorStepAnyFrame(const TaylorSide& a, const TaylorSide& b, vec3 n, vec3 relative, f32 tolerance, u32 maxInner) {
    f32 best = taylorStep(a, b, n, {}, relative, tolerance, maxInner);
    if (a.core != nullptr && b.core != nullptr) {
        if (a.rotation.dot(a.rotation) > 0.f) {
            best = std::max(best, taylorStep(a, b, n, a.rotation, relative, tolerance, maxInner));
        }
        if (b.rotation.dot(b.rotation) > 0.f) {
            best = std::max(best, taylorStep(a, b, n, b.rotation, relative, tolerance, maxInner));
        }
    }
    return best;
}

/// Piece of a rotating core for the hierarchical (C2A-style) advancement bound: a sub-box of the
/// core with the same ball radius, and its reach from the shape's rotation centre.
struct CorePiece {
    CcdCore core{};
    f32 halfDiagonal = 0.f;
    f32 reach = 0.f;
    u32 depth = 0;
};

/// One side of a swept pair for the piece hierarchy.
struct SweptSide {
    const narrowphase::ShapeInstance* shape = nullptr;
    vec3 rotation{};   ///< rotation vector over the frame
    f32 spin = 0.f;    ///< |rotation|
    bool plane = false;
};

f32 halfDiagonalOf(const CcdCore& core) {
    return std::sqrt(core.extent[0] * core.extent[0] + core.extent[1] * core.extent[1] + core.extent[2] * core.extent[2]);
}

CorePiece rootPiece(const narrowphase::ShapeInstance& shape) {
    CorePiece piece;
    piece.core = makeCore(shape.type, shape.params, shape.position, shape.orientation);
    piece.halfDiagonal = halfDiagonalOf(piece.core);
    piece.reach = piece.halfDiagonal;
    return piece;
}

/// Halves a piece across its longest local axis.
void splitPiece(const CorePiece& piece, vec3 pivot, CorePiece& first, CorePiece& second) {
    u32 axis = 0;
    for (u32 k = 1; k < 3u; ++k) {
        axis = piece.core.extent[k] > piece.core.extent[axis] ? k : axis;
    }
    const f32 half = 0.5f * piece.core.extent[axis];
    first = piece;
    first.core.extent[axis] = half;
    first.halfDiagonal = halfDiagonalOf(first.core);
    first.depth = piece.depth + 1u;
    second = first;
    first.core.centre = piece.core.centre + piece.core.axis[axis] * half;
    second.core.centre = piece.core.centre - piece.core.axis[axis] * half;
    first.reach = std::min(piece.reach, (first.core.centre - pivot).length() + first.halfDiagonal);
    second.reach = std::min(piece.reach, (second.core.centre - pivot).length() + second.halfDiagonal);
}

/// Hierarchical conservative-advancement bound (in the spirit of C2A, Tang et al. 2009): covers
/// both rotating cores with pieces and returns the smallest per-piece-pair safe step, so the
/// rotation bound of each pair uses that piece's own reach instead of the whole shape's. A far
/// tip that cannot reach the other shape soon no longer limits the step taken near the pivot.
/// Every returned step is a certificate: no piece pair (so no point pair) gets closer than
/// tolerance / 2 before it.
class PieceAdvancementBound {
public:
    PieceAdvancementBound(const SweptSide& a,
                          const SweptSide& b,
                          vec3 relative,
                          f32 wholeSeparation,
                          vec3 wholeAxis,
                          f32 tolerance)
        : a_(a), b_(b), relative_(relative), relativeLength_(relative.length()), wholeSeparation_(wholeSeparation),
          wholeAxis_(wholeAxis),
          tolerance_(tolerance) {}

    f32 run() {
        CorePiece rootA = a_.plane ? CorePiece{} : rootPiece(*a_.shape);
        CorePiece rootB = b_.plane ? CorePiece{} : rootPiece(*b_.shape);
        visit(rootA, rootB);
        return best_;
    }

private:
    static constexpr f32 kTiltFraction = 0.5f; ///< split while spin x half diagonal > this x gap
    static constexpr u32 kMaxDepth = 30u;
    static constexpr u32 kMaxLeafPairs = 256u;

    f32 motionBound(const CorePiece& pa, const CorePiece& pb) const {
        return relativeLength_ + a_.spin * pa.reach + b_.spin * pb.reach;
    }

    /// Lower bound on the pair's distance: the core separation bound (exact plane distance for a
    /// plane). Bounding spheres are far too loose for long thin pieces (a 2 m post) to prune.
    f32 gapLowerBound(const CorePiece& pa, const CorePiece& pb) const {
        if (b_.plane) {
            return corePlaneSeparation(pa.core, b_.shape->params, b_.shape->scalar);
        }
        if (a_.plane) {
            return corePlaneSeparation(pb.core, a_.shape->params, a_.shape->scalar);
        }
        vec3 unusedAxis{};
        return coreSeparationLowerBound(pa.core, pb.core, unusedAxis) - pa.core.radius - pb.core.radius;
    }

    f32 leafStep(const CorePiece& pa, const CorePiece& pb) const {
        vec3 axis{};
        f32 separation = 0.f;
        if (b_.plane) {
            axis = b_.shape->params;
            separation = corePlaneSeparation(pa.core, axis, b_.shape->scalar);
        } else if (a_.plane) {
            axis = a_.shape->params * -1.f;
            separation = corePlaneSeparation(pb.core, a_.shape->params, a_.shape->scalar);
        } else {
            separation = coreSeparationLowerBound(pa.core, pb.core, axis) - pa.core.radius - pb.core.radius;
        }
        const f32 half = kTargetFraction * tolerance_;
        f32 step = 0.f;
        // The pair is never closer than the whole shapes are, so the scalar Lipschitz step applies
        // to max(piece separation, whole separation).
        const f32 bound = motionBound(pa, pb);
        step = bound > 1e-9f ? (std::max(separation, wholeSeparation_) - half) / bound : 2.f;
        if (axis.x != 0.f || axis.y != 0.f || axis.z != 0.f) {
            const f32 closing =
                -axis.dot(relative_) + a_.rotation.cross(axis).length() * pa.reach + b_.rotation.cross(axis).length() * pb.reach;
            step = std::max(step, closing > 1e-9f ? (separation - half) / closing : 2.f);
            const TaylorSide sideA{a_.plane ? nullptr : &pa.core, a_.shape->position, a_.rotation,
                                   a_.shape->params, a_.shape->scalar};
            const TaylorSide sideB{b_.plane ? nullptr : &pb.core, b_.shape->position, b_.rotation,
                                   b_.shape->params, b_.shape->scalar};
            step = std::max(step, taylorStepAnyFrame(sideA, sideB, axis, relative_, tolerance_, 16u));
            // Pieces next to a sliding contact see oblique own axes that pick up the tangential
            // sliding speed; the whole pair's separating axis (the contact normal) does not.
            if (wholeAxis_.x != 0.f || wholeAxis_.y != 0.f || wholeAxis_.z != 0.f) {
                step = std::max(step, taylorStepAnyFrame(sideA, sideB, wholeAxis_, relative_, tolerance_, 16u));
            }
        }
        return step;
    }

    /// Split while the piece's own rotation (its tilt over the frame) is large against the gap: the
    /// leaf Taylor bound is then limited by that tilt rather than by the contact point's motion.
    bool wantsSplit(const CorePiece& piece, f32 spin, f32 gap) const {
        return piece.depth < kMaxDepth && piece.halfDiagonal > 0.5f * tolerance_ &&
               spin * piece.halfDiagonal > kTiltFraction * std::max(gap, tolerance_);
    }

    void visit(const CorePiece& pa, const CorePiece& pb) {
        const f32 bound = motionBound(pa, pb);
        const f32 gap = std::max(gapLowerBound(pa, pb), wholeSeparation_);
        if (bound <= 1e-9f || (gap - kTargetFraction * tolerance_) / bound >= best_) {
            return; // nothing inside this pair can move close enough to lower the step
        }
        const bool splitA = !a_.plane && leafPairs_ < kMaxLeafPairs && wantsSplit(pa, a_.spin, gap);
        const bool splitB = !b_.plane && leafPairs_ < kMaxLeafPairs && wantsSplit(pb, b_.spin, gap);
        if (!splitA && !splitB) {
            ++leafPairs_;
            const f32 leaf = leafStep(pa, pb);
            best_ = std::min(best_, leaf);
            return;
        }
        const bool pickA = splitA && (!splitB || a_.spin * pa.halfDiagonal >= b_.spin * pb.halfDiagonal);
        CorePiece first;
        CorePiece second;
        if (pickA) {
            splitPiece(pa, a_.shape->position, first, second);
            if (gapLowerBound(second, pb) < gapLowerBound(first, pb)) {
                std::swap(first, second);
            }
            visit(first, pb);
            visit(second, pb);
        } else {
            splitPiece(pb, b_.shape->position, first, second);
            if (gapLowerBound(pa, second) < gapLowerBound(pa, first)) {
                std::swap(first, second);
            }
            visit(pa, first);
            visit(pa, second);
        }
    }

    SweptSide a_;
    SweptSide b_;
    vec3 relative_{};
    f32 relativeLength_ = 0.f;
    f32 wholeSeparation_ = 0.f;
    vec3 wholeAxis_{};
    f32 tolerance_ = 0.f;
    f32 best_ = 2.f;
    u32 leafPairs_ = 0;
};

struct AtomicCcdStats {
    std::atomic<u64> sweeps{0};
    std::atomic<u64> iterativeSweeps{0};
    std::atomic<u64> totalIterations{0};
    std::atomic<u32> maxIterations{0};
};

AtomicCcdStats& ccdStats() {
    static AtomicCcdStats stats;
    return stats;
}

void recordCcdIterations(u32 iterations, bool iterative) {
    AtomicCcdStats& stats = ccdStats();
    stats.sweeps.fetch_add(1u, std::memory_order_relaxed);
    if (iterative) {
        stats.iterativeSweeps.fetch_add(1u, std::memory_order_relaxed);
    }
    stats.totalIterations.fetch_add(iterations, std::memory_order_relaxed);
    u32 seen = stats.maxIterations.load(std::memory_order_relaxed);
    while (iterations > seen &&
           !stats.maxIterations.compare_exchange_weak(seen, iterations, std::memory_order_relaxed)) {
    }
}

} // namespace

CcdIterationStats ccdIterationStats() {
    const AtomicCcdStats& stats = ccdStats();
    CcdIterationStats out;
    out.sweeps = stats.sweeps.load(std::memory_order_relaxed);
    out.iterativeSweeps = stats.iterativeSweeps.load(std::memory_order_relaxed);
    out.totalIterations = stats.totalIterations.load(std::memory_order_relaxed);
    out.maxIterations = stats.maxIterations.load(std::memory_order_relaxed);
    return out;
}

void resetCcdIterationStats() {
    AtomicCcdStats& stats = ccdStats();
    stats.sweeps.store(0u, std::memory_order_relaxed);
    stats.iterativeSweeps.store(0u, std::memory_order_relaxed);
    stats.totalIterations.store(0u, std::memory_order_relaxed);
    stats.maxIterations.store(0u, std::memory_order_relaxed);
}

TOIResult conservativeAdvancementToi(const narrowphase::ShapeInstance& shapeA,
                                     vec3 displacementA,
                                     vec3 rotationA,
                                     const narrowphase::ShapeInstance& shapeB,
                                     vec3 displacementB,
                                     vec3 rotationB,
                                     f32 tolerance) {
    // Each iteration evaluates the separation once at the certified time t and then advances by
    // the largest of several certified steps (each proves no contact before t + step on its own,
    // so their maximum does too); `TOIResult::iterations` counts those evaluations.
    //  - scalar: any surface point moves at most |dx| + |theta| r_core over the frame, so the true
    //    distance D(t) >= d - bound (t' - t);
    //  - directional: for a fixed unit axis n (B towards A),
    //    s_n(t') = n.(cA - cB) - hA(-n) - hB(n) - rA - rB is itself a lower bound on D(t'); its centre
    //    term is linear in time and each rotating core's support changes at most |theta_K x n|
    //    reach_K per frame, so closing_n <= 0 proves a miss for the rest of the frame;
    //  - Taylor (rotating cores): s_n along n, or along n carried by either body's rotation, with
    //    the supports rotated exactly (Rodrigues) and re-expanded at every certified sub-step
    //    (`taylorStep`), a safeguarded Newton step on the separation function itself;
    //  - hierarchical (rotating cores): the same bounds per pair of core pieces, each piece with its
    //    own reach from the pivot (`PieceAdvancementBound`, C2A-style), so a long bar's fast far
    //    tips do not throttle a contact near its pivot.
    // Steps stop kTargetFraction x tolerance short of contact, so the reported TOI is never
    // inside the other shape; the search ends once the separation is <= tolerance.
    // Planes never move in the separation evaluation (their pose is the scalar/normal pair), so
    // their displacement is left out of every bound consistently.
    const bool planeA = shapeA.type == CollisionShapeType::Plane;
    const bool planeB = shapeB.type == CollisionShapeType::Plane;
    const vec3 moveA = planeA ? vec3{} : displacementA;
    const vec3 moveB = planeB ? vec3{} : displacementB;
    const vec3 relative = moveA - moveB;
    const f32 reachA = coreReach(shapeA.type, shapeA.params);
    const f32 reachB = coreReach(shapeB.type, shapeB.params);
    const f32 bound = relative.length() + rotationA.length() * reachA + rotationB.length() * reachB;
    // Rotating cores also take the hierarchical piece bound (a spinning sphere's core is a point).
    const bool rotating = rotationA.length() * reachA + rotationB.length() * reachB > 0.f;
    const auto poseAt = [](const narrowphase::ShapeInstance& shape, vec3 displacement, vec3 rotation, f32 t) {
        narrowphase::ShapeInstance moved = shape;
        moved.position = shape.position + displacement * t;
        moved.orientation = applyRotationVector(shape.orientation, rotation * t);
        return moved;
    };
    f32 t = 0.f;
    u32 iterations = 0;
    narrowphase::ShapeInstance a = shapeA;
    narrowphase::ShapeInstance b = shapeB;
    for (u32 iteration = 0; iteration < kCcdMaxAdvancementSteps; ++iteration) {
        a = poseAt(shapeA, displacementA, rotationA, t);
        b = poseAt(shapeB, displacementB, rotationB, t);
        vec3 axis{};
        const f32 separation = shapeSeparationLowerBound(a, b, axis);
        ++iterations;
        if (separation <= tolerance) {
            break;
        }
        const f32 target = separation - kTargetFraction * tolerance; // stop short, never inside
        f32 step = bound > 1e-9f ? target / bound : 2.f;
        if (axis.x != 0.f || axis.y != 0.f || axis.z != 0.f) {
            const f32 closing =
                -axis.dot(relative) + rotationA.cross(axis).length() * reachA + rotationB.cross(axis).length() * reachB;
            step = closing > 1e-9f ? std::max(step, target / closing) : 2.f;
        }
        if (rotating && t + step <= 1.f && (axis.x != 0.f || axis.y != 0.f || axis.z != 0.f)) {
            const CcdCore coreA = makeCore(a.type, a.params, a.position, a.orientation);
            const CcdCore coreB = makeCore(b.type, b.params, b.position, b.orientation);
            const TaylorSide sideA{planeA ? nullptr : &coreA, a.position, rotationA, a.params, a.scalar};
            const TaylorSide sideB{planeB ? nullptr : &coreB, b.position, rotationB, b.params, b.scalar};
            step = std::max(step, taylorStepAnyFrame(sideA, sideB, axis, relative, tolerance, 64u));
        }
        if (rotating && t + step <= 1.f) {
            const SweptSide sideA{&a, rotationA, rotationA.length(), planeA};
            const SweptSide sideB{&b, rotationB, rotationB.length(), planeB};
            step = std::max(step, PieceAdvancementBound(sideA, sideB, relative, separation, axis, tolerance).run());
        }
        t += step;
        if (t > 1.f) {
            return {.valid = false, .iterations = iterations};
        }
    }
    // Out of steps: t is still a safe (conservative) time to stop at.
    TOIResult result{};
    result.toi = t;
    result.valid = true;
    result.iterations = iterations;
    const narrowphase::ContactManifold manifold = narrowphase::collideShapes(a, b, 0u, 1u, 4.f * tolerance + 1e-3f);
    if (manifold.valid && manifold.pointCount > 0u) {
        result.contactNormal = manifold.contactNormal;
        u32 deepest = 0;
        for (u32 k = 1; k < manifold.pointCount; ++k) {
            deepest = manifold.points[k].penetration > manifold.points[deepest].penetration ? k : deepest;
        }
        result.contactPoint = manifold.points[deepest].point;
    } else if (b.type == CollisionShapeType::Plane) {
        result.contactNormal = b.params;
        result.contactPoint = a.position;
    } else if (a.type == CollisionShapeType::Plane) {
        result.contactNormal = a.params * -1.f;
        result.contactPoint = b.position;
    } else {
        result.contactNormal = (a.position - b.position).normalized();
        result.contactPoint = (a.position + b.position) * 0.5f;
    }
    return result;
}

namespace {

TOIResult dispatchCcdPair(const broadphase::CandidatePair& pair,
                          const RigidBodySoA& bodies,
                          const CollisionShapeSoA& shapes,
                          f32 dt) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count() || dt <= 0.f) {
        return {};
    }

    if (!bodyNeedsCcd(bodies, pair.bodyA) && !bodyNeedsCcd(bodies, pair.bodyB)) {
        return {};
    }

    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return {};
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    const vec3 posA = bodies.positions[pair.bodyA];
    const vec3 posB = bodies.positions[pair.bodyB];
    const vec3 velA = bodies.linearVelocities[pair.bodyA] * dt;
    const vec3 velB = bodies.linearVelocities[pair.bodyB] * dt;

    // Rotation matters only for the non-spherical cores: oriented or spinning boxes and capsules
    // (and every pair without a closed form) take conservative advancement.
    const vec3 spinA = bodies.angularVelocities[pair.bodyA] * dt;
    const vec3 spinB = bodies.angularVelocities[pair.bodyB] * dt;
    const auto needsSweep = [&](CollisionShapeType type, u32 shapeIndex, u32 body, vec3 spin) {
        if (type == CollisionShapeType::Sphere || type == CollisionShapeType::Plane) {
            return false;
        }
        return !isIdentity(bodies.orientations[body]) ||
               spin.length() * coreReach(type, shapes.params[shapeIndex]) > kCcdRotationEpsilon;
    };
    // Closed forms: sphere-sphere, sphere-plane and sphere vs an axis-aligned, non-spinning box.
    const bool sphereOrPlaneOnly = typeA != CollisionShapeType::Capsule && typeB != CollisionShapeType::Capsule;
    const bool analytic = (typeA == CollisionShapeType::Sphere || typeB == CollisionShapeType::Sphere) &&
                          sphereOrPlaneOnly && !needsSweep(typeA, shapeA, pair.bodyA, spinA) &&
                          !needsSweep(typeB, shapeB, pair.bodyB, spinB);
    const auto sweepable = [](CollisionShapeType type) {
        return type == CollisionShapeType::Sphere || type == CollisionShapeType::Box ||
               type == CollisionShapeType::Capsule || type == CollisionShapeType::Plane;
    };
    if (!analytic && (!sweepable(typeA) || !sweepable(typeB) ||
                      (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane))) {
        return {};
    }
    if (!analytic) {
        const narrowphase::ShapeInstance a{typeA, shapes.params[shapeA], shapes.scalars[shapeA], posA,
                                           bodies.orientations[pair.bodyA]};
        const narrowphase::ShapeInstance b{typeB, shapes.params[shapeB], shapes.scalars[shapeB], posB,
                                           bodies.orientations[pair.bodyB]};
        TOIResult result = conservativeAdvancementToi(a, velA, spinA, b, velB, spinB, kCcdTolerance);
        recordCcdIterations(result.iterations, true);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    recordCcdIterations(1u, false); // closed form: one evaluation, no iteration

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Sphere) {
        TOIResult result = sweptSphereSphere(
            posA, velA, shapes.params[shapeA].x, posB, velB, shapes.params[shapeB].x);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Plane) {
        TOIResult result = sweptSpherePlane(
            posA, velA - velB, shapes.params[shapeA].x, shapes.params[shapeB], shapes.scalars[shapeB]);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Sphere) {
        TOIResult result = sweptSpherePlane(
            posB, velB - velA, shapes.params[shapeB].x, shapes.params[shapeA], shapes.scalars[shapeA]);
        if (!result.valid) {
            return {};
        }
        result.contactNormal = result.contactNormal * -1.f;
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Box) {
        const aabb box = makeCenteredAabb(posB, shapes.params[shapeB]);
        TOIResult result =
            sweptSphereAabb(posA, velA - velB, shapes.params[shapeA].x, box);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Sphere) {
        const aabb box = makeCenteredAabb(posA, shapes.params[shapeA]);
        TOIResult result =
            sweptSphereAabb(posB, velB - velA, shapes.params[shapeB].x, box);
        if (!result.valid) {
            return {};
        }
        result.contactNormal = result.contactNormal * -1.f;
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    return {};
}

} // namespace

void runCcdIntoBuffer(const std::vector<broadphase::CandidatePair>& pairs,
                      const RigidBodySoA& bodies,
                      const CollisionShapeSoA& shapes,
                      f32 dt,
                      ToiBufferSoA& buffer) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    if (pairCount == 0u) {
        buffer.clear();
        return;
    }

    buffer.preparePairSlots(pairCount);

    // Per-pair slots are job-safe (disjoint writes). Serial dispatch on the CPU stub avoids
    // scheduler reference-capture flakes; the slot layout matches the future parallel_for path.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        const TOIResult result = dispatchCcdPair(pairs[pairIndex], bodies, shapes, dt);
        if (result.valid) {
            buffer.writeSlot(pairIndex, result);
        }
    }

    buffer.compactAndSort();
}

u32 CcdPipeline::sweepPairs(const RigidBodySoA& bodies,
                            const CollisionShapeSoA& shapes,
                            const std::vector<broadphase::CandidatePair>& pairs,
                            f32 dt,
                            std::vector<TOIResult>& outResults) const {
    ToiBufferSoA buffer;
    buffer.reserve(static_cast<u32>(pairs.size()));
    runCcdIntoBuffer(pairs, bodies, shapes, dt, buffer);
    outResults = buffer.toVector();
    lastResultCount_ = buffer.activeCount;
    return lastResultCount_;
}

} // namespace fuse::physics
