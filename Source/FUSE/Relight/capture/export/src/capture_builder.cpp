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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_game_capturer.cpp@0867d3c, src/lssusd/game_exporter.cpp@0867d3c,
// src/dxvk/shaders/rtx/pass/gen_tri_list_index_buffer.h@0867d3c, src/lssusd/mdl_helpers.h@0867d3c and
// src/dxvk/rtx_render/rtx_lights.cpp@0867d3c (see capture_builder.hpp).
// FUSE Relight RL-1.8: GameCapturer.
#include <fuse/relight/capture/export/capture_builder.hpp>

#include <fuse/relight/hash/hash_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <type_traits>

namespace fuse::relight::capture::exporter {

namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr std::uint32_t kVkTriangleList = 3, kVkTriangleStrip = 4, kVkTriangleFan = 5;
constexpr std::uint32_t kD3DCullNone = 1;

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000) << 16;
    std::uint32_t exp = (h >> 10) & 0x1f;
    std::uint32_t mant = h & 0x3ff;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

template <typename T>
T load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

float snorm(float v, float maxv) { return std::max(v / maxv, -1.0f); }

Mat4d toMat4d(const std::array<float, 16>& m) {
    Mat4d d{};
    for (std::size_t i = 0; i < 16; ++i) {
        d[i] = static_cast<double>(m[i]);
    }
    return d;
}

std::array<double, 3> cross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const std::array<double, 3>& a, const std::array<double, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::array<double, 3> normalize(const std::array<double, 3>& v) {
    const double l = std::sqrt(dot(v, v));
    return l > 0.0 ? std::array<double, 3>{v[0] / l, v[1] / l, v[2] / l} : v;
}

/// util_matrix.h isMirrorTransform on the upper 3x3 rows.
bool isMirrorTransform(const std::array<double, 16>& m) {
    const std::array<double, 3> x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    return dot(cross(x, y), z) < 0.0;
}

/// GfRotation::SetRotateInto(from, to) as a 3x3 row-vector matrix.
std::array<double, 9> rotateInto(std::array<double, 3> from, std::array<double, 3> to) {
    from = normalize(from);
    to = normalize(to);
    const double c = dot(from, to);
    std::array<double, 3> axis = cross(from, to);
    double s = std::sqrt(dot(axis, axis));
    if (s < 1e-12) {
        if (c > 0.0) {
            return {1, 0, 0, 0, 1, 0, 0, 0, 1};
        }
        // Opposite vectors: 180 degrees about any axis perpendicular to `from`.
        axis = cross(from, {1.0, 0.0, 0.0});
        if (dot(axis, axis) < 1e-12) {
            axis = cross(from, {0.0, 1.0, 0.0});
        }
        axis = normalize(axis);
        s = 0.0;
    } else {
        axis = {axis[0] / s, axis[1] / s, axis[2] / s};
    }
    const double x = axis[0], y = axis[1], z = axis[2], t = 1.0 - c;
    // Column-vector rotation R; the row-vector matrix is R^T.
    const double R[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                         t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                         t * x * z - s * y, t * y * z + s * x, t * z * z + c};
    return {R[0], R[3], R[6], R[1], R[4], R[7], R[2], R[5], R[8]};
}

} // namespace

// ---- helpers --------------------------------------------------------------------------------------------

Mat4d multiply(const Mat4d& a, const Mat4d& b) {
    Mat4d r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                s += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = s;
        }
    }
    return r;
}

Mat4d inverse(const Mat4d& m) {
    Mat4d inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0) {
        return identity4d();
    }
    for (double& v : inv) {
        v /= det;
    }
    return inv;
}

Mat4d rotationBetween(const Vec3f& from, const Vec3f& to, const Vec3f& t) {
    const auto r = rotateInto({from[0], from[1], from[2]}, {to[0], to[1], to[2]});
    Mat4d m = identity4d();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i * 4 + j] = r[i * 3 + j];
        }
    }
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}

std::uint32_t mdlWrapMode(std::uint32_t d3dAddressMode) {
    // D3DTADDRESS_WRAP 1 -> REPEAT, MIRROR 2 -> MIRRORED_REPEAT, CLAMP 3 -> CLAMP_TO_EDGE, BORDER 4 ->
    // CLAMP_TO_BORDER, MIRRORONCE 5 -> MIRROR_CLAMP_TO_EDGE (vkToMdl: default Repeat).
    switch (d3dAddressMode) {
    case 1: return mdl::kWrapRepeat;
    case 2: return mdl::kWrapMirroredRepeat;
    case 3: return mdl::kWrapClamp;
    case 4: return mdl::kWrapClip;
    default: return mdl::kWrapRepeat;
    }
}

std::uint32_t mdlFilter(std::uint32_t d3dTextureFilter) {
    // DecodeFilter: D3DTEXF_NONE / POINT -> NEAREST, everything else -> LINEAR.
    return d3dTextureFilter <= 1 ? mdl::kFilterNearest : mdl::kFilterLinear;
}

