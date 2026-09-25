/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/util/util_matrix.h@0867d3c, src/d3d9/d3d9_rtx.cpp@0867d3c and
// src/dxso/dxso_compiler.cpp@0867d3c (see back_transform.hpp)

// FUSE Relight RL-1.6: CPU back-transform of captured vertices. See back_transform.hpp.
//
// Modifications (FUSE): the GPU arithmetic of emitVertexCaptureOp runs on the CPU over the clip
// position the SPIR-V pass stores; matrices are flat D3DMATRIX arrays instead of Matrix4.
#include <fuse/relight/capture/vertex_capture/back_transform.hpp>

#include <cmath>

namespace fuse::relight::capture::vertex_capture {

namespace {

using Vector4d = std::array<double, 4>;

Vector4d mul(const Vector4d& a, const Vector4d& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]}; }
Vector4d add(const Vector4d& a, const Vector4d& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]}; }
Vector4d sub(const Vector4d& a, const Vector4d& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]}; }

float at(const Matrix4& m, int r, int c) { return m[std::size_t(r * 4 + c)]; }

} // namespace

std::uint32_t packColor(const float rgba[4]) {
    std::uint32_t c[4];
    for (int i = 0; i < 4; ++i) {
        // FMin(FMax(f, 0), 1) * 255 + 0.5, OpConvertFToU. NaN: FMax returns the other operand on
        // the GPUs Remix targets; the CPU twin maps it to 0.
        float f = rgba[i];
        f = f > 0.0f ? f : 0.0f;
        f = f < 1.0f ? f : 1.0f;
        c[i] = static_cast<std::uint32_t>(f * 255.0f + 0.5f);
    }
    return (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
}

Matrix4 identityMatrix() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Vector4 transform(const Matrix4& m, const Vector4& v) {
    Vector4 out{};
    for (int k = 0; k < 4; ++k) {
        const float mul0 = at(m, 0, k) * v[0];
        const float mul1 = at(m, 1, k) * v[1];
        const float mul2 = at(m, 2, k) * v[2];
        const float mul3 = at(m, 3, k) * v[3];
        const float add0 = mul0 + mul1;
        const float add1 = mul2 + mul3;
        out[std::size_t(k)] = add0 + add1;
    }
    return out;
}

Matrix4 inverse(const Matrix4& fm) {
    auto m = [&](int r, int c) { return double(at(fm, r, c)); };
    const double coef00 = m(2, 2) * m(3, 3) - m(3, 2) * m(2, 3);
    const double coef02 = m(1, 2) * m(3, 3) - m(3, 2) * m(1, 3);
    const double coef03 = m(1, 2) * m(2, 3) - m(2, 2) * m(1, 3);
    const double coef04 = m(2, 1) * m(3, 3) - m(3, 1) * m(2, 3);
    const double coef06 = m(1, 1) * m(3, 3) - m(3, 1) * m(1, 3);
    const double coef07 = m(1, 1) * m(2, 3) - m(2, 1) * m(1, 3);
    const double coef08 = m(2, 1) * m(3, 2) - m(3, 1) * m(2, 2);
    const double coef10 = m(1, 1) * m(3, 2) - m(3, 1) * m(1, 2);
    const double coef11 = m(1, 1) * m(2, 2) - m(2, 1) * m(1, 2);
    const double coef12 = m(2, 0) * m(3, 3) - m(3, 0) * m(2, 3);
    const double coef14 = m(1, 0) * m(3, 3) - m(3, 0) * m(1, 3);
    const double coef15 = m(1, 0) * m(2, 3) - m(2, 0) * m(1, 3);
    const double coef16 = m(2, 0) * m(3, 2) - m(3, 0) * m(2, 2);
    const double coef18 = m(1, 0) * m(3, 2) - m(3, 0) * m(1, 2);
    const double coef19 = m(1, 0) * m(2, 2) - m(2, 0) * m(1, 2);
    const double coef20 = m(2, 0) * m(3, 1) - m(3, 0) * m(2, 1);
    const double coef22 = m(1, 0) * m(3, 1) - m(3, 0) * m(1, 1);
    const double coef23 = m(1, 0) * m(2, 1) - m(2, 0) * m(1, 1);

    const Vector4d fac0{coef00, coef00, coef02, coef03};
    const Vector4d fac1{coef04, coef04, coef06, coef07};
    const Vector4d fac2{coef08, coef08, coef10, coef11};
    const Vector4d fac3{coef12, coef12, coef14, coef15};
    const Vector4d fac4{coef16, coef16, coef18, coef19};
    const Vector4d fac5{coef20, coef20, coef22, coef23};

    const Vector4d vec0{m(1, 0), m(0, 0), m(0, 0), m(0, 0)};
    const Vector4d vec1{m(1, 1), m(0, 1), m(0, 1), m(0, 1)};
    const Vector4d vec2{m(1, 2), m(0, 2), m(0, 2), m(0, 2)};
    const Vector4d vec3{m(1, 3), m(0, 3), m(0, 3), m(0, 3)};

    const Vector4d inv0 = add(sub(mul(vec1, fac0), mul(vec2, fac1)), mul(vec3, fac2));
    const Vector4d inv1 = add(sub(mul(vec0, fac0), mul(vec2, fac3)), mul(vec3, fac4));
    const Vector4d inv2 = add(sub(mul(vec0, fac1), mul(vec1, fac3)), mul(vec3, fac5));
    const Vector4d inv3 = add(sub(mul(vec0, fac2), mul(vec1, fac4)), mul(vec2, fac5));

    const Vector4d signA{+1, -1, +1, -1};
    const Vector4d signB{-1, +1, -1, +1};
    const Vector4d inv[4] = {mul(inv0, signA), mul(inv1, signB), mul(inv2, signA), mul(inv3, signB)};

    const Vector4d row0{inv[0][0], inv[1][0], inv[2][0], inv[3][0]};
    const Vector4d dot0 = mul(Vector4d{m(0, 0), m(0, 1), m(0, 2), m(0, 3)}, row0);
    const double dot1 = (dot0[0] + dot0[1]) + (dot0[2] + dot0[3]);

    Matrix4 out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = static_cast<float>(inv[i / 4][i % 4] / dot1);
    }
    return out;
}

