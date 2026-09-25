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
// Ported from dxvk-remix src/d3d9/d3d9_rtx_geometry.cpp@0867d3c (D3D9Rtx::computeHash, vertex shader
// component) and the DxsoShaderMetaInfo analysis of src/dxso/dxso_compiler.cpp@0867d3c

// FUSE Relight RL-1.6: the "vertexshader" geometry hash component (plan §4.1.1).
//
// Remix hashes a programmable-VS draw's shader (only with rtx.useVertexCapture and a generation
// rule that has geometrydescriptor):
//   h = XXH3(bytecode); h = XXH3(fConsts, maxConstIndexF * 16, h); h = XXH3(iConsts, maxConstIndexI * 16, h);
//   h = XXH3(bConsts, maxConstIndexB * 4 / 32, h)   (the integer quirk: 16 bools hash 2 bytes).
// maxConstIndex* is DxsoShaderMetaInfo, which Remix's dxso compiler fills while it translates the
// shader, one instruction at a time. analyzeVertexShader() replays that on the D3D9 token stream:
//   * a source operand that reads c#, i# or b# raises the maximum to index + 1, clamped to the
//     constant layout (HWVP VS: 256 float, 16 int, 16 bool; SWVP: 8192, 2048, 2048);
//   * a register a def / defi / defb already defined (earlier in the stream) does not count;
//   * a relatively addressed float constant sets maxConstIndexF to the layout's float count, even
//     when it was defined;
//   * only the sources the compiler loads count: m4x4/m4x3/m3x4/m3x3/m3x2 read src1 + 0..rows-1;
//     sincos, sgn, expp, logp, frc, abs, nrm, lit, mov, mova, rcp, rsq, exp, log read src0 only;
//     loop reads its integer register (src1), rep and if / ifc / breakc their sources; call,
//     callnz, label, ret, texldl samplers and dcl / def* operands read nothing it counts.
// Parity for VS draws is best-effort (plan risk R3): this is the documented part of the
// compiler's behaviour, covered by known-answer tests against a Python twin of the same rules.
#pragma once

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace fuse::relight::capture::vertex_capture {

/// D3D9ConstantLayout for vertex shaders.
struct ConstantLayout {
    std::uint32_t floatCount = 256; ///< caps::MaxFloatConstantsVS (8192 with software vertex processing)
    std::uint32_t intCount = 16;    ///< caps::MaxOtherConstants (2048 with SWVP)
    std::uint32_t boolCount = 16;   ///< caps::MaxOtherConstants (2048 with SWVP)

    [[nodiscard]] static ConstantLayout forVertexShaders(bool softwareVertexProcessing) {
        return softwareVertexProcessing ? ConstantLayout{8192, 2048, 2048} : ConstantLayout{};
    }
};

/// DxsoShaderMetaInfo's constant ranges.
struct ShaderConstantRanges {
    std::uint32_t maxConstIndexF = 0;
    std::uint32_t maxConstIndexI = 0;
    std::uint32_t maxConstIndexB = 0;
    bool valid = false; ///< false: not a vertex shader, or a malformed token stream

    friend bool operator==(const ShaderConstantRanges&, const ShaderConstantRanges&) = default;
};

/// Replays the dxso compiler's constant bookkeeping over a D3D9 vertex shader (tokens as
/// GetFunction returns them, `byteSize` bytes). Returns valid = false for pixel shaders or
/// streams that end without D3DSIO_END.
[[nodiscard]] ShaderConstantRanges analyzeVertexShader(std::span<const std::uint32_t> tokens,
                                                       const ConstantLayout& layout);

/// The vertexshader component from the draw's shader and current constants. `floatConstants`
/// holds 16 bytes per register, `intConstants` 16 bytes per register, `boolConstants` u32 words
/// (bit i of word i / 32); each must cover the ranges it is read over (null only when the range
/// is empty).
[[nodiscard]] hash::Hash64 hashVertexShader(std::span<const std::uint8_t> bytecode, const ShaderConstantRanges& ranges,
                                            const void* floatConstants, const void* intConstants,
                                            const void* boolConstants) noexcept;

/// The component for one draw, from the tap's DrawState: nullopt without a programmable VS (none
/// bound, or a POSITIONT declaration: D3D9DeviceEx::UseProgrammableVS()), without its bytecode, or
/// without the constants the ranges need (a replay that does not carry them).
/// Results per shader id are not cached: the analysis is linear in the token count.
[[nodiscard]] std::optional<hash::Hash64> vertexShaderHash(const tap::DrawState& state);

/// A GeometryCaptureConfig::vertexShaderHash hook (capture/geometry) computing vertexShaderHash().
[[nodiscard]] std::function<std::optional<hash::Hash64>(const tap::DrawState&)> vertexShaderHashHook();

} // namespace fuse::relight::capture::vertex_capture
