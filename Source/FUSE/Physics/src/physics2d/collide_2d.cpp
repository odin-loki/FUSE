#include "collide_2d.hpp"

#include <algorithm>
#include <cfloat>

namespace fuse::physics::p2d {

namespace {

struct ClipVertex {
    vec2 v{0.f, 0.f};
    ContactFeature id{};
};

u8 toU8(u32 value) { return static_cast<u8>(value & 0xFFu); }

/// Clips the segment vIn against the half-plane dot(normal, x) <= offset.
u32 clipSegmentToLine(ClipVertex vOut[2], const ClipVertex vIn[2], vec2 normal, f32 offset, u32 vertexIndexA) {
    u32 count = 0u;
    const f32 d0 = dot(normal, vIn[0].v) - offset;
    const f32 d1 = dot(normal, vIn[1].v) - offset;
    if (d0 <= 0.f) {
        vOut[count++] = vIn[0];
    }
    if (d1 <= 0.f) {
        vOut[count++] = vIn[1];
    }
    if (d0 * d1 < 0.f) {
        const f32 interp = d0 / (d0 - d1);
        vOut[count].v = vIn[0].v + interp * (vIn[1].v - vIn[0].v);
        vOut[count].id.indexA = toU8(vertexIndexA);
        vOut[count].id.indexB = vIn[0].id.indexB;
        vOut[count].id.typeA = kFeatureVertex;
        vOut[count].id.typeB = kFeatureFace;
        ++count;
    }
    return count;
}

/// Max separation of poly2 from the faces of poly1 (poly2 expressed through xf1 / xf2).
f32 findMaxSeparation(u32& edgeIndex, const Shape2D& poly1, const Xf& xf1, const Shape2D& poly2, const Xf& xf2) {
    const Xf xf = mulT(xf2, xf1);
    u32 bestIndex = 0u;
    f32 maxSeparation = -FLT_MAX;
    for (u32 i = 0; i < poly1.count; ++i) {
        const vec2 n = mul(xf.q, poly1.normals[i]);
        const vec2 v1 = mul(xf, poly1.vertices[i]);
        f32 si = FLT_MAX;
        for (u32 j = 0; j < poly2.count; ++j) {
            const f32 sij = dot(n, poly2.vertices[j] - v1);
            si = std::min(si, sij);
        }
        if (si > maxSeparation) {
            maxSeparation = si;
            bestIndex = i;
        }
    }
    edgeIndex = bestIndex;
    return maxSeparation;
}

void findIncidentEdge(ClipVertex c[2], const Shape2D& poly1, const Xf& xf1, u32 edge1, const Shape2D& poly2,
                      const Xf& xf2) {
    const vec2 normal1 = mulT(xf2.q, mul(xf1.q, poly1.normals[edge1]));
    u32 index = 0u;
    f32 minDot = FLT_MAX;
    for (u32 i = 0; i < poly2.count; ++i) {
        const f32 d = dot(normal1, poly2.normals[i]);
        if (d < minDot) {
            minDot = d;
            index = i;
        }
    }
    const u32 i1 = index;
    const u32 i2 = i1 + 1u < poly2.count ? i1 + 1u : 0u;
    c[0].v = mul(xf2, poly2.vertices[i1]);
    c[0].id = {toU8(edge1), toU8(i1), kFeatureFace, kFeatureVertex};
    c[1].v = mul(xf2, poly2.vertices[i2]);
    c[1].id = {toU8(edge1), toU8(i2), kFeatureFace, kFeatureVertex};
}

ContactFeature swapFeature(ContactFeature f) {
    ContactFeature out;
    out.indexA = f.indexB;
    out.indexB = f.indexA;
    out.typeA = f.typeB;
    out.typeB = f.typeA;
    return out;
}

} // namespace

void computeWorldManifold(const Manifold& manifold, const Xf& xfA, f32 radiusA, const Xf& xfB, f32 radiusB,
                          WorldManifold& out) {
    if (manifold.pointCount == 0u) {
        return;
    }
    switch (manifold.type) {
    case ManifoldType::Circles: {
        out.normal = {1.f, 0.f};
        const vec2 pointA = mul(xfA, manifold.localPoint);
        const vec2 pointB = mul(xfB, manifold.points[0].localPoint);
        if (distanceSq(pointA, pointB) > kEpsilon * kEpsilon) {
            out.normal = normalized(pointB - pointA);
        }
        const vec2 cA = pointA + radiusA * out.normal;
        const vec2 cB = pointB - radiusB * out.normal;
        out.points[0] = 0.5f * (cA + cB);
        out.separations[0] = dot(cB - cA, out.normal);
        break;
    }
    case ManifoldType::FaceA: {
        out.normal = mul(xfA.q, manifold.localNormal);
        const vec2 planePoint = mul(xfA, manifold.localPoint);
        for (u32 i = 0; i < manifold.pointCount; ++i) {
            const vec2 clipPoint = mul(xfB, manifold.points[i].localPoint);
            const vec2 cA = clipPoint + (radiusA - dot(clipPoint - planePoint, out.normal)) * out.normal;
            const vec2 cB = clipPoint - radiusB * out.normal;
            out.points[i] = 0.5f * (cA + cB);
            out.separations[i] = dot(cB - cA, out.normal);
        }
        break;
    }
    case ManifoldType::FaceB: {
        out.normal = mul(xfB.q, manifold.localNormal);
        const vec2 planePoint = mul(xfB, manifold.localPoint);
        for (u32 i = 0; i < manifold.pointCount; ++i) {
            const vec2 clipPoint = mul(xfA, manifold.points[i].localPoint);
            const vec2 cB = clipPoint + (radiusB - dot(clipPoint - planePoint, out.normal)) * out.normal;
            const vec2 cA = clipPoint - radiusA * out.normal;
            out.points[i] = 0.5f * (cA + cB);
            out.separations[i] = dot(cA - cB, out.normal);
        }
        out.normal = -out.normal;
        break;
    }
    }
}

bool canCollide(ShapeType2D a, ShapeType2D b) { return !(a == ShapeType2D::Edge && b == ShapeType2D::Edge); }

bool collideOrderSwapped(ShapeType2D a, ShapeType2D b) {
    // Routine "A" order: Edge > Polygon > Circle.
    auto rank = [](ShapeType2D t) {
        switch (t) {
        case ShapeType2D::Edge:
            return 2;
        case ShapeType2D::Polygon:
            return 1;
        case ShapeType2D::Circle:
            return 0;
        }
        return 0;
    };
    return rank(a) < rank(b);
}

void collide(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out) {
    out.pointCount = 0u;
    if (a.type == ShapeType2D::Circle && b.type == ShapeType2D::Circle) {
        collideCircles(a, xfA, b, xfB, out);
    } else if (a.type == ShapeType2D::Polygon && b.type == ShapeType2D::Circle) {
        collidePolygonAndCircle(a, xfA, b, xfB, out);
    } else if (a.type == ShapeType2D::Polygon && b.type == ShapeType2D::Polygon) {
        collidePolygons(a, xfA, b, xfB, out);
    } else if (a.type == ShapeType2D::Edge && b.type == ShapeType2D::Circle) {
        collideEdgeAndCircle(a, xfA, b, xfB, out);
    } else if (a.type == ShapeType2D::Edge && b.type == ShapeType2D::Polygon) {
        collideEdgeAndPolygon(a, xfA, b, xfB, out);
    }
}

void collideCircles(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out) {
    out.pointCount = 0u;
    const vec2 pA = mul(xfA, a.center);
    const vec2 pB = mul(xfB, b.center);
    const f32 radius = a.radius + b.radius;
    if (distanceSq(pA, pB) > radius * radius) {
        return;
    }
    out.type = ManifoldType::Circles;
    out.localPoint = a.center;
    out.localNormal = {0.f, 0.f};
    out.pointCount = 1u;
    out.points[0].localPoint = b.center;
    out.points[0].id = 0u;
}

void collidePolygonAndCircle(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out) {
    out.pointCount = 0u;
    const vec2 c = mul(xfB, b.center);
    const vec2 cLocal = mulT(xfA, c);
    u32 normalIndex = 0u;
    f32 separation = -FLT_MAX;
    const f32 radius = a.radius + b.radius;
    for (u32 i = 0; i < a.count; ++i) {
        const f32 s = dot(a.normals[i], cLocal - a.vertices[i]);
        if (s > radius) {
            return;
        }
        if (s > separation) {
            separation = s;
            normalIndex = i;
        }
    }
    const vec2 v1 = a.vertices[normalIndex];
    const vec2 v2 = a.vertices[normalIndex + 1u < a.count ? normalIndex + 1u : 0u];

    out.points[0].localPoint = b.center;
    out.points[0].id = 0u;
    if (separation < kEpsilon) {
        out.pointCount = 1u;
        out.type = ManifoldType::FaceA;
        out.localNormal = a.normals[normalIndex];
        out.localPoint = 0.5f * (v1 + v2);
        return;
    }
    const f32 u1 = dot(cLocal - v1, v2 - v1);
    const f32 u2 = dot(cLocal - v2, v1 - v2);
    if (u1 <= 0.f) {
        if (distanceSq(cLocal, v1) > radius * radius) {
            return;
        }
        out.pointCount = 1u;
        out.type = ManifoldType::FaceA;
        out.localNormal = normalized(cLocal - v1);
        out.localPoint = v1;
    } else if (u2 <= 0.f) {
        if (distanceSq(cLocal, v2) > radius * radius) {
            return;
        }
        out.pointCount = 1u;
        out.type = ManifoldType::FaceA;
        out.localNormal = normalized(cLocal - v2);
        out.localPoint = v2;
    } else {
        const vec2 faceCenter = 0.5f * (v1 + v2);
        const f32 s = dot(cLocal - faceCenter, a.normals[normalIndex]);
        if (s > radius) {
            return;
        }
        out.pointCount = 1u;
        out.type = ManifoldType::FaceA;
        out.localNormal = a.normals[normalIndex];
        out.localPoint = faceCenter;
    }
}

void collidePolygons(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out) {
    out.pointCount = 0u;
    const f32 totalRadius = a.radius + b.radius;

    u32 edgeA = 0u;
    const f32 separationA = findMaxSeparation(edgeA, a, xfA, b, xfB);
    if (separationA > totalRadius) {
        return;
    }
    u32 edgeB = 0u;
    const f32 separationB = findMaxSeparation(edgeB, b, xfB, a, xfA);
    if (separationB > totalRadius) {
        return;
    }

    const Shape2D* poly1 = &a;
    const Shape2D* poly2 = &b;
    Xf xf1 = xfA;
    Xf xf2 = xfB;
    u32 edge1 = edgeA;
    bool flip = false;
    const f32 kTol = 0.1f * kLinearSlop;
    if (separationB > separationA + kTol) {
        poly1 = &b;
        poly2 = &a;
        xf1 = xfB;
        xf2 = xfA;
        edge1 = edgeB;
        out.type = ManifoldType::FaceB;
        flip = true;
    } else {
        out.type = ManifoldType::FaceA;
    }

    ClipVertex incidentEdge[2];
    findIncidentEdge(incidentEdge, *poly1, xf1, edge1, *poly2, xf2);

    const u32 count1 = poly1->count;
    const u32 iv1 = edge1;
    const u32 iv2 = edge1 + 1u < count1 ? edge1 + 1u : 0u;
    vec2 v11 = poly1->vertices[iv1];
    vec2 v12 = poly1->vertices[iv2];

    const vec2 localTangent = normalized(v12 - v11);
    const vec2 localNormal = cross(localTangent, 1.f);
    const vec2 planePoint = 0.5f * (v11 + v12);

    const vec2 tangent = mul(xf1.q, localTangent);
    const vec2 normal = cross(tangent, 1.f);

    v11 = mul(xf1, v11);
    v12 = mul(xf1, v12);

    const f32 frontOffset = dot(normal, v11);
    const f32 sideOffset1 = -dot(tangent, v11) + totalRadius;
    const f32 sideOffset2 = dot(tangent, v12) + totalRadius;

    ClipVertex clipPoints1[2];
    ClipVertex clipPoints2[2];
    if (clipSegmentToLine(clipPoints1, incidentEdge, -tangent, sideOffset1, iv1) < 2u) {
        return;
    }
    if (clipSegmentToLine(clipPoints2, clipPoints1, tangent, sideOffset2, iv2) < 2u) {
        return;
    }

    out.localNormal = localNormal;
    out.localPoint = planePoint;
    u32 pointCount = 0u;
    for (u32 i = 0; i < 2u; ++i) {
        const f32 separation = dot(normal, clipPoints2[i].v) - frontOffset;
        if (separation <= totalRadius) {
            ManifoldPoint& cp = out.points[pointCount];
            cp.localPoint = mulT(xf2, clipPoints2[i].v);
            cp.id = flip ? swapFeature(clipPoints2[i].id).key() : clipPoints2[i].id.key();
            cp.normalImpulse = 0.f;
            cp.tangentImpulse = 0.f;
            ++pointCount;
        }
    }
    out.pointCount = pointCount;
}

void collideEdgeAndCircle(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out) {
    out.pointCount = 0u;
    const vec2 q = mulT(xfA, mul(xfB, b.center));
    const vec2 pa = a.vertices[0];
    const vec2 pb = a.vertices[1];
    const vec2 e = pb - pa;
    vec2 n{e.y, -e.x};
    const f32 offset = dot(n, q - pa);
    if (a.oneSided && offset < 0.f) {
        return;
    }
    const f32 u = dot(e, pb - q);
    const f32 v = dot(e, q - pa);
    const f32 radius = a.radius + b.radius;

    out.points[0].localPoint = b.center;
    out.points[0].normalImpulse = 0.f;
    out.points[0].tangentImpulse = 0.f;

    if (v <= 0.f) {
        // Region A.
        if (distanceSq(q, pa) > radius * radius) {
            return;
        }
        if (a.oneSided) {
            const vec2 e1 = pa - a.vertex0;
            if (dot(e1, pa - q) > 0.f) {
                return; // belongs to the previous edge
            }
        }
        out.pointCount = 1u;
        out.type = ManifoldType::Circles;
        out.localNormal = {0.f, 0.f};
        out.localPoint = pa;
        out.points[0].id = ContactFeature{0u, 0u, kFeatureVertex, kFeatureVertex}.key();
        return;
    }
    if (u <= 0.f) {
        // Region B.
        if (distanceSq(q, pb) > radius * radius) {
            return;
        }
        if (a.oneSided) {
            const vec2 e2 = a.vertex3 - pb;
            if (dot(e2, q - pb) > 0.f) {
                return; // belongs to the next edge
            }
        }
        out.pointCount = 1u;
        out.type = ManifoldType::Circles;
        out.localNormal = {0.f, 0.f};
        out.localPoint = pb;
        out.points[0].id = ContactFeature{1u, 0u, kFeatureVertex, kFeatureVertex}.key();
        return;
    }
    // Region AB.
    const f32 den = dot(e, e);
    const vec2 p = (1.f / den) * (u * pa + v * pb);
    if (distanceSq(q, p) > radius * radius) {
        return;
    }
    if (offset < 0.f) {
        n = -n;
    }
    normalize(n);
    out.pointCount = 1u;
    out.type = ManifoldType::FaceA;
    out.localNormal = n;
    out.localPoint = pa;
    out.points[0].id = ContactFeature{0u, 0u, kFeatureFace, kFeatureVertex}.key();
}

namespace {

enum class AxisType : u8 { Unknown, EdgeA, EdgeB };
struct EpAxis {
    vec2 normal{0.f, 0.f};
    AxisType type = AxisType::Unknown;
    u32 index = 0u;
    f32 separation = -FLT_MAX;
};

struct TempPolygon {
    vec2 vertices[kMaxPolygonVertices2D]{};
    vec2 normals[kMaxPolygonVertices2D]{};
    u32 count = 0u;
};

EpAxis computeEdgeSeparation(const TempPolygon& poly, vec2 v1, vec2 normal1) {
    EpAxis axis;
    axis.type = AxisType::EdgeA;
    const vec2 axes[2] = {normal1, -normal1};
    for (u32 j = 0; j < 2u; ++j) {
        f32 sj = FLT_MAX;
        for (u32 i = 0; i < poly.count; ++i) {
            sj = std::min(sj, dot(axes[j], poly.vertices[i] - v1));
        }
        if (sj > axis.separation) {
            axis.index = j;
            axis.separation = sj;
            axis.normal = axes[j];
        }
    }
    return axis;
}

EpAxis computePolygonSeparation(const TempPolygon& poly, vec2 v1, vec2 v2) {
    EpAxis axis;
    for (u32 i = 0; i < poly.count; ++i) {
        const vec2 n = -poly.normals[i];
        const f32 s1 = dot(n, poly.vertices[i] - v1);
        const f32 s2 = dot(n, poly.vertices[i] - v2);
        const f32 s = std::min(s1, s2);
        if (s > axis.separation) {
            axis.type = AxisType::EdgeB;
            axis.index = i;
            axis.separation = s;
            axis.normal = n;
        }
    }
    return axis;
}

} // namespace

void collideEdgeAndPolygon(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out) {
    out.pointCount = 0u;
    const Xf xf = mulT(xfA, xfB);
    const vec2 centroidB = mul(xf, b.center);
    const vec2 v1 = a.vertices[0];
    const vec2 v2 = a.vertices[1];
    const vec2 edge1 = normalized(v2 - v1);
    const vec2 normal1{edge1.y, -edge1.x};
    const f32 offset1 = dot(normal1, centroidB - v1);
    if (a.oneSided && offset1 < 0.f) {
        return;
    }

    TempPolygon poly;
    poly.count = b.count;
    for (u32 i = 0; i < b.count; ++i) {
        poly.vertices[i] = mul(xf, b.vertices[i]);
        poly.normals[i] = mul(xf.q, b.normals[i]);
    }
    const f32 radius = a.radius + b.radius;

    const EpAxis edgeAxis = computeEdgeSeparation(poly, v1, normal1);
    if (edgeAxis.separation > radius) {
        return;
    }
    const EpAxis polygonAxis = computePolygonSeparation(poly, v1, v2);
    if (polygonAxis.separation > radius) {
        return;
    }

    // Hysteresis: prefer the edge axis unless the polygon axis is clearly better.
    constexpr f32 kRelativeTol = 0.98f;
    constexpr f32 kAbsoluteTol = 0.001f;
    EpAxis primaryAxis = edgeAxis;
    if (polygonAxis.separation - radius > kRelativeTol * (edgeAxis.separation - radius) + kAbsoluteTol) {
        primaryAxis = polygonAxis;
    }

    if (a.oneSided) {
        // Smooth collision against the chain: keep the normal inside this edge's Gauss-map region.
        const vec2 edge0 = normalized(v1 - a.vertex0);
        const vec2 normal0{edge0.y, -edge0.x};
        const bool convex1 = cross(edge0, edge1) >= 0.f;
        const vec2 edge2 = normalized(a.vertex3 - v2);
        const vec2 normal2{edge2.y, -edge2.x};
        const bool convex2 = cross(edge1, edge2) >= 0.f;
        constexpr f32 kSinTol = 0.1f;
        const bool side1 = dot(primaryAxis.normal, edge1) <= 0.f;
        if (side1) {
            if (convex1) {
                if (cross(primaryAxis.normal, normal0) > kSinTol) {
                    return; // the previous edge owns this normal
                }
            } else {
                primaryAxis = edgeAxis; // snap to the edge normal
            }
        } else {
            if (convex2) {
                if (cross(normal2, primaryAxis.normal) > kSinTol) {
                    return; // the next edge owns this normal
                }
            } else {
                primaryAxis = edgeAxis;
            }
        }
    }

    ClipVertex clipPoints[2];
    u32 refI1 = 0u;
    u32 refI2 = 0u;
    vec2 refV1{0.f, 0.f};
    vec2 refV2{0.f, 0.f};
    vec2 refNormal{0.f, 0.f};
    vec2 sideNormal1{0.f, 0.f};
    vec2 sideNormal2{0.f, 0.f};
    if (primaryAxis.type == AxisType::EdgeA) {
        out.type = ManifoldType::FaceA;
        u32 bestIndex = 0u;
        f32 bestValue = dot(primaryAxis.normal, poly.normals[0]);
        for (u32 i = 1; i < poly.count; ++i) {
            const f32 value = dot(primaryAxis.normal, poly.normals[i]);
            if (value < bestValue) {
                bestValue = value;
                bestIndex = i;
            }
        }
        const u32 i1 = bestIndex;
        const u32 i2 = i1 + 1u < poly.count ? i1 + 1u : 0u;
        clipPoints[0].v = poly.vertices[i1];
        clipPoints[0].id = {0u, toU8(i1), kFeatureFace, kFeatureVertex};
        clipPoints[1].v = poly.vertices[i2];
        clipPoints[1].id = {0u, toU8(i2), kFeatureFace, kFeatureVertex};
        refI1 = 0u;
        refI2 = 1u;
        refV1 = v1;
        refV2 = v2;
        refNormal = primaryAxis.normal;
        sideNormal1 = -edge1;
        sideNormal2 = edge1;
    } else {
        out.type = ManifoldType::FaceB;
        clipPoints[0].v = v2;
        clipPoints[0].id = {1u, toU8(primaryAxis.index), kFeatureVertex, kFeatureFace};
        clipPoints[1].v = v1;
        clipPoints[1].id = {0u, toU8(primaryAxis.index), kFeatureVertex, kFeatureFace};
        refI1 = primaryAxis.index;
        refI2 = refI1 + 1u < poly.count ? refI1 + 1u : 0u;
        refV1 = poly.vertices[refI1];
        refV2 = poly.vertices[refI2];
        refNormal = poly.normals[refI1];
        sideNormal1 = {refNormal.y, -refNormal.x};
        sideNormal2 = -sideNormal1;
    }
    const f32 sideOffset1 = dot(sideNormal1, refV1);
    const f32 sideOffset2 = dot(sideNormal2, refV2);

    ClipVertex clipPoints1[2];
    ClipVertex clipPoints2[2];
    if (clipSegmentToLine(clipPoints1, clipPoints, sideNormal1, sideOffset1, refI1) < 2u) {
        return;
    }
    if (clipSegmentToLine(clipPoints2, clipPoints1, sideNormal2, sideOffset2, refI2) < 2u) {
        return;
    }

    if (primaryAxis.type == AxisType::EdgeA) {
        out.localNormal = refNormal;
        out.localPoint = refV1;
    } else {
        out.localNormal = b.normals[refI1];
        out.localPoint = b.vertices[refI1];
    }
    u32 pointCount = 0u;
    for (u32 i = 0; i < 2u; ++i) {
        const f32 separation = dot(refNormal, clipPoints2[i].v - refV1);
        if (separation <= radius) {
            ManifoldPoint& cp = out.points[pointCount];
            cp.normalImpulse = 0.f;
            cp.tangentImpulse = 0.f;
            if (primaryAxis.type == AxisType::EdgeA) {
                cp.localPoint = mulT(xf, clipPoints2[i].v);
                cp.id = clipPoints2[i].id.key();
            } else {
                cp.localPoint = clipPoints2[i].v;
                cp.id = swapFeature(clipPoints2[i].id).key();
            }
            ++pointCount;
        }
    }
    out.pointCount = pointCount;
}

Aabb2D computeAabb(const Shape2D& shape, const Xf& xf) {
    Aabb2D box;
    if (shape.type == ShapeType2D::Circle) {
        const vec2 p = mul(xf, shape.center);
        box.lower = {p.x - shape.radius, p.y - shape.radius};
        box.upper = {p.x + shape.radius, p.y + shape.radius};
        return box;
    }
    vec2 lower = mul(xf, shape.vertices[0]);
    vec2 upper = lower;
    for (u32 i = 1; i < shape.count; ++i) {
        const vec2 v = mul(xf, shape.vertices[i]);
        lower = vmin(lower, v);
        upper = vmax(upper, v);
    }
    box.lower = {lower.x - shape.radius, lower.y - shape.radius};
    box.upper = {upper.x + shape.radius, upper.y + shape.radius};
    return box;
}

bool rayCast(const Shape2D& shape, const Xf& xf, vec2 p1, vec2 p2, f32 maxFraction, f32& outFraction,
             vec2& outNormal) {
    switch (shape.type) {
    case ShapeType2D::Circle: {
        const vec2 position = mul(xf, shape.center);
        const vec2 s = p1 - position;
        const f32 b = dot(s, s) - shape.radius * shape.radius;
        const vec2 r = p2 - p1;
        const f32 c = dot(s, r);
        const f32 rr = dot(r, r);
        const f32 sigma = c * c - rr * b;
        if (sigma < 0.f || rr < kEpsilon) {
            return false;
        }
        f32 t = -(c + std::sqrt(sigma));
        if (0.f <= t && t <= maxFraction * rr) {
            t /= rr;
            outFraction = t;
            outNormal = normalized(s + t * r);
            return true;
        }
        return false;
    }
    case ShapeType2D::Polygon: {
        const vec2 lp1 = mulT(xf.q, p1 - xf.p);
        const vec2 lp2 = mulT(xf.q, p2 - xf.p);
        const vec2 d = lp2 - lp1;
        f32 lower = 0.f;
        f32 upper = maxFraction;
        bool found = false;
        u32 index = 0u;
        for (u32 i = 0; i < shape.count; ++i) {
            const f32 numerator = dot(shape.normals[i], shape.vertices[i] - lp1);
            const f32 denominator = dot(shape.normals[i], d);
            if (denominator == 0.f) {
                if (numerator < 0.f) {
                    return false;
                }
            } else if (denominator < 0.f && numerator < lower * denominator) {
                lower = numerator / denominator;
                index = i;
                found = true;
            } else if (denominator > 0.f && numerator < upper * denominator) {
                upper = numerator / denominator;
            }
            if (upper < lower) {
                return false;
            }
        }
        if (found) {
            outFraction = lower;
            outNormal = mul(xf.q, shape.normals[index]);
            return true;
        }
        return false;
    }
    case ShapeType2D::Edge: {
        const vec2 lp1 = mulT(xf.q, p1 - xf.p);
        const vec2 lp2 = mulT(xf.q, p2 - xf.p);
        const vec2 d = lp2 - lp1;
        const vec2 v1 = shape.vertices[0];
        const vec2 v2 = shape.vertices[1];
        const vec2 e = v2 - v1;
        const vec2 normal = normalized(vec2{e.y, -e.x});
        const f32 numerator = dot(normal, v1 - lp1);
        if (shape.oneSided && numerator > 0.f) {
            return false; // ray starts behind a one-sided edge
        }
        const f32 denominator = dot(normal, d);
        if (denominator == 0.f) {
            return false;
        }
        const f32 t = numerator / denominator;
        if (t < 0.f || maxFraction < t) {
            return false;
        }
        const vec2 q = lp1 + t * d;
        const f32 rr = dot(e, e);
        if (rr == 0.f) {
            return false;
        }
        const f32 s = dot(q - v1, e) / rr;
        if (s < 0.f || 1.f < s) {
            return false;
        }
        outFraction = t;
        outNormal = numerator > 0.f ? -mul(xf.q, normal) : mul(xf.q, normal);
        return true;
    }
    }
    return false;
}

bool testPoint(const Shape2D& shape, const Xf& xf, vec2 p) {
    if (shape.type == ShapeType2D::Circle) {
        return distanceSq(p, mul(xf, shape.center)) <= shape.radius * shape.radius;
    }
    if (shape.type == ShapeType2D::Edge) {
        return false;
    }
    const vec2 local = mulT(xf, p);
    for (u32 i = 0; i < shape.count; ++i) {
        if (dot(shape.normals[i], local - shape.vertices[i]) > 0.f) {
            return false;
        }
    }
    return true;
}

MassData computeMass(const Shape2D& shape) {
    MassData md;
    if (shape.type == ShapeType2D::Circle) {
        const f32 r2 = shape.radius * shape.radius;
        md.mass = shape.density * 3.14159265359f * r2;
        md.center = shape.center;
        md.inertia = md.mass * (0.5f * r2 + dot(shape.center, shape.center));
        return md;
    }
    if (shape.type == ShapeType2D::Edge || shape.count < 3u) {
        md.center = shape.type == ShapeType2D::Edge ? 0.5f * (shape.vertices[0] + shape.vertices[1]) : shape.center;
        return md;
    }
    vec2 center{0.f, 0.f};
    f32 area = 0.f;
    f32 inertia = 0.f;
    const vec2 s = shape.vertices[0];
    constexpr f32 kInv3 = 1.f / 3.f;
    for (u32 i = 0; i < shape.count; ++i) {
        const vec2 e1 = shape.vertices[i] - s;
        const vec2 e2 = (i + 1u < shape.count ? shape.vertices[i + 1u] : shape.vertices[0]) - s;
        const f32 d = cross(e1, e2);
        const f32 triangleArea = 0.5f * d;
        area += triangleArea;
        center += triangleArea * kInv3 * (e1 + e2);
        const f32 intx2 = e1.x * e1.x + e2.x * e1.x + e2.x * e2.x;
        const f32 inty2 = e1.y * e1.y + e2.y * e1.y + e2.y * e2.y;
        inertia += (0.25f * kInv3 * d) * (intx2 + inty2);
    }
    md.mass = shape.density * area;
    center = (1.f / area) * center;
    md.center = center + s;
    md.inertia = shape.density * inertia + md.mass * (dot(md.center, md.center) - dot(center, center));
    return md;
}

bool buildPolygon(Shape2D& shape, const vec2* points, u32 count) {
    if (points == nullptr || count < 3u) {
        return false;
    }
    // Weld near-duplicate points (at most 32 inputs considered).
    constexpr u32 kMaxInput = 32u;
    vec2 ps[kMaxInput];
    u32 n = 0u;
    for (u32 i = 0; i < count && n < kMaxInput; ++i) {
        bool unique = true;
        for (u32 j = 0; j < n; ++j) {
            if (distanceSq(points[i], ps[j]) < (0.5f * kLinearSlop) * (0.5f * kLinearSlop)) {
                unique = false;
                break;
            }
        }
        if (unique) {
            ps[n++] = points[i];
        }
    }
    if (n < 3u) {
        return false;
    }
    // Andrew's monotone chain (sorted by x then y), CCW, collinear points dropped.
    std::sort(ps, ps + n, [](const vec2& l, const vec2& r) { return l.x < r.x || (l.x == r.x && l.y < r.y); });
    vec2 hull[2u * kMaxInput];
    u32 k = 0u;
    for (u32 i = 0; i < n; ++i) {
        while (k >= 2u && cross(hull[k - 1u] - hull[k - 2u], ps[i] - hull[k - 2u]) <= 0.f) {
            --k;
        }
        hull[k++] = ps[i];
    }
    for (u32 i = n - 1u, t = k + 1u; i > 0u; --i) {
        while (k >= t && cross(hull[k - 1u] - hull[k - 2u], ps[i - 1u] - hull[k - 2u]) <= 0.f) {
            --k;
        }
        hull[k++] = ps[i - 1u];
    }
    const u32 hullCount = k - 1u; // last point repeats the first
    if (hullCount < 3u || hullCount > kMaxPolygonVertices2D) {
        return false;
    }
    shape.type = ShapeType2D::Polygon;
    shape.count = hullCount;
    for (u32 i = 0; i < hullCount; ++i) {
        shape.vertices[i] = hull[i];
    }
    for (u32 i = 0; i < hullCount; ++i) {
        const vec2 e = shape.vertices[i + 1u < hullCount ? i + 1u : 0u] - shape.vertices[i];
        if (lengthSq(e) <= kEpsilon * kEpsilon) {
            return false;
        }
        shape.normals[i] = normalized(cross(e, 1.f));
    }
    // Centroid (area-weighted).
    vec2 c{0.f, 0.f};
    f32 area = 0.f;
    const vec2 s = shape.vertices[0];
    for (u32 i = 0; i < hullCount; ++i) {
        const vec2 e1 = shape.vertices[i] - s;
        const vec2 e2 = (i + 1u < hullCount ? shape.vertices[i + 1u] : shape.vertices[0]) - s;
        const f32 a = 0.5f * cross(e1, e2);
        area += a;
        c += (a / 3.f) * (e1 + e2);
    }
    if (area <= kEpsilon) {
        return false;
    }
    shape.center = (1.f / area) * c + s;
    shape.radius = kPolygonRadius;
    return true;
}

void buildEdge(Shape2D& shape, vec2 v1, vec2 v2) {
    shape.type = ShapeType2D::Edge;
    shape.count = 2u;
    shape.vertices[0] = v1;
    shape.vertices[1] = v2;
    const vec2 n = normalized(cross(v2 - v1, 1.f));
    shape.normals[0] = n;
    shape.normals[1] = -n;
    shape.center = 0.5f * (v1 + v2);
    shape.radius = kPolygonRadius;
    shape.vertex0 = v1;
    shape.vertex3 = v2;
}

} // namespace fuse::physics::p2d