Matrix4 inverseAffine(const Matrix4& m) {
    const double r00 = at(m, 0, 0), r01 = at(m, 0, 1), r02 = at(m, 0, 2);
    const double r10 = at(m, 1, 0), r11 = at(m, 1, 1), r12 = at(m, 1, 2);
    const double r20 = at(m, 2, 0), r21 = at(m, 2, 1), r22 = at(m, 2, 2);

    const double det = r00 * (r11 * r22 - r12 * r21) - r01 * (r10 * r22 - r12 * r20) + r02 * (r10 * r21 - r11 * r20);
    if (std::fabs(det) < 1e-24) {
        return inverse(m);
    }
    const double invDet = 1.0 / det;
    Matrix4 inv{};
    auto set = [&](int r, int c, float v) { inv[std::size_t(r * 4 + c)] = v; };
    set(0, 0, float((r11 * r22 - r12 * r21) * invDet));
    set(0, 1, float((r02 * r21 - r01 * r22) * invDet));
    set(0, 2, float((r01 * r12 - r02 * r11) * invDet));
    set(1, 0, float((r12 * r20 - r10 * r22) * invDet));
    set(1, 1, float((r00 * r22 - r02 * r20) * invDet));
    set(1, 2, float((r02 * r10 - r00 * r12) * invDet));
    set(2, 0, float((r10 * r21 - r11 * r20) * invDet));
    set(2, 1, float((r01 * r20 - r00 * r21) * invDet));
    set(2, 2, float((r00 * r11 - r01 * r10) * invDet));

    const float tx = at(m, 3, 0), ty = at(m, 3, 1), tz = at(m, 3, 2);
    set(3, 0, -(at(inv, 0, 0) * tx + at(inv, 1, 0) * ty + at(inv, 2, 0) * tz));
    set(3, 1, -(at(inv, 0, 1) * tx + at(inv, 1, 1) * ty + at(inv, 2, 1) * tz));
    set(3, 2, -(at(inv, 0, 2) * tx + at(inv, 1, 2) * ty + at(inv, 2, 2) * tz));

    set(0, 3, 0.0f);
    set(1, 3, 0.0f);
    set(2, 3, 0.0f);
    set(3, 3, 1.0f);
    return inv;
}

DrawTransforms drawTransforms(const float world[16], const float view[16], const float projection[16],
                              bool useObjectToWorld) {
    DrawTransforms t;
    auto copy = [](Matrix4& dst, const float* src) {
        if (src) {
            for (std::size_t i = 0; i < 16; ++i) {
                dst[i] = src[i];
            }
        }
    };
    if (useObjectToWorld) {
        copy(t.objectToWorld, world);
    }
    copy(t.worldToView, view);
    copy(t.viewToProjection, projection);
    // DrawCallTransforms::sanitize.
    if (t.objectToWorld[15] == 0.0f) {
        t.objectToWorld[15] = 1.0f;
    }
    if (t.worldToView[15] == 0.0f) {
        t.worldToView[15] = 1.0f;
    }
    return t;
}

BackTransform backTransformFor(const DrawTransforms& t) {
    BackTransform bt;
    bt.invProj = inverse(t.viewToProjection);
    bt.viewToWorld = inverseAffine(t.worldToView);
    bt.worldToObject = inverseAffine(t.objectToWorld);
    bt.normalTransform = t.objectToWorld;
    return bt;
}

std::array<float, 3> clipToObject(const BackTransform& bt, const float clip[4]) {
    const Vector4 viewH = transform(bt.invProj, {clip[0], clip[1], clip[2], clip[3]});
    const Vector4 worldH = transform(bt.viewToWorld, {viewH[0], viewH[1], viewH[2], 1.0f});
    const Vector4 objH = transform(bt.worldToObject, {worldH[0], worldH[1], worldH[2], 1.0f});
    return {objH[0], objH[1], objH[2]};
}

std::array<float, 3> transformNormal(const BackTransform& bt, const float n[3]) {
    std::array<float, 3> out{};
    for (int r = 0; r < 3; ++r) {
        out[std::size_t(r)] = at(bt.normalTransform, r, 0) * n[0] + at(bt.normalTransform, r, 1) * n[1] +
                              at(bt.normalTransform, r, 2) * n[2];
    }
    return out;
}

CapturedVertex backTransform(const BackTransform& bt, const RawCapturedVertex& raw) {
    CapturedVertex v;
    const std::array<float, 3> p = clipToObject(bt, raw.clip);
    v.position[0] = p[0];
    v.position[1] = p[1];
    v.position[2] = p[2];
    if (raw.fields & fields::kTexcoord) {
        v.texcoord0[0] = raw.texcoord0[0];
        v.texcoord0[1] = raw.texcoord0[1];
    }
    if (raw.fields & (fields::kNormalOutput | fields::kNormalInput)) {
        const std::array<float, 3> n = transformNormal(bt, raw.normal0);
        v.normal0[0] = n[0];
        v.normal0[1] = n[1];
        v.normal0[2] = n[2];
    }
    v.color0 = (raw.fields & fields::kColor) ? raw.color0 : 0xFFFFFFFFu;
    return v;
}

} // namespace fuse::relight::capture::vertex_capture
