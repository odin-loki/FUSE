#include <fuse/physics/queries/shape_queries.hpp>

#include <fuse/physics/narrowphase/contact_cluster.hpp>
#include <fuse/physics/narrowphase/mesh_contacts.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>
#include <fuse/physics/spatial/svo.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fuse::physics {

namespace np = narrowphase;

namespace {

constexpr f32 kMaxF = std::numeric_limits<f32>::max();

// ---------------------------------------------------------------------------------------------------
// GJK (Johnson's sub-algorithm via Voronoi regions, Ericson RTCD 9.5).

struct SimplexVertex {
    vec3 w{}; ///< a - b
    vec3 a{};
    vec3 b{};
};

struct Simplex {
    SimplexVertex v[4]{};
    f32 lambda[4]{};
    u32 count = 0;
};

/// Reduces the simplex to the sub-simplex whose convex hull holds the point closest to the origin,
/// with barycentric weights. Returns that closest point.
vec3 closestOnSegment(Simplex& s) {
    const vec3 a = s.v[0].w;
    const vec3 b = s.v[1].w;
    const vec3 ab = b - a;
    const f32 denom = ab.dot(ab);
    const f32 t = denom > 1e-30f ? -a.dot(ab) / denom : 0.f;
    if (t <= 0.f) {
        s.count = 1;
        s.lambda[0] = 1.f;
        return a;
    }
    if (t >= 1.f) {
        s.v[0] = s.v[1];
        s.count = 1;
        s.lambda[0] = 1.f;
        return b;
    }
    s.lambda[0] = 1.f - t;
    s.lambda[1] = t;
    return a + ab * t;
}

vec3 closestOnTriangle(Simplex& s) {
    const SimplexVertex va = s.v[0];
    const SimplexVertex vb = s.v[1];
    const SimplexVertex vc = s.v[2];
    const vec3 a = va.w;
    const vec3 b = vb.w;
    const vec3 c = vc.w;
    const vec3 ab = b - a;
    const vec3 ac = c - a;
    const vec3 ap = a * -1.f;
    const f32 d1 = ab.dot(ap);
    const f32 d2 = ac.dot(ap);
    if (d1 <= 0.f && d2 <= 0.f) {
        s.v[0] = va;
        s.count = 1;
        s.lambda[0] = 1.f;
        return a;
    }
    const vec3 bp = b * -1.f;
    const f32 d3 = ab.dot(bp);
    const f32 d4 = ac.dot(bp);
    if (d3 >= 0.f && d4 <= d3) {
        s.v[0] = vb;
        s.count = 1;
        s.lambda[0] = 1.f;
        return b;
    }
    const f32 vc3 = d1 * d4 - d3 * d2;
    if (vc3 <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        const f32 t = d1 / (d1 - d3);
        s.v[0] = va;
        s.v[1] = vb;
        s.count = 2;
        s.lambda[0] = 1.f - t;
        s.lambda[1] = t;
        return a + ab * t;
    }
    const vec3 cp = c * -1.f;
    const f32 d5 = ab.dot(cp);
    const f32 d6 = ac.dot(cp);
    if (d6 >= 0.f && d5 <= d6) {
        s.v[0] = vc;
        s.count = 1;
        s.lambda[0] = 1.f;
        return c;
    }
    const f32 vb3 = d5 * d2 - d1 * d6;
    if (vb3 <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        const f32 t = d2 / (d2 - d6);
        s.v[0] = va;
        s.v[1] = vc;
        s.count = 2;
        s.lambda[0] = 1.f - t;
        s.lambda[1] = t;
        return a + ac * t;
    }
    const f32 va3 = d3 * d6 - d5 * d4;
    if (va3 <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        const f32 t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        s.v[0] = vb;
        s.v[1] = vc;
        s.count = 2;
        s.lambda[0] = 1.f - t;
        s.lambda[1] = t;
        return b + (c - b) * t;
    }
    const f32 denom = va3 + vb3 + vc3;
    if (std::fabs(denom) < 1e-30f) {
        s.v[0] = va;
        s.count = 1;
        s.lambda[0] = 1.f;
        return a;
    }
    const f32 v = vb3 / denom;
    const f32 w = vc3 / denom;
    s.lambda[0] = 1.f - v - w;
    s.lambda[1] = v;
    s.lambda[2] = w;
    return a + ab * v + ac * w;
}

/// True when the origin and `d` lie on opposite sides of plane abc (or the tetrahedron is flat).
bool originOutside(vec3 a, vec3 b, vec3 c, vec3 d) {
    const vec3 n = (b - a).cross(c - a);
    const f32 signOrigin = (a * -1.f).dot(n);
    const f32 signD = (d - a).dot(n);
    if (signD * signD < 1e-18f) {
        return true;
    }
    return signOrigin * signD < 0.f;
}

vec3 closestOnTetrahedron(Simplex& s, bool& inside) {
    inside = false;
    const SimplexVertex q[4] = {s.v[0], s.v[1], s.v[2], s.v[3]};
    const u32 faces[4][4] = {{0, 1, 2, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {1, 3, 2, 0}};
    f32 best = kMaxF;
    vec3 bestPoint{};
    Simplex bestSimplex{};
    bool anyOutside = false;
    for (const auto& f : faces) {
        if (!originOutside(q[f[0]].w, q[f[1]].w, q[f[2]].w, q[f[3]].w)) {
            continue;
        }
        anyOutside = true;
        Simplex tri{};
        tri.v[0] = q[f[0]];
        tri.v[1] = q[f[1]];
        tri.v[2] = q[f[2]];
        tri.count = 3;
        const vec3 p = closestOnTriangle(tri);
        const f32 d = p.dot(p);
        if (d < best) {
            best = d;
            bestPoint = p;
            bestSimplex = tri;
        }
    }
    if (!anyOutside) {
        inside = true;
        return {};
    }
    s = bestSimplex;
    return bestPoint;
}

vec3 supportA(const ConvexCore& core, vec3 d) {
    return core.support(d);
}

} // namespace

// ---------------------------------------------------------------------------------------------------

vec3 ConvexCore::support(vec3 d) const {
    switch (kind) {
    case Kind::Point:
        return p0;
    case Kind::Segment:
        return p0.dot(d) >= p1.dot(d) ? p0 : p1;
    case Kind::Triangle: {
        const f32 a = p0.dot(d);
        const f32 b = p1.dot(d);
        const f32 c = p2.dot(d);
        return a >= b ? (a >= c ? p0 : p2) : (b >= c ? p1 : p2);
    }
    case Kind::Box: {
        const vec3 local = inverseRotate(orientation, d);
        const vec3 corner{local.x >= 0.f ? halfExtents.x : -halfExtents.x, local.y >= 0.f ? halfExtents.y : -halfExtents.y,
                          local.z >= 0.f ? halfExtents.z : -halfExtents.z};
        return position + rotate(orientation, corner);
    }
    case Kind::Hull: {
        const vec3 local = inverseRotate(orientation, d);
        f32 best = -kMaxF;
        vec3 out{};
        for (u32 i = 0; i < vertexCount; ++i) {
            const f32 dot = vertices[i].dot(local);
            if (dot > best) {
                best = dot;
                out = vertices[i];
            }
        }
        return position + rotate(orientation, out);
    }
    }
    return p0;
}

ConvexCore ConvexCore::moved(vec3 offset) const {
    ConvexCore c = *this;
    c.p0 += offset;
    c.p1 += offset;
    c.p2 += offset;
    c.position += offset;
    return c;
}

GjkDistanceResult gjkDistance(const ConvexCore& a, const ConvexCore& b) {
    GjkDistanceResult r{};
    Simplex s{};
    vec3 dir = a.support({1.f, 0.f, 0.f}) - b.support({-1.f, 0.f, 0.f});
    if (dir.dot(dir) < 1e-20f) {
        dir = {1.f, 0.f, 0.f};
    }
    {
        const vec3 pa = supportA(a, dir * -1.f);
        const vec3 pb = b.support(dir);
        s.v[0] = {pa - pb, pa, pb};
        s.lambda[0] = 1.f;
        s.count = 1;
    }
    vec3 v = s.v[0].w;
    f32 lower = 0.f;
    constexpr u32 kMaxIterations = 64u;
    for (u32 it = 0; it < kMaxIterations; ++it) {
        r.iterations = it + 1u;
        const f32 vv = v.dot(v);
        if (vv < 1e-14f) {
            r.overlap = true;
            break;
        }
        const vec3 pa = a.support(v * -1.f);
        const vec3 pb = b.support(v);
        const vec3 w = pa - pb;
        const f32 vw = v.dot(w);
        // Converged: the support plane does not get closer to the origin.
        if (vv - vw <= 1e-6f * vv + 1e-12f) {
            break;
        }
        bool duplicate = false;
        for (u32 i = 0; i < s.count; ++i) {
            const vec3 d = s.v[i].w - w;
            duplicate = duplicate || d.dot(d) < 1e-14f;
        }
        if (duplicate) {
            break;
        }
        s.v[s.count++] = {w, pa, pb};
        if (s.count == 2u) {
            v = closestOnSegment(s);
        } else if (s.count == 3u) {
            v = closestOnTriangle(s);
        } else {
            bool inside = false;
            v = closestOnTetrahedron(s, inside);
            if (inside) {
                r.overlap = true;
                break;
            }
        }
    }
    if (s.count == 1u) {
        s.lambda[0] = 1.f;
    }
    vec3 pointA{};
    vec3 pointB{};
    for (u32 i = 0; i < s.count; ++i) {
        pointA += s.v[i].a * s.lambda[i];
        pointB += s.v[i].b * s.lambda[i];
    }
    r.pointA = pointA;
    r.pointB = pointB;
    if (r.overlap) {
        r.distance = 0.f;
        r.lowerBound = 0.f;
        return r;
    }
    r.distance = v.length();
    if (r.distance > 0.f) {
        r.direction = v * (1.f / r.distance);
    }
    // The certified bound belongs to the final direction v (conservative advancement steps along it):
    // the support plane of the Minkowski difference in direction -v.
    {
        const vec3 w = a.support(v * -1.f) - b.support(v);
        lower = r.distance > 0.f ? v.dot(w) / r.distance : 0.f;
    }
    r.lowerBound = std::min(std::max(lower, 0.f), r.distance);
    return r;
}

bool convexCoreOf(const np::ShapeInstance& shape, ConvexCore& out) {
    out = ConvexCore{};
    switch (shape.type) {
    case CollisionShapeType::Sphere:
        out.kind = ConvexCore::Kind::Point;
        out.p0 = shape.position;
        out.radius = shape.params.x;
        return true;
    case CollisionShapeType::Capsule: {
        const vec3 half = capsuleHalfAxis(shape.orientation, std::max(shape.params.y, 0.f));
        out.kind = ConvexCore::Kind::Segment;
        out.p0 = shape.position - half;
        out.p1 = shape.position + half;
        out.radius = shape.params.x;
        return true;
    }
    case CollisionShapeType::Box:
        out.kind = ConvexCore::Kind::Box;
        out.position = shape.position;
        out.orientation = shape.orientation;
        out.halfExtents = shape.params;
        return true;
    case CollisionShapeType::ConvexHull: {
        const ConvexHull* hull = ShapePool::global().hull(shape.shapeRef);
        if (hull == nullptr || hull->empty()) {
            return false;
        }
        out.kind = ConvexCore::Kind::Hull;
        out.position = shape.position;
        out.orientation = shape.orientation;
        out.vertices = hull->vertices.data();
        out.vertexCount = static_cast<u32>(hull->vertices.size());
        return true;
    }
    default:
        return false;
    }
}

aabb shapeInstanceBounds(const np::ShapeInstance& shape) {
    const vec3 p = shape.position;
    switch (shape.type) {
    case CollisionShapeType::Sphere:
        return {p - vec3{shape.params.x, shape.params.x, shape.params.x}, p + vec3{shape.params.x, shape.params.x, shape.params.x}};
    case CollisionShapeType::Box: {
        const vec3 h = orientedBoxHalfExtents(shape.orientation, shape.params);
        return {p - h, p + h};
    }
    case CollisionShapeType::Capsule: {
        const vec3 h = orientedCapsuleHalfExtents(shape.orientation, shape.params);
        return {p - h, p + h};
    }
    case CollisionShapeType::Plane: {
        constexpr f32 big = 1e30f;
        return {{-big, -big, -big}, {big, big, big}};
    }
    default:
        return pooledShapeWorldBounds(shape.shapeRef, p, shape.orientation);
    }
}

// ---------------------------------------------------------------------------------------------------
// Ray casts.

namespace {

/// Ray vs sphere; t of the first hit at or after 0.
bool raySphere(vec3 origin, vec3 dir, vec3 center, f32 radius, f32& t) {
    const vec3 m = origin - center;
    const f32 b = m.dot(dir);
    const f32 c = m.dot(m) - radius * radius;
    if (c > 0.f && b > 0.f) {
        return false;
    }
    const f32 disc = b * b - c;
    if (disc < 0.f) {
        return false;
    }
    t = std::max(0.f, -b - std::sqrt(disc));
    return true;
}

bool rayAabb(vec3 origin, vec3 dir, vec3 lo, vec3 hi, f32& t, vec3& normal) {
    f32 tMin = 0.f;
    f32 tMax = kMaxF;
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {dir.x, dir.y, dir.z};
    const f32 mn[3] = {lo.x, lo.y, lo.z};
    const f32 mx[3] = {hi.x, hi.y, hi.z};
    int axis = -1;
    f32 sign = 0.f;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-12f) {
            if (o[a] < mn[a] || o[a] > mx[a]) {
                return false;
            }
            continue;
        }
        f32 t0 = (mn[a] - o[a]) / d[a];
        f32 t1 = (mx[a] - o[a]) / d[a];
        f32 s = -1.f;
        if (t0 > t1) {
            std::swap(t0, t1);
            s = 1.f;
        }
        if (t0 > tMin) {
            tMin = t0;
            axis = a;
            sign = s;
        }
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }
    t = tMin;
    normal = {axis == 0 ? sign : 0.f, axis == 1 ? sign : 0.f, axis == 2 ? sign : 0.f};
    return true;
}

