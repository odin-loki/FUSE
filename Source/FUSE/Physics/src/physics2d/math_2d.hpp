#pragma once

// Small 2D math kit for the rigid-body world (package G11). Operates on fuse::physics::vec2.

#include <fuse/physics/math.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::p2d {

// Tolerances (metres / radians), matching the usual Box2D tuning for metre-scale worlds.
inline constexpr f32 kLinearSlop = 0.005f;
inline constexpr f32 kAngularSlop = 2.f / 180.f * 3.14159265359f;
inline constexpr f32 kPolygonRadius = 2.f * kLinearSlop;
inline constexpr f32 kMaxLinearCorrection = 0.2f;
inline constexpr f32 kMaxAngularCorrection = 8.f / 180.f * 3.14159265359f;
inline constexpr f32 kBaumgarte = 0.2f;
inline constexpr f32 kMaxTranslation = 2.f;
inline constexpr f32 kMaxRotation = 0.5f * 3.14159265359f;
inline constexpr f32 kVelocityThreshold = 1.f;
inline constexpr f32 kAabbMargin = 0.1f;
inline constexpr f32 kEpsilon = 1.1920928955078125e-7f;

inline vec2 operator-(const vec2& v) { return {-v.x, -v.y}; }
inline vec2 operator*(f32 s, const vec2& v) { return {s * v.x, s * v.y}; }
inline vec2& operator+=(vec2& a, const vec2& b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}
inline vec2& operator-=(vec2& a, const vec2& b) {
    a.x -= b.x;
    a.y -= b.y;
    return a;
}

inline f32 dot(const vec2& a, const vec2& b) { return a.x * b.x + a.y * b.y; }
inline f32 cross(const vec2& a, const vec2& b) { return a.x * b.y - a.y * b.x; }
/// cross(v, s) = (s * v.y, -s * v.x)
inline vec2 cross(const vec2& v, f32 s) { return {s * v.y, -s * v.x}; }
/// cross(s, v) = (-s * v.y, s * v.x)
inline vec2 cross(f32 s, const vec2& v) { return {-s * v.y, s * v.x}; }
inline f32 lengthSq(const vec2& v) { return dot(v, v); }
inline f32 length(const vec2& v) { return std::sqrt(dot(v, v)); }
inline f32 distanceSq(const vec2& a, const vec2& b) { return lengthSq(a - b); }
inline vec2 vmin(const vec2& a, const vec2& b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline vec2 vmax(const vec2& a, const vec2& b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline vec2 vabs(const vec2& a) { return {std::fabs(a.x), std::fabs(a.y)}; }

/// Normalises in place and returns the old length (vector untouched when shorter than epsilon).
inline f32 normalize(vec2& v) {
    const f32 len = length(v);
    if (len < kEpsilon) {
        return 0.f;
    }
    const f32 inv = 1.f / len;
    v.x *= inv;
    v.y *= inv;
    return len;
}
inline vec2 normalized(vec2 v) {
    normalize(v);
    return v;
}

struct Rot {
    f32 s = 0.f;
    f32 c = 1.f;
    Rot() = default;
    explicit Rot(f32 angle) : s(std::sin(angle)), c(std::cos(angle)) {}
    f32 angle() const { return std::atan2(s, c); }
};

inline vec2 mul(const Rot& q, const vec2& v) { return {q.c * v.x - q.s * v.y, q.s * v.x + q.c * v.y}; }
inline vec2 mulT(const Rot& q, const vec2& v) { return {q.c * v.x + q.s * v.y, -q.s * v.x + q.c * v.y}; }
/// q^T * r
inline Rot mulT(const Rot& q, const Rot& r) {
    Rot out;
    out.s = q.c * r.s - q.s * r.c;
    out.c = q.c * r.c + q.s * r.s;
    return out;
}
inline Rot mul(const Rot& q, const Rot& r) {
    Rot out;
    out.s = q.s * r.c + q.c * r.s;
    out.c = q.c * r.c - q.s * r.s;
    return out;
}

struct Xf {
    vec2 p{0.f, 0.f};
    Rot q{};
};

inline vec2 mul(const Xf& t, const vec2& v) { return mul(t.q, v) + t.p; }
inline vec2 mulT(const Xf& t, const vec2& v) { return mulT(t.q, v - t.p); }
/// A^T * B: B expressed in A's frame.
inline Xf mulT(const Xf& a, const Xf& b) {
    Xf out;
    out.q = mulT(a.q, b.q);
    out.p = mulT(a.q, b.p - a.p);
    return out;
}

struct Mat22 {
    vec2 ex{1.f, 0.f};
    vec2 ey{0.f, 1.f};
    vec2 solve(const vec2& b) const {
        const f32 a11 = ex.x;
        const f32 a12 = ey.x;
        const f32 a21 = ex.y;
        const f32 a22 = ey.y;
        f32 det = a11 * a22 - a12 * a21;
        if (det != 0.f) {
            det = 1.f / det;
        }
        return {det * (a22 * b.x - a12 * b.y), det * (a11 * b.y - a21 * b.x)};
    }
};

struct Vec3f {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
};

inline f32 dot3(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross3(const Vec3f& a, const Vec3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct Mat33 {
    Vec3f ex{1.f, 0.f, 0.f};
    Vec3f ey{0.f, 1.f, 0.f};
    Vec3f ez{0.f, 0.f, 1.f};
    Vec3f solve33(const Vec3f& b) const {
        f32 det = dot3(ex, cross3(ey, ez));
        if (det != 0.f) {
            det = 1.f / det;
        }
        return {det * dot3(b, cross3(ey, ez)), det * dot3(ex, cross3(b, ez)), det * dot3(ex, cross3(ey, b))};
    }
    vec2 solve22(const vec2& b) const {
        const f32 a11 = ex.x;
        const f32 a12 = ey.x;
        const f32 a21 = ex.y;
        const f32 a22 = ey.y;
        f32 det = a11 * a22 - a12 * a21;
        if (det != 0.f) {
            det = 1.f / det;
        }
        return {det * (a22 * b.x - a12 * b.y), det * (a11 * b.y - a21 * b.x)};
    }
};

} // namespace fuse::physics::p2d