Vec4f readAttribute(const geometry::VertexAttribute& a, std::uint32_t vertex) {
    Vec4f v{0.f, 0.f, 0.f, 1.f};
    if (!a.defined()) {
        return v;
    }
    const std::uint8_t* p = a.base() + std::size_t(vertex) * a.stride;
    using T = hash::D3DDeclType;
    switch (a.type) {
    case T::Float4: v[3] = load<float>(p + 12); [[fallthrough]];
    case T::Float3: v[2] = load<float>(p + 8); [[fallthrough]];
    case T::Float2: v[1] = load<float>(p + 4); [[fallthrough]];
    case T::Float1: v[0] = load<float>(p); break;
    case T::D3DColor: v = {p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f}; break;
    case T::UByte4: v = {float(p[0]), float(p[1]), float(p[2]), float(p[3])}; break;
    case T::UByte4N: v = {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f}; break;
    case T::Short2: v = {float(load<std::int16_t>(p)), float(load<std::int16_t>(p + 2)), 0.f, 1.f}; break;
    case T::Short4:
        v = {float(load<std::int16_t>(p)), float(load<std::int16_t>(p + 2)), float(load<std::int16_t>(p + 4)),
             float(load<std::int16_t>(p + 6))};
        break;
    case T::Short2N: v = {snorm(load<std::int16_t>(p), 32767.f), snorm(load<std::int16_t>(p + 2), 32767.f), 0.f, 1.f}; break;
    case T::Short4N:
        v = {snorm(load<std::int16_t>(p), 32767.f), snorm(load<std::int16_t>(p + 2), 32767.f),
             snorm(load<std::int16_t>(p + 4), 32767.f), snorm(load<std::int16_t>(p + 6), 32767.f)};
        break;
    case T::UShort2N: v = {load<std::uint16_t>(p) / 65535.f, load<std::uint16_t>(p + 2) / 65535.f, 0.f, 1.f}; break;
    case T::UShort4N:
        v = {load<std::uint16_t>(p) / 65535.f, load<std::uint16_t>(p + 2) / 65535.f, load<std::uint16_t>(p + 4) / 65535.f,
             load<std::uint16_t>(p + 6) / 65535.f};
        break;
    case T::UDec3: {
        const std::uint32_t u = load<std::uint32_t>(p);
        v = {float(u & 1023), float((u >> 10) & 1023), float((u >> 20) & 1023), 1.f};
        break;
    }
    case T::Dec3N: {
        const std::uint32_t u = load<std::uint32_t>(p);
        auto s10 = [](std::uint32_t x) { return snorm(float(std::int32_t(x << 22) >> 22), 511.f); };
        v = {s10(u & 1023), s10((u >> 10) & 1023), s10((u >> 20) & 1023), 1.f};
        break;
    }
    case T::Float16_2: v = {halfToFloat(load<std::uint16_t>(p)), halfToFloat(load<std::uint16_t>(p + 2)), 0.f, 1.f}; break;
    case T::Float16_4:
        v = {halfToFloat(load<std::uint16_t>(p)), halfToFloat(load<std::uint16_t>(p + 2)), halfToFloat(load<std::uint16_t>(p + 4)),
             halfToFloat(load<std::uint16_t>(p + 6))};
        break;
    case T::Unused: break;
    }
    return v;
}

std::vector<std::int32_t> triangleListIndices(std::uint32_t topology, const geometry::RebasedIndices* indices,
                                              std::uint32_t vertexCount) {
    // getOptimalTriangleListSize + generateIndices (minVertex 0, maxVertex vertexCount - 1).
    const bool useIndexBuffer = indices != nullptr && indices->indexCount() > 0;
    const std::uint32_t primCount = useIndexBuffer ? indices->indexCount() : vertexCount;
    std::uint32_t indexCount = 0;
    if (topology == kVkTriangleList) {
        indexCount = primCount;
    } else if ((topology == kVkTriangleStrip || topology == kVkTriangleFan) && primCount >= 3) {
        indexCount = (primCount - 2) * 3;
    }
    const std::uint32_t primIterCount = indexCount / 3;
    const std::uint32_t maxVertex = vertexCount > 0 ? vertexCount - 1 : 0;
    std::vector<std::int32_t> out;
    out.reserve(std::size_t(primIterCount) * 3);
    for (std::uint32_t idx = 0; idx < primIterCount; ++idx) {
        std::uint32_t i0, i1, i2;
        if (topology == kVkTriangleFan) {
            i0 = 0;
            i1 = idx + 1;
            i2 = idx + 2;
        } else if (topology == kVkTriangleStrip) {
            i0 = idx;
            i1 = idx + 1 + (idx & 1);
            i2 = idx + 2 - (idx & 1);
        } else {
            i0 = idx * 3;
            i1 = idx * 3 + 1;
            i2 = idx * 3 + 2;
        }
        if (useIndexBuffer) {
            std::uint32_t a = indices->at(i0), b = indices->at(i1), c = indices->at(i2);
            if (a == b || a == c || b == c || a > maxVertex || b > maxVertex || c > maxVertex) {
                a = b = c = 0; // degenerate or invalid: collapsed onto minVertex
            }
            out.push_back(std::int32_t(a));
            out.push_back(std::int32_t(b));
            out.push_back(std::int32_t(c));
        } else {
            out.push_back(std::int32_t(i0));
            out.push_back(std::int32_t(i1));
            out.push_back(std::int32_t(i2));
        }
    }
    return out;
}

