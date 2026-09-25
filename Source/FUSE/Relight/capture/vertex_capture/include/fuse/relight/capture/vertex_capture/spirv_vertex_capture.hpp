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
// Ported from dxvk-remix src/dxso/dxso_compiler.cpp@0867d3c (emitVertexCaptureInit,
// emitVertexCaptureOp, emitVertexCaptureWrite: NV-DXVK vertex capture)

// FUSE Relight RL-1.6: vertex capture as a SPIR-V transform (plan §2.5).
//
// Remix emits its capture code while its dxso compiler translates the shader. Relight's vendored
// DXVK 3.1.1 compiles SM1-3 through dxbc-spirv instead, so the same capture is added to the *final*
// SPIR-V of a vertex shader, touching nothing but standard SPIR-V (SPIRV-Headers only):
//   * finds gl_Position (a BuiltIn Position variable, or a member of a gl_PerVertex-style block)
//     and the TEXCOORD0 / COLOR0 / NORMAL0 outputs by Location (dxbc-spirv gives SM1-3 outputs the
//     fixed-function locations: NORMAL0 = 0, TEXCOORD0 = 1, COLOR0 = 9) among the outputs the module
//     stores a non-zero value to (dxbc-spirv declares and zero-fills every fixed-function output of
//     an SM1/2 shader, written or not),
//     and, without a NORMAL0 output, the NORMAL0 input (dxbc-spirv names it "v<n>_normal0", or by
//     Location when given);
//   * declares the capture storage buffer (Block { int baseVertex; uint vertexCount; uint draw[2];
//     RawCapturedVertex data[] at offset 48 }, capture_layout.hpp) at the requested descriptor set
//     and binding, and gl_VertexIndex if the shader has none, and lists both in the entry point;
//   * before every OpReturn of the entry point, loads the outputs' final values and, when
//     rel = gl_VertexIndex - baseVertex is < vertexCount (unsigned), stores them at data[rel].
// Outputs are only read, so the rasterised result is unchanged. Remix's customWorldToProjection
// and clip-space jitter rewrites of gl_Position are not part of capture and are not ported.
//
// The pass rejects (status != Transformed, words empty) modules it cannot handle safely: no
// vertex entry point, no gl_Position, an entry point with OpReturnValue, a malformed stream or a
// module it already transformed.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::capture::vertex_capture {

struct SpirvCaptureOptions {
    std::uint32_t descriptorSet = 0;
    std::uint32_t binding = 0;
    std::uint32_t normal0Location = 0;   ///< output Location of NORMAL0 (dxbc-spirv)
    std::uint32_t texcoord0Location = 1; ///< output Location of TEXCOORD0
    std::uint32_t color0Location = 9;    ///< output Location of COLOR0
    /// Without a NORMAL0 output, capture the NORMAL0 input: the Input variable at this Location,
    /// or (kNoLocation) the one whose OpName ends in "_normal0" (dxbc-spirv's naming).
    std::uint32_t inputNormalLocation = kNoLocation;
    bool captureInputNormal = true;

    static constexpr std::uint32_t kNoLocation = 0xFFFFFFFFu;
};

enum class SpirvCaptureStatus : std::uint32_t {
    Transformed = 0,
    Malformed,          ///< not a SPIR-V module, or truncated / inconsistent instructions
    NoVertexEntryPoint,
    NoPosition,         ///< no BuiltIn Position output
    Unsupported,        ///< e.g. OpReturnValue in the entry point, non-float32 Position
    AlreadyTransformed, ///< the module carries the capture buffer already
};

[[nodiscard]] std::string_view spirvCaptureStatusName(SpirvCaptureStatus status);

struct SpirvCaptureResult {
    SpirvCaptureStatus status = SpirvCaptureStatus::Malformed;
    std::vector<std::uint32_t> words; ///< the transformed module (empty unless Transformed)
    std::uint32_t fields = 0;         ///< capture_layout.hpp fields::* the shader will write
    std::uint32_t returns = 0;        ///< OpReturn sites instrumented
    std::string detail;               ///< why a module was rejected

    [[nodiscard]] bool transformed() const { return status == SpirvCaptureStatus::Transformed; }
};

/// OpName given to the capture buffer variable (marks a transformed module).
inline constexpr std::string_view kCaptureBufferName = "fuse_vertex_capture";

[[nodiscard]] SpirvCaptureResult addVertexCapture(std::span<const std::uint32_t> spirv, const SpirvCaptureOptions& options);

} // namespace fuse::relight::capture::vertex_capture
