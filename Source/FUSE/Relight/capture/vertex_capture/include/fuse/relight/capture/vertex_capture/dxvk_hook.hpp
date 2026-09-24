// FUSE Relight RL-1.6: the process-wide hand-over between the vendored DXVK and vertex capture.
//
// DXVK builds a vertex shader's pipeline layout (DxvkIrShader::getLayout) and SPIR-V
// (DxvkIrShader::getCode) lazily, per pipeline, on its compile threads, and neither knows the D3D9
// device. The FUSE-DXVK patches RL-1.6-01 / -02 therefore ask this registry:
//   * getLayout: dxvk::fuseRelightVertexCaptureBinding(name, ...) adds the capture storage buffer
//     binding (descriptor set kDescriptorSet (DXVK's constant-buffer set), binding kBinding, resource
//     slot kResourceSlot, bound with DxvkContext::bindUniformBuffer) to every D3D9 vertex shader
//     ("vs." name) once vertex capture was enabled; the answer is latched per shader name, so a
//     shader's layout and code always agree;
//   * getCode: dxvk::fuseRelightVertexCaptureCode(name, code, set, binding, out) hands the finished
//     SPIR-V (with the binding DXVK mapped the capture buffer to) to the registered substitutor
//     (the tap of the device that enabled capture), only for shaders whose layout has the binding.
// A layout with the binding but code without the capture (no substitutor, or it declined) is
// valid: the descriptor is simply unused.
//
// FUSE_RELIGHT_VC_DUMP=<dir>: every substitution also writes <dir>/<name>.in.spv and .out.spv
// (tests run spirv-val on them).
//
// This file is linked into both DXVK DLLs (d3d9.dll enables it; d3d8.dll forwards to d3d9.dll and
// never does). No DXVK or Vulkan headers.
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuse::relight::capture::vertex_capture::dxvk_hook {

inline constexpr std::uint32_t kDescriptorSet = 1;  ///< DxvkShaderResourceMapping::setIndexForType(eCbv)
inline constexpr std::uint32_t kBinding = 120;      ///< binding inside that set (D3D9 uses 0..6)
inline constexpr std::uint32_t kResourceSlot = 120; ///< DxvkContext uniform-buffer slot (< MaxNumUniformBufferSlots)

/// Turns vertex capture on for the process (sticky). Called by the d3d9 dispatcher when a tap
/// wants vertex capture and the device can store from vertex shaders.
void enable();
[[nodiscard]] bool enabled();

/// The substitutor gets every vertex shader whose layout carries the binding. One at a time: a
/// second registration replaces the first; clear() only clears the given owner's.
using SubstituteFn = bool (*)(void* owner, const tap::ShaderModule& module, std::vector<std::uint32_t>& out);
void setSubstitutor(void* owner, SubstituteFn fn);
void clearSubstitutor(void* owner);

/// Statistics for tests: layouts that got the binding, substitutions that transformed the code.
struct Stats {
    std::uint64_t layouts = 0;
    std::uint64_t substituted = 0;
    std::uint64_t declined = 0;
};
[[nodiscard]] Stats stats();

/// What the patches call (also usable directly in tests).
bool layoutBinding(const char* shaderName, std::uint32_t* set, std::uint32_t* binding, std::uint32_t* slot);
bool substituteCode(const char* shaderName, const std::uint32_t* code, std::size_t words, std::uint32_t set,
                    std::uint32_t binding, std::vector<std::uint32_t>* out);

} // namespace fuse::relight::capture::vertex_capture::dxvk_hook
