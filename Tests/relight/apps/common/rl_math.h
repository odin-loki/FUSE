// FUSE Relight test-app kit (RL-0.4): small deterministic matrix helpers (D3DX conventions:
// row vectors, row-major D3DMATRIX layout, left-handed). Only IEEE float + - * / and the
// libm sinf/cosf/tanf/sqrtf of the statically linked CRT are used, so every run of one binary
// produces the same bits.
#pragma once

#include <math.h>
#include <string.h>

namespace rl {

struct Mat4 {
    float m[16]; // _11 _12 _13 _14 _21 ... _44
};
struct Vec3 {
    float x, y, z;
};

inline Mat4 identity()
{
    Mat4 r;
    memset(&r, 0, sizeof r);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

inline Mat4 transpose(const Mat4& a)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = a.m[j * 4 + i];
    return r;
}

inline Mat4 translation(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

inline Mat4 scaling(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
}

inline Mat4 rotationX(float a)
{
    Mat4 r = identity();
    float c = cosf(a), s = sinf(a);
    r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
    return r;
}

inline Mat4 rotationY(float a)
{
    Mat4 r = identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}

inline Mat4 rotationZ(float a)
{
    Mat4 r = identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}

inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(Vec3 a)
{
    float l = sqrtf(dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// D3DXMatrixLookAtLH
inline Mat4 lookAtLH(Vec3 eye, Vec3 at, Vec3 up)
{
    Vec3 z = normalize(sub(at, eye));
    Vec3 x = normalize(cross(up, z));
    Vec3 y = cross(z, x);
    Mat4 r = identity();
    r.m[0] = x.x; r.m[1] = y.x; r.m[2] = z.x;
    r.m[4] = x.y; r.m[5] = y.y; r.m[6] = z.y;
    r.m[8] = x.z; r.m[9] = y.z; r.m[10] = z.z;
    r.m[12] = -dot(x, eye); r.m[13] = -dot(y, eye); r.m[14] = -dot(z, eye);
    return r;
}

// D3DXMatrixPerspectiveFovLH
inline Mat4 perspectiveFovLH(float fovY, float aspect, float zn, float zf)
{
    Mat4 r;
    memset(&r, 0, sizeof r);
    float ys = 1.0f / tanf(fovY * 0.5f);
    r.m[0] = ys / aspect;
    r.m[5] = ys;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}

// D3DXMatrixOrthoOffCenterLH
inline Mat4 orthoOffCenterLH(float l, float r_, float b, float t, float zn, float zf)
{
    Mat4 r = identity();
    r.m[0] = 2.0f / (r_ - l);
    r.m[5] = 2.0f / (t - b);
    r.m[10] = 1.0f / (zf - zn);
    r.m[12] = (l + r_) / (l - r_);
    r.m[13] = (t + b) / (b - t);
    r.m[14] = zn / (zn - zf);
    return r;
}

static const float kPi = 3.14159265358979f;

} // namespace rl

namespace rl {
// Transforms object-space p by the row-vector matrix m (world * view * proj) to a pixel of a
// w x h viewport (D3D: pixel centres at integer + 0.5). Returns false behind the camera.
inline bool projectToPixel(const Mat4& m, Vec3 p, int w, int h, int& px, int& py)
{
    float x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12];
    float y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13];
    float cw = p.x * m.m[3] + p.y * m.m[7] + p.z * m.m[11] + m.m[15];
    if (cw <= 0.0f)
        return false;
    px = int(floorf((x / cw * 0.5f + 0.5f) * float(w)));
    py = int(floorf((0.5f - y / cw * 0.5f) * float(h)));
    return true;
}
} // namespace rl