f32 capsuleDistance(vec3 p, vec3 center, vec3 params, const quat& orientation) {
    const vec3 half = capsuleHalfAxis(orientation, params.y);
    const vec3 a = center - half;
    const vec3 ab = half * 2.f;
    const f32 denom = ab.dot(ab);
    const f32 s = denom > 1e-12f ? std::clamp((p - a).dot(ab) / denom, 0.f, 1.f) : 0.f;
    return (p - (a + ab * s)).length() - params.x;
}

bool rayHull(const ConvexHull& hull, vec3 origin, vec3 dir, f32 maxT, f32& t, vec3& normal) {
    f32 tEnter = 0.f;
    f32 tExit = maxT;
    s32 enterFace = -1;
    for (u32 f = 0; f < hull.faces.size(); ++f) {
        const HullFace& face = hull.faces[f];
        const f32 denom = face.normal.dot(dir);
        const f32 dist = face.normal.dot(origin) - face.offset;
        if (std::fabs(denom) < 1e-12f) {
            if (dist > 0.f) {
                return false;
            }
            continue;
        }
        const f32 hit = -dist / denom;
        if (denom < 0.f) {
            if (hit > tEnter) {
                tEnter = hit;
                enterFace = static_cast<s32>(f);
            }
        } else {
            tExit = std::min(tExit, hit);
        }
        if (tEnter > tExit) {
            return false;
        }
    }
    t = tEnter;
    normal = enterFace >= 0 ? hull.faces[static_cast<u32>(enterFace)].normal : dir * -1.f;
    return true;
}