std::vector<std::int32_t> reduceIndices(const std::vector<std::int32_t>& indices, std::vector<std::int32_t>& used) {
    const std::set<std::int32_t> ordered(indices.begin(), indices.end());
    std::map<std::int32_t, std::int32_t> ogToRed;
    used.assign(ordered.begin(), ordered.end());
    std::int32_t next = 0;
    for (const std::int32_t i : ordered) {
        ogToRed[i] = next++;
    }
    std::vector<std::int32_t> out;
    out.reserve(indices.size());
    for (const std::int32_t i : indices) {
        out.push_back(ogToRed[i]);
    }
    return out;
}

CaptureSkeleton generateSkeleton(const CaptureMesh& mesh) {
    CaptureSkeleton out;
    const std::size_t numBones = mesh.numBones;
    const std::size_t bpv = std::max<std::uint32_t>(1u, mesh.bonesPerVertex);
    out.bindPose.assign(numBones, identity4d());
    out.restPose.assign(numBones, identity4d());
    out.jointNames.resize(numBones);
    std::vector<std::array<double, 3>> sums(numBones, {0.0, 0.0, 0.0});
    std::vector<double> totals(numBones, 0.0);
    const float equalBlend = 1.f / float(bpv);
    for (std::size_t i = 0; i < mesh.points.size(); ++i) {
        for (std::size_t j = 0; j < bpv; ++j) {
            const float w = mesh.jointWeights.empty() ? equalBlend : mesh.jointWeights[i * bpv + j];
            if (w > 0.00001f) {
                const std::size_t ind = mesh.jointIndices.empty() ? j : std::size_t(mesh.jointIndices[i * bpv + j]);
                if (ind < numBones) {
                    for (int c = 0; c < 3; ++c) {
                        sums[ind][c] += double(mesh.points[i][c]) * w;
                    }
                    totals[ind] += w;
                }
            }
        }
    }
    auto translate = [](const std::array<double, 3>& t) {
        Mat4d m = identity4d();
        m[12] = t[0];
        m[13] = t[1];
        m[14] = t[2];
        return m;
    };
    std::array<double, 3> rootBindPos{0.0, 0.0, 0.0};
    if (numBones > 0) {
        if (totals[0] != 0.0) {
            rootBindPos = {sums[0][0] / totals[0], sums[0][1] / totals[0], sums[0][2] / totals[0]};
            out.bindPose[0] = translate(rootBindPos);
        }
        out.restPose[0] = out.bindPose[0];
        for (std::size_t i = 1; i < numBones; ++i) {
            if (totals[i] != 0.0) {
                const std::array<double, 3> c{sums[i][0] / totals[i], sums[i][1] / totals[i], sums[i][2] / totals[i]};
                out.bindPose[i] = translate(c);
                out.restPose[i] = translate({c[0] - rootBindPos[0], c[1] - rootBindPos[1], c[2] - rootBindPos[2]});
            }
        }
        out.jointNames[0] = "root";
        // Upstream str::format("root/joint", i) concatenates the index.
        for (std::size_t i = 1; i < numBones; ++i) {
            out.jointNames[i] = "root/joint" + std::to_string(i);
        }
    }
    return out;
}

std::vector<Mat4d> sanitizeBoneXforms(const std::vector<Mat4d>& xforms, const std::vector<Mat4d>& bindPose) {
    const std::size_t numBones = std::min(xforms.size(), bindPose.size());
    std::vector<Mat4d> out(numBones, identity4d());
    Mat4d worldFromRoot = identity4d();
    if (numBones > 0) {
        const Mat4d rootFromWorld = multiply(bindPose[0], xforms[0]);
        worldFromRoot = inverse(rootFromWorld);
        out[0] = rootFromWorld;
    }
    for (std::size_t i = 1; i < numBones; ++i) {
        out[i] = multiply(multiply(bindPose[i], xforms[i]), worldFromRoot);
    }
    return out;
}

// ---- GameCapturer ---------------------------------------------------------------------------------------

GameCapturer::GameCapturer(CaptureOptions options, TextureSource textures)
    : m_options(std::move(options)), m_textureSource(std::move(textures)) {
    m_cap.meta = m_options.meta;
    m_cap.meta.geometryHashRule = m_options.assetRuleString;
}

