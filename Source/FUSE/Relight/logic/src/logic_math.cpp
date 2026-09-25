/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/util/util_matrix.h@0867d3c (decomposeMatrix); the product and inverse are FUSE's own.
#include <fuse/relight/logic/logic_math.hpp>

#include <utility>

namespace fuse::relight::logic {

Matrix4 Matrix4::fromRowMajor(const float* m) {
    Matrix4 r;
    for (std::size_t i = 0; i < 4; ++i) {
        r.data[i] = Vector4(m[i * 4 + 0], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]);
    }
    return r;
}

Matrix4 Matrix4::fromRowMajor(const double* m) {
    Matrix4 r;
    for (std::size_t i = 0; i < 4; ++i) {
        r.data[i] = Vector4(static_cast<float>(m[i * 4 + 0]), static_cast<float>(m[i * 4 + 1]), static_cast<float>(m[i * 4 + 2]),
                            static_cast<float>(m[i * 4 + 3]));
    }
    return r;
}

Vector4 transform(const Matrix4& m, const Vector4& v) {
    return m.data[0] * v.x + m.data[1] * v.y + m.data[2] * v.z + m.data[3] * v.w;
}

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 r;
    for (std::size_t i = 0; i < 4; ++i) {
        r.data[i] = transform(a, b.data[i]);
    }
    return r;
}

Matrix4 inverse(const Matrix4& m) {
    // Gauss-Jordan elimination with partial pivoting, in double.
    double a[4][8];
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            a[i][j] = m.data[i][j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    for (std::size_t c = 0; c < 4; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][c]) < 1e-30) {
            return Matrix4();
        }
        if (pivot != c) {
            for (std::size_t j = 0; j < 8; ++j) {
                std::swap(a[pivot][j], a[c][j]);
            }
        }
        const double inv = 1.0 / a[c][c];
        for (std::size_t j = 0; j < 8; ++j) {
            a[c][j] *= inv;
        }
        for (std::size_t r = 0; r < 4; ++r) {
            if (r == c) {
                continue;
            }
            const double f = a[r][c];
            for (std::size_t j = 0; j < 8; ++j) {
                a[r][j] -= f * a[c][j];
            }
        }
    }
    Matrix4 out;
    for (std::size_t i = 0; i < 4; ++i) {
        out.data[i] = Vector4(static_cast<float>(a[i][4]), static_cast<float>(a[i][5]), static_cast<float>(a[i][6]),
                              static_cast<float>(a[i][7]));
    }
    return out;
}

void decomposeMatrix(const Matrix4& transform, Vector3& position, Vector4& rotation, Vector3& scale) {
    position = Vector3(transform[3][0], transform[3][1], transform[3][2]);
    const Vector3 col0(transform[0][0], transform[0][1], transform[0][2]);
    const Vector3 col1(transform[1][0], transform[1][1], transform[1][2]);
    const Vector3 col2(transform[2][0], transform[2][1], transform[2][2]);
    scale.x = length(col0);
    scale.y = length(col1);
    scale.z = length(col2);

    Vector3 rotationMatrix[3];
    rotationMatrix[0] = scale.x > 0.0f ? col0 / scale.x : Vector3(1.0f, 0.0f, 0.0f);
    rotationMatrix[1] = scale.y > 0.0f ? col1 / scale.y : Vector3(0.0f, 1.0f, 0.0f);
    rotationMatrix[2] = scale.z > 0.0f ? col2 / scale.z : Vector3(0.0f, 0.0f, 1.0f);

    const float trace = rotationMatrix[0][0] + rotationMatrix[1][1] + rotationMatrix[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        rotation.w = 0.25f * s;
        rotation.x = (rotationMatrix[2][1] - rotationMatrix[1][2]) / s;
        rotation.y = (rotationMatrix[0][2] - rotationMatrix[2][0]) / s;
        rotation.z = (rotationMatrix[1][0] - rotationMatrix[0][1]) / s;
    } else if ((rotationMatrix[0][0] > rotationMatrix[1][1]) && (rotationMatrix[0][0] > rotationMatrix[2][2])) {
        const float s = std::sqrt(1.0f + rotationMatrix[0][0] - rotationMatrix[1][1] - rotationMatrix[2][2]) * 2.0f;
        rotation.w = (rotationMatrix[2][1] - rotationMatrix[1][2]) / s;
        rotation.x = 0.25f * s;
        rotation.y = (rotationMatrix[0][1] + rotationMatrix[1][0]) / s;
        rotation.z = (rotationMatrix[0][2] + rotationMatrix[2][0]) / s;
    } else if (rotationMatrix[1][1] > rotationMatrix[2][2]) {
        const float s = std::sqrt(1.0f + rotationMatrix[1][1] - rotationMatrix[0][0] - rotationMatrix[2][2]) * 2.0f;
        rotation.w = (rotationMatrix[0][2] - rotationMatrix[2][0]) / s;
        rotation.x = (rotationMatrix[0][1] + rotationMatrix[1][0]) / s;
        rotation.y = 0.25f * s;
        rotation.z = (rotationMatrix[1][2] + rotationMatrix[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + rotationMatrix[2][2] - rotationMatrix[0][0] - rotationMatrix[1][1]) * 2.0f;
        rotation.w = (rotationMatrix[1][0] - rotationMatrix[0][1]) / s;
        rotation.x = (rotationMatrix[0][2] + rotationMatrix[2][0]) / s;
        rotation.y = (rotationMatrix[1][2] + rotationMatrix[2][1]) / s;
        rotation.z = 0.25f * s;
    }
}

} // namespace fuse::relight::logic