bool raySdf(const SdfSampler& sdf, vec3 origin, vec3 dir, f32 maxT, f32& t, vec3& normal) {
    f32 travelled = 0.f;
    for (int i = 0; i < 256 && travelled <= maxT; ++i) {
        const vec3 p = origin + dir * travelled;
        const f32 d = sdf.distance(p);
        if (d < 1e-4f) {
            t = travelled;
            const vec3 g = sdf.gradient(p);
            normal = g.length() > 1e-12f ? g.normalized() : dir * -1.f;
            return true;
        }
        travelled += d;
    }
    return false;
}

} // namespace

bool rayCastShape(const np::ShapeInstance& shape, vec3 origin, vec3 dir, f32 maxT, RayHit& hit) {
    const vec3 center = shape.position;
    const vec3 params = shape.params;
    const quat rotation = shape.orientation;
    f32 tHit = 0.f;
    vec3 n{};
    bool ok = false;
    switch (shape.type) {
    case CollisionShapeType::Sphere:
        ok = raySphere(origin, dir, center, params.x, tHit);
        n = ok ? (origin + dir * tHit - center).normalized() : n;
        break;
    case CollisionShapeType::Box: {
        // Slab test in the box frame; the hit normal is rotated back to world space.
        const vec3 localOrigin = inverseRotate(rotation, origin - center);
        const vec3 localDir = inverseRotate(rotation, dir);
        ok = rayAabb(localOrigin, localDir, params * -1.f, params, tHit, n);
        n = rotate(rotation, n);
        break;
    }
    case CollisionShapeType::Capsule: {
        // Sphere tracing on the capsule distance (exact distance => never overshoots).
        f32 travelled = 0.f;
        for (int i = 0; i < 96 && travelled <= maxT; ++i) {
            const f32 d = capsuleDistance(origin + dir * travelled, center, params, rotation);
            if (d < 1e-4f) {
                ok = true;
                tHit = travelled;
                const vec3 p = origin + dir * travelled;
                const f32 h = 1e-3f;
                n = vec3{capsuleDistance(p + vec3{h, 0.f, 0.f}, center, params, rotation) -
                             capsuleDistance(p - vec3{h, 0.f, 0.f}, center, params, rotation),
                         capsuleDistance(p + vec3{0.f, h, 0.f}, center, params, rotation) -
                             capsuleDistance(p - vec3{0.f, h, 0.f}, center, params, rotation),
                         capsuleDistance(p + vec3{0.f, 0.f, h}, center, params, rotation) -
                             capsuleDistance(p - vec3{0.f, 0.f, h}, center, params, rotation)}
                        .normalized();
                break;
            }
            travelled += d;
        }
        break;
    }
    case CollisionShapeType::Plane: {
        const f32 denom = params.dot(dir);
        if (std::fabs(denom) > 1e-9f) {
            tHit = (shape.scalar - params.dot(origin)) / denom;
            ok = tHit >= 0.f;
            n = denom < 0.f ? params : params * -1.f;
        }
        break;
    }
    case CollisionShapeType::ConvexHull: {
        const ConvexHull* hull = ShapePool::global().hull(shape.shapeRef);
        if (hull != nullptr) {
            ok = rayHull(*hull, inverseRotate(rotation, origin - center), inverseRotate(rotation, dir), maxT, tHit, n);
            n = rotate(rotation, n);
        }
        break;
    }
    case CollisionShapeType::TriMesh: {
        const TriMesh* mesh = ShapePool::global().mesh(shape.shapeRef);
        TriMeshRayHit meshHit{};
        if (mesh != nullptr &&
            mesh->rayCast(inverseRotate(rotation, origin - center), inverseRotate(rotation, dir), maxT, meshHit)) {
            ok = true;
            tHit = meshHit.t;
            n = rotate(rotation, meshHit.normal);
        }
        break;
    }
    case CollisionShapeType::Voxel: {
        const VoxelVolume* volume = ShapePool::global().voxel(shape.shapeRef);
        if (volume != nullptr && volume->solidCount() > 0u) {
            const vec3 o = inverseRotate(rotation, origin - center);
            const vec3 d = inverseRotate(rotation, dir);
            scene::ivec3 voxel{};
            scene::vec3 normal{};
            f32 distance = 0.f;
            if (volume->svo().rayCast(scene::vec3(o.x, o.y, o.z), scene::vec3(d.x, d.y, d.z), maxT, voxel, normal,
                                      distance)) {
                ok = true;
                tHit = distance;
                const vec3 local{normal.x, normal.y, normal.z};
                n = local.dot(local) > 0.f ? rotate(rotation, local) : dir * -1.f;
            }
        }
        break;
    }
    case CollisionShapeType::SdfMesh: {
        const SdfSampler* sdf = ShapePool::global().sdf(shape.shapeRef);
        if (sdf != nullptr) {
            ok = raySdf(*sdf, inverseRotate(rotation, origin - center), inverseRotate(rotation, dir), maxT, tHit, n);
            n = rotate(rotation, n);
        }
        break;
    }
    }
    if (!ok || tHit > maxT) {
        return false;
    }
    hit.t = tHit;
    hit.normal = n;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Shape casts.

namespace {

struct CastScratch {
    std::vector<u32> triangles;
    std::vector<u32> region;
};

CastScratch& castScratch() {
    thread_local CastScratch s;
    return s;
}

/// Conservative advancement of a convex caster core against one convex target core (translation).
bool convexCast(const ConvexCore& caster, vec3 dir, f32 maxDistance, const ConvexCore& target,
                const ShapeCastOptions& options, ShapeCastHit& hit) {
    const f32 radii = caster.radius + target.radius;
    f32 t = 0.f;
    vec3 previous{};
    bool havePrevious = false;
    for (u32 it = 0; it < options.maxIterations; ++it) {
        const GjkDistanceResult g = gjkDistance(caster.moved(dir * t), target);
        // At (numerically) zero core distance the GJK direction is noise: keep the separating axis the
        // last step advanced along (the contact normal of a translational sweep).
        const bool touchingCores = g.overlap || g.distance < 1e-4f;
        vec3 n = g.direction;
        if (touchingCores && havePrevious) {
            n = previous;
        }
        if (g.overlap) {
            if (!havePrevious) {
                return false; // cores overlap at the start: handled by the caller's overlap test
            }
            hit.distance = t;
            hit.normal = n;
            hit.point = g.pointB;
            return true;
        }
        const f32 gap = g.lowerBound - radii;
        const f32 approach = -n.dot(dir);
        if (gap <= options.tolerance) {
            if (approach <= 1e-6f) {
                return false; // touching but sliding along / moving away: never penetrates
            }
            hit.distance = t;
            hit.normal = n;
            hit.point = g.pointB + n * target.radius;
            return true;
        }
        if (approach <= 1e-6f) {
            return false;
        }
        // The caster cannot touch the target before reaching the separating plane.
        t += gap / approach;
        if (t > maxDistance) {
            return false;
        }
        previous = n;
        havePrevious = true;
    }
    return false;
}

bool castAgainstPlane(const ConvexCore& caster, vec3 dir, f32 maxDistance, vec3 n, f32 d, ShapeCastHit& hit) {
    const vec3 s = caster.support(n * -1.f);
    const f32 gap = n.dot(s) - d - caster.radius;
    const f32 approach = -n.dot(dir);
    if (approach <= 1e-6f) {
        return false;
    }
    const f32 toi = std::max(gap, 0.f) / approach;
    if (toi > maxDistance) {
        return false;
    }
    hit.distance = toi;
    hit.normal = n;
    hit.point = s + dir * toi - n * caster.radius;
    return true;
}

bool castAgainstMesh(const ConvexCore& caster, const aabb& casterStart, vec3 dir, f32 maxDistance,
                     const np::ShapeInstance& target, const ShapeCastOptions& options, ShapeCastHit& hit) {
    const TriMesh* mesh = ShapePool::global().mesh(target.shapeRef);
    if (mesh == nullptr) {
        return false;
    }
    // Swept caster bounds in the mesh frame.
    const vec3 end = dir * maxDistance;
    const vec3 lo{casterStart.min.x + std::min(end.x, 0.f), casterStart.min.y + std::min(end.y, 0.f),
                  casterStart.min.z + std::min(end.z, 0.f)};
    const vec3 hi{casterStart.max.x + std::max(end.x, 0.f), casterStart.max.y + std::max(end.y, 0.f),
                  casterStart.max.z + std::max(end.z, 0.f)};
    const vec3 centre = inverseRotate(target.orientation, (lo + hi) * 0.5f - target.position);
    const vec3 half = orientedBoxHalfExtents(quatConjugate(target.orientation), (hi - lo) * 0.5f) +
                      vec3{options.tolerance, options.tolerance, options.tolerance};
    CastScratch& s = castScratch();
    s.triangles.clear();
    mesh->queryAabb(centre - half, centre + half, [&](u32 t) { s.triangles.push_back(t); });
    bool any = false;
    u32 bestTri = 0;
    for (const u32 t : s.triangles) {
        vec3 a;
        vec3 b;
        vec3 c;
        mesh->triangle(t, a, b, c);
        ConvexCore tri{};
        tri.kind = ConvexCore::Kind::Triangle;
        tri.p0 = target.position + rotate(target.orientation, a);
        tri.p1 = target.position + rotate(target.orientation, b);
        tri.p2 = target.position + rotate(target.orientation, c);
        ShapeCastHit candidate{};
        if (convexCast(caster, dir, maxDistance, tri, options, candidate) && (!any || candidate.distance < hit.distance)) {
            hit = candidate;
            any = true;
            bestTri = t;
        }
    }
    if (any) {
        // Internal-edge fix: a hit on an edge / vertex shared with a coplanar or concave neighbour takes
        // the face normal.
        vec3 a;
        vec3 b;
        vec3 c;
        mesh->triangle(bestTri, a, b, c);
        const vec3 local = inverseRotate(target.orientation, hit.point - target.position);
        u32 region = 6;
        np::closestPointOnTriangle(local, a, b, c, &region);
        const u8 flags = mesh->edgeFlags(bestTri);
        const auto real = [&](u32 e) { return (flags & (1u << e)) != 0u; };
        const bool internal = (region == 3u && !real(0u)) || (region == 4u && !real(1u)) || (region == 5u && !real(2u)) ||
                              (region == 0u && !real(2u) && !real(0u)) || (region == 1u && !real(0u) && !real(1u)) ||
                              (region == 2u && !real(1u) && !real(2u));
        if (internal) {
            vec3 n = rotate(target.orientation, mesh->triangleNormal(bestTri));
            if (n.dot(hit.normal) < 0.f) {
                n = n * -1.f;
            }
            hit.normal = n;
        }
    }
    return any;
}

bool castAgainstVoxels(const ConvexCore& caster, const aabb& casterStart, vec3 dir, f32 maxDistance,
                       const np::ShapeInstance& target, const ShapeCastOptions& options, ShapeCastHit& hit) {
    const VoxelVolume* volume = ShapePool::global().voxel(target.shapeRef);
    if (volume == nullptr) {
        return false;
    }
    const vec3 end = dir * maxDistance;
    const vec3 lo{casterStart.min.x + std::min(end.x, 0.f), casterStart.min.y + std::min(end.y, 0.f),
                  casterStart.min.z + std::min(end.z, 0.f)};
    const vec3 hi{casterStart.max.x + std::max(end.x, 0.f), casterStart.max.y + std::max(end.y, 0.f),
                  casterStart.max.z + std::max(end.z, 0.f)};
    const vec3 centre = inverseRotate(target.orientation, (lo + hi) * 0.5f - target.position);
    const vec3 half = orientedBoxHalfExtents(quatConjugate(target.orientation), (hi - lo) * 0.5f);
    const ivec3 dims = volume->dims();
    const ivec3 a = volume->voxelAt(centre - half);
    const ivec3 b = volume->voxelAt(centre + half);
    const ivec3 v0{std::max(a.x, 0), std::max(a.y, 0), std::max(a.z, 0)};
    const ivec3 v1{std::min(b.x, dims.x - 1), std::min(b.y, dims.y - 1), std::min(b.z, dims.z - 1)};
    if (v0.x > v1.x || v0.y > v1.y || v0.z > v1.z) {
        return false;
    }
    const ivec3 r0{v0.x - 1, v0.y - 1, v0.z - 1};
    const ivec3 size{v1.x - v0.x + 3, v1.y - v0.y + 3, v1.z - v0.z + 3};
    CastScratch& s = castScratch();
    volume->svo().readBox(scene::ivec3(r0.x, r0.y, r0.z), scene::ivec3(size.x, size.y, size.z), s.region);
    const auto solid = [&](s32 x, s32 y, s32 z) {
        const s32 lx = x - r0.x;
        const s32 ly = y - r0.y;
        const s32 lz = z - r0.z;
        if (x < 0 || y < 0 || z < 0 || x >= dims.x || y >= dims.y || z >= dims.z) {
            return false;
        }
        return s.region[(static_cast<usize>(lz) * static_cast<usize>(size.y) + static_cast<usize>(ly)) *
                            static_cast<usize>(size.x) +
                        static_cast<usize>(lx)] != 0u;
    };
    const f32 h = volume->voxelSize() * 0.5f;
    bool any = false;
    ivec3 bestVoxel{};
    for (s32 z = v0.z; z <= v1.z; ++z) {
        for (s32 y = v0.y; y <= v1.y; ++y) {
            for (s32 x = v0.x; x <= v1.x; ++x) {
                if (!solid(x, y, z) || (solid(x + 1, y, z) && solid(x - 1, y, z) && solid(x, y + 1, z) &&
                                        solid(x, y - 1, z) && solid(x, y, z + 1) && solid(x, y, z - 1))) {
                    continue;
                }
                ConvexCore box{};
                box.kind = ConvexCore::Kind::Box;
                box.position = target.position + rotate(target.orientation, volume->voxelCenter({x, y, z}));
                box.orientation = target.orientation;
                box.halfExtents = {h, h, h};
                ShapeCastHit candidate{};
                if (convexCast(caster, dir, maxDistance, box, options, candidate) &&
                    (!any || candidate.distance < hit.distance)) {
                    hit = candidate;
                    any = true;
                    bestVoxel = {x, y, z};
                }
            }
        }
    }
    if (any) {
        // An internal voxel face (solid neighbour across it) takes the occupancy-gradient normal.
        const vec3 local = inverseRotate(target.orientation, hit.normal);
        ivec3 nb = bestVoxel;
        const f32 ax = std::fabs(local.x);
        const f32 ay = std::fabs(local.y);
        const f32 az = std::fabs(local.z);
        if (ax >= ay && ax >= az) {
            nb.x += local.x > 0.f ? 1 : -1;
        } else if (ay >= az) {
            nb.y += local.y > 0.f ? 1 : -1;
        } else {
            nb.z += local.z > 0.f ? 1 : -1;
        }
        if (solid(nb.x, nb.y, nb.z)) {
            const vec3 g = volume->occupancyNormal(bestVoxel);
            if (g.length() > 1e-4f) {
                hit.normal = rotate(target.orientation, g.normalized());
            }
        }
    }
    return any;
}

/// Sphere tracing of the caster's sample points through an SDF.
bool castAgainstSdf(const np::ShapeInstance& caster, vec3 dir, f32 maxDistance, const np::ShapeInstance& target,
                    const ShapeCastOptions& options, ShapeCastHit& hit) {
    const SdfSampler* sdf = ShapePool::global().sdf(target.shapeRef);
    if (sdf == nullptr) {
        return false;
    }
    // Caster samples in the field frame: (point, radius).
    vec3 points[26];
    f32 radius = 0.f;
    u32 count = 0;
    const vec3 origin = inverseRotate(target.orientation, caster.position - target.position);
    const quat orientation = quatMul(quatConjugate(target.orientation), caster.orientation);
    const vec3 d = inverseRotate(target.orientation, dir);
    switch (caster.type) {
    case CollisionShapeType::Sphere:
        points[count++] = origin;
        radius = caster.params.x;
        break;
    case CollisionShapeType::Capsule: {
        const vec3 half = capsuleHalfAxis(orientation, std::max(caster.params.y, 0.f));
        for (u32 i = 0; i < 9u; ++i) {
            points[count++] = origin - half + half * (2.f * static_cast<f32>(i) / 8.f);
        }
        radius = caster.params.x;
        break;
    }
    case CollisionShapeType::Box:
        for (s32 i = -1; i <= 1; ++i) {
            for (s32 j = -1; j <= 1; ++j) {
                for (s32 k = -1; k <= 1; ++k) {
                    if (i != 0 || j != 0 || k != 0) {
                        points[count++] = origin + rotate(orientation, {caster.params.x * static_cast<f32>(i),
                                                                        caster.params.y * static_cast<f32>(j),
                                                                        caster.params.z * static_cast<f32>(k)});
                    }
                }
            }
        }
        break;
    default:
        return false;
    }
    f32 t = 0.f;
    for (u32 it = 0; it < 4u * options.maxIterations && t <= maxDistance; ++it) {
        f32 best = kMaxF;
        u32 bestIndex = 0;
        for (u32 i = 0; i < count; ++i) {
            const f32 dist = sdf->distance(points[i] + d * t) - radius;
            if (dist < best) {
                best = dist;
                bestIndex = i;
            }
        }
        if (best <= options.tolerance) {
            const vec3 p = points[bestIndex] + d * t;
            const vec3 g = sdf->gradient(p);
            const vec3 n = g.length() > 1e-12f ? g.normalized() : d * -1.f;
            if (n.dot(d) >= -1e-6f && it > 0u) {
                t += options.tolerance; // grazing: keep going
                continue;
            }
            hit.distance = t;
            hit.normal = rotate(target.orientation, n);
            hit.point = target.position + rotate(target.orientation, p - n * (radius + best));
            return true;
        }
        t += best;
    }
    return false;
}

} // namespace

bool shapeCast(const np::ShapeInstance& caster, vec3 direction, f32 maxDistance, const np::ShapeInstance& target,
               ShapeCastHit& hit, const ShapeCastOptions& options) {
    if (maxDistance < 0.f || direction.dot(direction) < 1e-12f) {
        return false;
    }
    const vec3 dir = direction.normalized();
    ConvexCore core{};
    if (!convexCoreOf(caster, core) || caster.type == CollisionShapeType::ConvexHull) {
        return false; // sphere, capsule and box casters
    }

    // Already overlapping at the start pose: report the penetration (distance 0).
    np::ContactManifold manifolds[np::kMaxManifoldsPerPair];
    const u32 overlaps = np::collideShapesMulti(caster, target, 0u, 1u, 0.f, manifolds, np::kMaxManifoldsPerPair);
    f32 deepest = options.tolerance;
    for (u32 m = 0; m < overlaps; ++m) {
        const f32 depth = manifolds[m].maxPenetration();
        if (manifolds[m].valid && depth > deepest) {
            deepest = depth;
            hit.distance = 0.f;
            hit.normal = manifolds[m].contactNormal;
            hit.point = manifolds[m].contactPoint;
            hit.penetration = depth;
            hit.startPenetrating = true;
        }
    }
    if (deepest > options.tolerance) {
        return true;
    }

    hit = ShapeCastHit{};
    switch (target.type) {
    case CollisionShapeType::Plane: {
        const f32 len = target.params.length();
        if (len < 1e-12f) {
            return false;
        }
        return castAgainstPlane(core, dir, maxDistance, target.params * (1.f / len), target.scalar / len, hit);
    }
    case CollisionShapeType::Sphere:
    case CollisionShapeType::Capsule:
    case CollisionShapeType::Box:
    case CollisionShapeType::ConvexHull: {
        ConvexCore other{};
        if (!convexCoreOf(target, other)) {
            return false;
        }
        return convexCast(core, dir, maxDistance, other, options, hit);
    }
    case CollisionShapeType::TriMesh:
        return castAgainstMesh(core, shapeInstanceBounds(caster), dir, maxDistance, target, options, hit);
    case CollisionShapeType::Voxel:
        return castAgainstVoxels(core, shapeInstanceBounds(caster), dir, maxDistance, target, options, hit);
    case CollisionShapeType::SdfMesh:
        return castAgainstSdf(caster, dir, maxDistance, target, options, hit);
    }
    return false;
}

} // namespace fuse::physics