bool GameCapturer::captureFrame(const CaptureFrame& frame) {
    if (m_options.maxFrames != 0 && m_frames >= m_options.maxFrames) {
        return false;
    }
    m_currentTime = static_cast<double>(m_frames);
    if (m_options.captureInstances) {
        if (frame.mainCamera) {
            captureCamera(*frame.mainCamera);
        }
        captureLights(frame.lights);
    }
    std::unordered_set<std::uint64_t> seen;
    for (const CaptureDraw& d : frame.draws) {
        captureInstance(d, seen);
    }
    ++m_frames;
    return true;
}

void GameCapturer::captureCamera(const scene::CameraState& sceneCamera) {
    CaptureCamera& cam = m_cap.camera;
    if (!cam.valid) {
        // Step 1: the projection's decomposition (RL-1.5 CameraState holds decomposeProjection's result).
        cam.valid = true;
        cam.fov = sceneCamera.fov;
        cam.aspectRatio = sceneCamera.aspectRatio;
        cam.nearPlane = sceneCamera.nearPlane;
        constexpr float kMaxFarPlane = 100000000.f; // USD does not take infinite projections
        cam.farPlane = std::min(sceneCamera.farPlane, kMaxFarPlane);
        cam.isReverseZ = sceneCamera.isReverseZ;
        // Step 2: handedness, orientation, inversion.
        cam.projLhs = sceneCamera.isLHS;
        const auto& p = sceneCamera.viewToProjection;
        cam.projInv = p[0] * p[5] < 0.0f;
        const auto& v2w = sceneCamera.viewToWorld;
        if (cam.projInv) {
            const std::array<double, 3> up{v2w[4], v2w[5], v2w[6]};
            const std::array<double, 3> worldUp = m_options.meta.isZUp ? std::array<double, 3>{0.0, 0.0, 1.0}
                                                                       : std::array<double, 3>{0.0, 1.0, 0.0};
            cam.viewInv = dot(up, worldUp) < 0.0;
        }
        cam.viewLhs = isMirrorTransform(v2w);
        cam.firstTime = m_currentTime;
    }
    cam.finalTime = m_currentTime;

    // Step 3: the capture camera.
    const auto& v2w = sceneCamera.viewToWorld;
    const auto pos = sceneCamera.position();
    const auto dir = sceneCamera.direction();
    std::array<double, 3> position{pos[0], pos[1], pos[2]};
    std::array<double, 3> direction{dir[0], dir[1], dir[2]};
    std::array<double, 3> up{v2w[4], v2w[5], v2w[6]};
    if (cam.viewInv) {
        up = {-up[0], -up[1], -up[2]};
    }
    if (cam.projInv && !cam.viewInv) {
        position[0] *= -1.0;
        position[1] *= -1.0;
        direction[0] *= -1.0;
        direction[1] *= -1.0;
        up[0] *= -1.0;
    } else if (cam.isLHS() && !cam.viewInv) {
        position[0] *= -1.0;
        direction[0] *= -1.0;
    }
    // worldToView.SetLookAt(position, position + direction, up); the capture keeps its inverse.
    const std::array<double, 3> f = normalize(direction);
    const std::array<double, 3> s = normalize(cross(f, normalize(up)));
    const std::array<double, 3> u = cross(s, f);
    Mat4d viewToWorld = identity4d();
    for (int k = 0; k < 3; ++k) {
        viewToWorld[0 + k] = s[k];
        viewToWorld[4 + k] = u[k];
        viewToWorld[8 + k] = -f[k];
        viewToWorld[12 + k] = position[k];
    }
    cam.xforms.push_back({m_currentTime, viewToWorld});
}

