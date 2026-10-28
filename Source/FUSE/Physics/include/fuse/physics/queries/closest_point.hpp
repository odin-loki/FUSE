#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

/// Closest point to `p` on triangle `abc`, including edges and vertices.
///
/// A non-finite `p` returns `a` when `a` is finite, otherwise the zero vector.
/// A degenerate triangle (zero area, or any non-finite vertex) returns the closest
/// finite vertex, or `a` when every vertex is non-finite.
[[nodiscard]] inline vec3 closest_point_on_triangle(vec3 p, vec3 a, vec3 b, vec3 c) {
    const auto finite = [](vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(p)) {
        return finite(a) ? a : vec3{0.f, 0.f, 0.f};
    }

    const auto closestVertex = [&]() -> vec3 {
        const vec3 verts[3] = {a, b, c};
        vec3 best = a;
        f32 bestDistSq = 0.f;
        bool found = false;
        for (const vec3 v : verts) {
            if (!finite(v)) {
                continue;
            }
            const vec3 delta = v - p;
            const f32 distSq = delta.dot(delta);
            if (!std::isfinite(distSq)) {
                continue;
            }
            if (!found || distSq < bestDistSq) {
                found = true;
                best = v;
                bestDistSq = distSq;
            }
        }
        return found ? best : a;
    };

    if (!finite(a) || !finite(b) || !finite(c)) {
        return closestVertex();
    }

    const vec3 ab = b - a;
    const vec3 ac = c - a;
    const vec3 bc = c - b;
    const f32 abSq = ab.dot(ab);
    const f32 acSq = ac.dot(ac);
    const f32 bcSq = bc.dot(bc);
    f32 maxEdgeSq = abSq > acSq ? abSq : acSq;
    if (bcSq > maxEdgeSq) {
        maxEdgeSq = bcSq;
    }
    const vec3 normal = ab.cross(ac);
    const f32 areaSq = normal.dot(normal);
    const f32 scale = maxEdgeSq * maxEdgeSq;
    // A height below about 1e-6 of the longest edge is not a stable face.
    constexpr f32 kMinAreaSqScale = 1.0e-12f;
    if (!std::isfinite(areaSq) || !std::isfinite(scale) || !(scale > 0.f) || !(areaSq > kMinAreaSqScale * scale)) {
        return closestVertex();
    }

    // Ericson, Real-Time Collision Detection 5.1.5. Vertex regions return the vertex itself.
    const vec3 ap = p - a;
    const f32 d1 = ab.dot(ap);
    const f32 d2 = ac.dot(ap);
    if (d1 <= 0.f && d2 <= 0.f) {
        return a;
    }

    const vec3 bp = p - b;
    const f32 d3 = ab.dot(bp);
    const f32 d4 = ac.dot(bp);
    if (d3 >= 0.f && d4 <= d3) {
        return b;
    }

    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        const f32 abDen = d1 - d3;
        if (abDen > 0.f && std::isfinite(abDen)) {
            const f32 t = d1 / abDen;
            const vec3 q = a + ab * t;
            if (std::isfinite(t) && finite(q)) {
                return q;
            }
        }
        return closestVertex();
    }

    const vec3 cp = p - c;
    const f32 d5 = ab.dot(cp);
    const f32 d6 = ac.dot(cp);
    if (d6 >= 0.f && d5 <= d6) {
        return c;
    }

    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        const f32 acDen = d2 - d6;
        if (acDen > 0.f && std::isfinite(acDen)) {
            const f32 t = d2 / acDen;
            const vec3 q = a + ac * t;
            if (std::isfinite(t) && finite(q)) {
                return q;
            }
        }
        return closestVertex();
    }

    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        const f32 bcDen = (d4 - d3) + (d5 - d6);
        if (bcDen > 0.f && std::isfinite(bcDen)) {
            const f32 t = (d4 - d3) / bcDen;
            const vec3 q = b + bc * t;
            if (std::isfinite(t) && finite(q)) {
                return q;
            }
        }
        return closestVertex();
    }

    const f32 denom = va + vb + vc;
    if (!(denom > 0.f) || !std::isfinite(denom)) {
        return closestVertex();
    }
    const f32 inv = 1.f / denom;
    const vec3 q = a + ab * (vb * inv) + ac * (vc * inv);
    if (!finite(q)) {
        return closestVertex();
    }
    return q;
}

} // namespace fuse::physics
