/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/d3d9/d3d9_state.h@0867d3c (CapturedVertex, NV-DXVK vertex capture)

// FUSE Relight RL-1.6: the 48-byte vertex capture layout (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.5).
//
// Remix's dxso compiler writes a CapturedVertex per vertex (position already back-transformed to
// object space on the GPU). Relight's SPIR-V pass (spirv_vertex_capture.hpp) writes the same 48
// bytes, but with the clip-space position (x, y, z, w) in the first 16 bytes; the CPU
// back-transform (back_transform.hpp) then produces Remix's CapturedVertex. Every slot is 48 bytes
// in both forms and the texcoord / normal / colour members sit at Remix's offsets.
//
// One capture region per draw: a 48-byte RegionHeader, then `vertexCount` slots. The injected code
// stores vertex gl_VertexIndex at slot gl_VertexIndex - baseVertex when that is < vertexCount
// (Remix: vertexIndex = gl_VertexIndex - baseVertex, baseVertex = BaseVertexIndex + minIndex).
//
// Modifications (FUSE): the GPU form keeps the clip-space w in Remix's pad0 and a field mask in
// pad1; the region header replaces Remix's per-draw constant buffer member baseVertex.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fuse::relight::capture::vertex_capture {

inline constexpr std::uint32_t kCapturedVertexSize = 48;
inline constexpr std::uint32_t kRegionHeaderSize = 48;

/// Remix's CapturedVertex (the CPU result of the back-transform).
struct CapturedVertex {
    float position[3] = {};  ///< object space
    std::uint32_t pad0 = 0;
    float texcoord0[2] = {}; ///< VS output TEXCOORD0.xy
    std::uint32_t pad1 = 0;
    std::uint32_t pad2 = 0;
    float normal0[3] = {};   ///< VS NORMAL0 (output, else input) times normalTransform
    std::uint32_t color0 = 0xFFFFFFFFu; ///< VS output COLOR0 as D3DCOLOR 0xAARRGGBB, else opaque white
};
static_assert(sizeof(CapturedVertex) == kCapturedVertexSize, "Remix's CapturedVertex is 48 bytes");
static_assert(offsetof(CapturedVertex, texcoord0) == 16 && offsetof(CapturedVertex, normal0) == 32 &&
                  offsetof(CapturedVertex, color0) == 44,
              "CapturedVertex member offsets (Remix d3d9_state.h)");

/// Bits of RawCapturedVertex::fields: which members the shader wrote.
namespace fields {
inline constexpr std::uint32_t kWritten = 1u << 0;      ///< the slot was written (clip position is valid)
inline constexpr std::uint32_t kTexcoord = 1u << 1;     ///< a TEXCOORD0 output exists
inline constexpr std::uint32_t kNormalOutput = 1u << 2; ///< normal0 comes from a NORMAL0 output
inline constexpr std::uint32_t kNormalInput = 1u << 3;  ///< normal0 comes from the NORMAL0 input
inline constexpr std::uint32_t kColor = 1u << 4;        ///< a COLOR0 output exists
inline constexpr std::uint32_t kAll = kWritten | kTexcoord | kNormalOutput | kNormalInput | kColor;
} // namespace fields

/// What the injected SPIR-V stores per vertex (std430, 48-byte array stride).
struct RawCapturedVertex {
    float clip[4] = {};      ///< gl_Position at the shader's return (Remix position + pad0)
    float texcoord0[2] = {}; ///< TEXCOORD0.xy, (0, 0) without that output
    std::uint32_t fields = 0; ///< fields::* (Remix pad1)
    std::uint32_t pad2 = 0;
    float normal0[3] = {};    ///< untransformed NORMAL0.xyz (output, else input); 0 without one
    std::uint32_t color0 = 0xFFFFFFFFu; ///< COLOR0 packed as Remix packs it; opaque white without one
};
static_assert(sizeof(RawCapturedVertex) == kCapturedVertexSize, "the GPU slot is 48 bytes");
static_assert(offsetof(RawCapturedVertex, texcoord0) == offsetof(CapturedVertex, texcoord0) &&
                  offsetof(RawCapturedVertex, fields) == offsetof(CapturedVertex, pad1) &&
                  offsetof(RawCapturedVertex, normal0) == offsetof(CapturedVertex, normal0) &&
                  offsetof(RawCapturedVertex, color0) == offsetof(CapturedVertex, color0),
              "the GPU slot keeps Remix's member offsets");

/// First 48 bytes of a capture region. Written by the CPU before the draw, read by the shader.
struct RegionHeader {
    std::int32_t baseVertex = 0;   ///< gl_VertexIndex of slot 0
    std::uint32_t vertexCount = 0; ///< slots in the region; 0 = capture nothing
    std::uint32_t drawLow = 0;     ///< draw ordinal (tap onDraw count), for the CPU
    std::uint32_t drawHigh = 0;
    std::uint32_t reserved[8] = {};
};
static_assert(sizeof(RegionHeader) == kRegionHeaderSize, "the region header is 48 bytes");

/// Byte size of a region with `vertexCount` slots.
constexpr std::size_t regionBytes(std::uint32_t vertexCount) {
    return std::size_t(kRegionHeaderSize) + std::size_t(vertexCount) * kCapturedVertexSize;
}

/// Remix's COLOR0 packing (dxso_compiler emitVertexCaptureOp): each channel clamped to [0, 1],
/// times 255, plus 0.5, converted to an unsigned integer; 0xAARRGGBB.
std::uint32_t packColor(const float rgba[4]);

} // namespace fuse::relight::capture::vertex_capture