void GameCapturer::captureLights(const std::vector<scene::LightRecord>& lights) {
    auto colorAndIntensity = [](const scene::Float3& radiance, Vec3f& color, float& intensity) {
        // safeColorAndIntensity.
        const float i = std::max(std::max(radiance[0], radiance[1]), radiance[2]);
        if (i < std::numeric_limits<float>::min()) {
            color = {0.f, 0.f, 0.f};
            intensity = 0.f;
            return;
        }
        constexpr float kIntensityMax = 1e+20f;
        for (int k = 0; k < 3; ++k) {
            color[k] = std::clamp(radiance[k], 0.f, i * kIntensityMax) / i;
        }
        intensity = i;
    };
    for (const scene::LightRecord& l : lights) {
        if (l.hash == 0) {
            continue;
        }
        if (l.type == hash::LightType::Distant) {
            auto [it, isNew] = m_cap.distantLights.try_emplace(l.hash);
            CaptureDistantLight& d = it->second;
            if (isNew) {
                d.hash = l.hash;
                colorAndIntensity(l.radiance, d.color, d.intensity);
                d.angleDegrees = static_cast<float>(l.halfAngle * 2.0 * kRadiansToDegrees);
                d.direction = {l.direction[0], l.direction[1], l.direction[2]};
                d.firstTime = m_currentTime;
            }
            d.finalTime = m_currentTime;
            continue;
        }
        if (l.type != hash::LightType::Sphere) {
            continue; // Rect / Disk / Cylinder: not implemented upstream either
        }
        auto [it, isNew] = m_cap.sphereLights.try_emplace(l.hash);
        CaptureSphereLight& s = it->second;
        Vec3f shapingDir{0.f, 0.f, -1.f};
        if (isNew) {
            s.hash = l.hash;
            colorAndIntensity(l.radiance, s.color, s.intensity);
            s.radius = l.radius;
            s.firstTime = m_currentTime;
            if (l.shaping.enabled) {
                s.shapingEnabled = true;
                s.coneAngleDegrees = static_cast<float>(std::acos(double(l.shaping.cosConeAngle)) * kRadiansToDegrees);
                s.coneSoftness = l.shaping.coneSoftness;
                s.focusExponent = l.shaping.focusExponent;
            }
        }
        if (s.shapingEnabled) {
            shapingDir = {l.shaping.direction[0], l.shaping.direction[1], l.shaping.direction[2]};
        }
        s.xforms.push_back({m_currentTime, rotationBetween({0.f, 0.f, -1.f}, shapingDir, {l.position[0], l.position[1], l.position[2]})});
        s.finalTime = m_currentTime;
    }
}

Mat4d GameCapturer::instanceCorrection() const {
    Mat4d xform = identity4d();
    const CaptureCamera& cam = m_cap.camera;
    if (!cam.valid) {
        return xform;
    }
    if (cam.projInv && !cam.viewInv) {
        xform[0] = xform[5] = -1.0; // games that render the world upside down
    } else if (cam.isLHS() && !cam.viewInv) {
        xform[0] = -1.0; // view and projection of different handedness: mirror
    }
    return xform;
}

void GameCapturer::captureInstance(const CaptureDraw& draw, std::unordered_set<std::uint64_t>& seen) {
    ++m_stats.draws;
    if (!draw.geometry || !draw.geometry->captured() || !draw.translation) {
        ++m_stats.skippedNoGeometry;
        return;
    }
    const std::uint64_t instanceId = draw.instance.instanceId;
    if (instanceId == 0 || instanceId == UINT64_MAX) {
        ++m_stats.skippedNoInstance; // no instance, or a "virtual" one
        return;
    }
    if (!seen.insert(instanceId).second) {
        ++m_stats.duplicateInstanceDraws;
        return;
    }
    const Hash64 meshHash = draw.geometry->assetHash(m_options.assetRule);
    if (meshHash == 0) {
        ++m_stats.skippedZeroHash;
        return;
    }
    const bool isNew = m_cap.instances.count(instanceId) == 0;
    CaptureInstance& instance = m_cap.instances[instanceId];
    if (isNew) {
        // newInstance.
        const Hash64 matHash = draw.translation->material.hash();
        if (matHash != 0 && m_cap.materials.count(matHash) == 0) {
            captureMaterial(matHash, draw);
        }
        const bool isNewMesh = m_cap.meshes.count(meshHash) == 0;
        const std::uint32_t instanceNum = m_meshInstanceCount[meshHash]++;
        if (isNewMesh) {
            captureMesh(meshHash, matHash, draw);
        }
        instance.id = instanceId;
        instance.mesh = meshHash;
        instance.material = matHash;
        instance.meshInstNum = instanceNum;
        instance.firstTime = m_currentTime;
    }
    if (!isNew && m_options.captureInstances && meshHash != instance.mesh) {
        // New vertex data for the instance's BLAS (dynamic geometry): upstream's PositionsUpdate path.
        if (auto it = m_cap.meshes.find(instance.mesh); it != m_cap.meshes.end()) {
            captureMeshUpdate(it->second, draw);
        }
    }
    if (m_options.captureInstances && (isNew || draw.instance.hasTransformChanged)) {
        instance.xforms.push_back({m_currentTime, multiply(toMat4d(draw.instance.objectToWorld), instanceCorrection())});
        if (draw.geometry->skinning.valid()) {
            const geometry::SkinningData& skin = draw.geometry->skinning.get();
            if (skin.numBones > 0) {
                SampledBoneXforms bones;
                bones.time = m_currentTime;
                for (std::uint32_t b = 0; b < skin.numBones && b < skin.boneMatrices.size(); ++b) {
                    bones.xforms.push_back(toMat4d(skin.boneMatrices[b]));
                }
                instance.boneXforms.push_back(std::move(bones));
            }
        }
    }
    instance.finalTime = m_currentTime;
    instance.isSky = draw.translation->cameraType == scene::CameraType::Sky;
    // createDrawCallMetadata.
    const scene::LegacyMaterialRecord& m = draw.translation->material;
    RenderingMetaData& md = instance.metadata;
    md.alphaTestEnabled = m.alphaTestEnabled;
    md.alphaTestReferenceValue = m.alphaTestReferenceValue;
    md.alphaTestCompareOp = m.alphaTestCompareOp;
    md.alphaBlendEnabled = m.blendMode.enableBlending;
    md.srcColorBlendFactor = m.blendMode.colorSrcFactor;
    md.dstColorBlendFactor = m.blendMode.colorDstFactor;
    md.colorBlendOp = m.blendMode.colorBlendOp;
    md.srcAlphaBlendFactor = m.blendMode.alphaSrcFactor;
    md.dstAlphaBlendFactor = m.blendMode.alphaDstFactor;
    md.alphaBlendOp = m.blendMode.alphaBlendOp;
    md.writeMask = m.blendMode.writeMask;
    md.textureColorArg1Source = static_cast<std::uint32_t>(m.textureColorArg1Source);
    md.textureColorArg2Source = static_cast<std::uint32_t>(m.textureColorArg2Source);
    md.textureColorOperation = static_cast<std::uint32_t>(m.textureColorOperation);
    md.textureAlphaArg1Source = static_cast<std::uint32_t>(m.textureAlphaArg1Source);
    md.textureAlphaArg2Source = static_cast<std::uint32_t>(m.textureAlphaArg2Source);
    md.textureAlphaOperation = static_cast<std::uint32_t>(m.textureAlphaOperation);
    md.tFactor = m.tFactor;
    md.isTextureFactorBlend = m.isTextureFactorBlend;
    md.isVertexColorBakedLighting = m.isVertexColorBakedLighting;
}

void GameCapturer::captureMaterial(Hash64 matHash, const CaptureDraw& draw) {
    const scene::LegacyMaterialRecord& m = draw.translation->material;
    CaptureMaterial mat;
    mat.hash = matHash;
    mat.albedoTexture = matHash;
    // !surface.alphaState.isFullyOpaque: alpha test or blending in use.
    mat.enableOpacity = m.alphaTestEnabled || m.blendMode.enableBlending;
    mat.filter = mdlFilter(draw.colorSampler.magFilter);
    mat.wrapU = mdlWrapMode(draw.colorSampler.addressU);
    mat.wrapV = mdlWrapMode(draw.colorSampler.addressV);
    mat.alphaTestEnabled = m.alphaTestEnabled;
    mat.alphaTestCompareOp = m.alphaTestCompareOp;
    mat.alphaTestReferenceValue = m.alphaTestReferenceValue;
    mat.blendEnabled = m.blendMode.enableBlending;
    mat.tFactor = m.tFactor;
    m_cap.materials[matHash] = mat;
    // dumpImageToFile(textures/<mat>.dds): the texture's captured bytes. The texture is recorded (and keyed)
    // even without bytes: the material hash stays a replacement key.
    if (m_cap.textures.count(matHash) == 0) {
        std::optional<CaptureTexture> tex;
        if (m_textureSource) {
            tex = m_textureSource(matHash, draw.colorTexture);
        }
        CaptureTexture t = tex ? std::move(*tex) : CaptureTexture{};
        t.hash = matHash;
        if (t.mip0.empty()) {
            ++m_stats.texturesMissing;
        }
        m_cap.textures[matHash] = std::move(t);
    }
}

void GameCapturer::captureMesh(Hash64 meshHash, Hash64 matHash, const CaptureDraw& draw) {
    const geometry::CapturedDraw& g = *draw.geometry;
    CaptureMesh mesh;
    mesh.hash = meshHash;
    mesh.components = g.hashes.get();
    if (m_options.generationRule.contains(hash::rules::kLegacyAsset0) || m_options.generationRule.contains(hash::rules::kLegacyAsset1)) {
        const hash::DrawGeometryHashes dg = g.geometryHashes();
        if (m_options.generationRule.contains(hash::rules::kLegacyAsset0) && dg.hashes.isRuleHashDefinedUpstream(hash::rules::kLegacyAsset0)) {
            mesh.legacy0 = hash::meshReplacementHashLegacy(dg, hash::rules::kLegacyAsset0, 0);
        }
        if (m_options.generationRule.contains(hash::rules::kLegacyAsset1) && dg.hashes.isRuleHashDefinedUpstream(hash::rules::kLegacyAsset1)) {
            mesh.legacy1 = hash::meshReplacementHashLegacy(dg, hash::rules::kLegacyAsset1, 0);
        }
    }
    mesh.categories = draw.categories;
    mesh.isDoubleSided = draw.cullMode == kD3DCullNone;
    mesh.isLhs = false;
    mesh.materialHash = matHash;

    const std::uint32_t vc = g.vertexCount;
    const geometry::SlicedVertices& v = g.vertices;
    std::vector<Vec3f> points(vc), normals;
    std::vector<Vec2f> texcoords;
    std::vector<Vec4f> colors;
    for (std::uint32_t i = 0; i < vc; ++i) {
        const Vec4f p = readAttribute(v.position, i);
        points[i] = {p[0], p[1], p[2]};
    }
    if (v.normal.defined()) {
        normals.resize(vc);
        for (std::uint32_t i = 0; i < vc; ++i) {
            const Vec4f n = readAttribute(v.normal, i);
            normals[i] = {n[0], n[1], n[2]};
        }
    }
    if (v.texcoord.defined()) {
        texcoords.resize(vc);
        for (std::uint32_t i = 0; i < vc; ++i) {
            const Vec4f t = readAttribute(v.texcoord, i);
            texcoords[i] = {t[0], 1.0f - t[1]};
        }
    }
    if (v.color0.defined()) {
        colors.resize(vc);
        for (std::uint32_t i = 0; i < vc; ++i) {
            colors[i] = readAttribute(v.color0, i);
        }
    }
    // Skinning (captureMeshBlending): bonesPerVertex - 1 stored weights, the last one is 1 - their sum.
    std::vector<float> weights;
    std::vector<std::int32_t> jointIndices;
    if (g.skinning.valid() && g.skinning.get().numBones > 0) {
        const geometry::SkinningData& skin = g.skinning.get();
        mesh.numBones = skin.numBones;
        mesh.bonesPerVertex = std::max<std::uint32_t>(1u, skin.numBonesPerVertex);
        const std::uint32_t bpv = mesh.bonesPerVertex;
        weights.reserve(std::size_t(vc) * bpv);
        for (std::uint32_t i = 0; i < vc; ++i) {
            const Vec4f w = readAttribute(skin.blendWeights, i);
            float last = 1.0f;
            for (std::uint32_t b = 0; b + 1 < bpv; ++b) {
                const float wb = skin.blendWeights.defined() ? w[b] : 0.f;
                last -= wb;
                weights.push_back(wb);
            }
            weights.push_back(last);
        }
        if (skin.blendIndices.defined()) {
            jointIndices.reserve(std::size_t(vc) * bpv);
            for (std::uint32_t i = 0; i < vc; ++i) {
                const std::uint8_t* p = skin.blendIndices.base() + std::size_t(i) * skin.blendIndices.stride;
                const bool bgra = skin.blendIndices.type == hash::D3DDeclType::D3DColor;
                for (std::uint32_t b = 0; b < bpv; ++b) {
                    const std::uint32_t k = bgra && b < 3 ? 2 - b : b;
                    jointIndices.push_back(k < 4 ? p[k] : 0);
                }
            }
        } else {
            // D3D9 default bone indices 0 .. bonesPerVertex - 1.
            for (std::uint32_t i = 0; i < vc; ++i) {
                for (std::uint32_t b = 0; b < bpv; ++b) {
                    jointIndices.push_back(std::int32_t(b));
                }
            }
        }
    }

    // captureMeshIndices: the triangle list, with the winding swapped when the handedness changes.
    std::vector<std::int32_t> indices = triangleListIndices(g.topology, g.indices.get(), vc);
    const CaptureCamera& cam = m_cap.camera;
    const bool changingHandedness = cam.valid && (cam.projInv != (mesh.isLhs != cam.isLHS()));
    if (changingHandedness) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            std::swap(indices[i], indices[i + 2]);
        }
    }

    mesh.firstTime = m_currentTime;
    mesh.sourceVertexCount = vc;
    if (m_options.reduceMeshBuffers && !indices.empty()) {
        std::vector<std::int32_t> used;
        mesh.indices = reduceIndices(indices, used);
        mesh.reducedFrom = used;
        auto reduce = [&](auto& buf, std::size_t perVertex) {
            using Elem = typename std::decay_t<decltype(buf)>::value_type;
            if (buf.empty()) {
                return;
            }
            std::vector<Elem> out;
            out.reserve(used.size() * perVertex);
            for (const std::int32_t og : used) {
                for (std::size_t e = 0; e < perVertex; ++e) {
                    const std::size_t at = std::size_t(og) * perVertex + e;
                    out.push_back(at < buf.size() ? buf[at] : Elem{});
                }
            }
            buf = std::move(out);
        };
        reduce(points, 1);
        reduce(normals, 1);
        reduce(texcoords, 1);
        reduce(colors, 1);
        reduce(weights, mesh.bonesPerVertex);
        reduce(jointIndices, mesh.bonesPerVertex);
    } else {
        mesh.indices = std::move(indices);
        mesh.reducedFrom.resize(vc);
        for (std::uint32_t i = 0; i < vc; ++i) {
            mesh.reducedFrom[i] = std::int32_t(i);
        }
    }
    mesh.points = std::move(points);
    mesh.normals = std::move(normals);
    mesh.texcoords = std::move(texcoords);
    mesh.colors = std::move(colors);
    mesh.jointWeights = std::move(weights);
    mesh.jointIndices = std::move(jointIndices);
    if (!mesh.points.empty()) {
        mesh.bounds = {mesh.points[0][0], mesh.points[0][1], mesh.points[0][2], mesh.points[0][0], mesh.points[0][1], mesh.points[0][2]};
        for (const Vec3f& p : mesh.points) {
            for (int k = 0; k < 3; ++k) {
                mesh.bounds[k] = std::min(mesh.bounds[k], p[k]);
                mesh.bounds[3 + k] = std::max(mesh.bounds[3 + k], p[k]);
            }
        }
    }
    m_cap.meshes[meshHash] = std::move(mesh);
}

void GameCapturer::captureMeshUpdate(CaptureMesh& mesh, const CaptureDraw& draw) {
    ++m_stats.meshUpdates;
    const geometry::CapturedDraw& g = *draw.geometry;
    const hash::GeometryHashes& h = g.hashes.get();
    if (g.vertexCount != mesh.sourceVertexCount || h[hash::HashComponent::Indices] != mesh.components[hash::HashComponent::Indices] ||
        h[hash::HashComponent::GeometryDescriptor] != mesh.components[hash::HashComponent::GeometryDescriptor]) {
        ++m_stats.meshTopologyChanges;
        return;
    }
    // evalNewBufferAndCache: a new sample when any value moved by more than the delta since the last sample.
    auto differs = [](const std::vector<Vec3f>& a, const std::vector<Vec3f>& b, float delta) {
        if (a.size() != b.size()) {
            return true;
        }
        const float d2 = delta * delta;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const float dx = a[i][0] - b[i][0], dy = a[i][1] - b[i][1], dz = a[i][2] - b[i][2];
            if (dx * dx + dy * dy + dz * dz > d2) {
                return true;
            }
        }
        return false;
    };
    auto read = [&](const geometry::VertexAttribute& attr) {
        std::vector<Vec3f> out;
        out.reserve(mesh.reducedFrom.size());
        for (const std::int32_t src : mesh.reducedFrom) {
            const Vec4f v = readAttribute(attr, std::uint32_t(src));
            out.push_back({v[0], v[1], v[2]});
        }
        return out;
    };
    bool sampled = false;
    std::vector<Vec3f> points = read(g.vertices.position);
    const std::vector<Vec3f>& lastPoints = mesh.pointSamples.empty() ? mesh.points : mesh.pointSamples.rbegin()->second;
    if (differs(points, lastPoints, m_options.meshPositionDelta)) {
        mesh.pointSamples[m_currentTime] = std::move(points);
        sampled = true;
    }
    if (g.vertices.normal.defined() && !mesh.normals.empty()) {
        std::vector<Vec3f> normals = read(g.vertices.normal);
        const std::vector<Vec3f>& last = mesh.normalSamples.empty() ? mesh.normals : mesh.normalSamples.rbegin()->second;
        if (differs(normals, last, m_options.meshNormalDelta)) {
            mesh.normalSamples[m_currentTime] = std::move(normals);
            sampled = true;
        }
    }
    m_stats.meshSamples += sampled ? 1 : 0;
}

CaptureData GameCapturer::finish() const {
    CaptureData out = m_cap;
    // prepExportMetaData.
    out.meta.geometryHashRule = m_options.assetRuleString;
    out.meta.startTimeCode = 0.0;
    out.meta.endTimeCode = m_frames > 0 ? std::floor(static_cast<double>(m_frames - 1)) : 0.0;
    out.meta.numFramesCaptured = m_frames;
    // prepExportMeshes: meshes without data are dropped; the material link only when it was captured.
    for (auto it = out.meshes.begin(); it != out.meshes.end();) {
        if (it->second.points.empty() || it->second.indices.empty()) {
            it = out.meshes.erase(it);
            continue;
        }
        if (out.materials.count(it->second.materialHash) == 0) {
            it->second.materialHash = 0;
        }
        if (it->second.numBones > 0) {
            out.skeletons[it->first] = generateSkeleton(it->second);
        }
        ++it;
    }
    // prepExportInstances: instances of dropped meshes are dropped; material link only when captured.
    for (auto it = out.instances.begin(); it != out.instances.end();) {
        if (it->second.mesh == 0 || out.meshes.count(it->second.mesh) == 0) {
            it = out.instances.erase(it);
            continue;
        }
        if (out.materials.count(it->second.material) == 0) {
            it->second.material = 0;
        }
        ++it;
    }
    // prepExport: the global transform of the lights root.
    const CaptureCamera& cam = out.camera;
    const bool invX = cam.valid && !cam.viewInv && (cam.projInv || cam.isLHS());
    const bool invY = cam.valid && !cam.viewInv && cam.projInv;
    out.globalXform = identity4d();
    out.globalXform[0] = invX ? -1.0 : 1.0;
    out.globalXform[5] = invY ? -1.0 : 1.0;
    return out;
}

} // namespace fuse::relight::capture::exporter
