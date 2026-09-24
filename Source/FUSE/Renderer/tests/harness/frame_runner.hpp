// WP-0.7 renderer test harness: headless frame runner on the existing G-buffer raster path.
//
// HeadlessFrameRunner owns a VulkanBootstrap (no swapchain), a ResourceManager, a GBuffer and a
// GBufferRasterPass, renders a harness::Scene into the six G-buffer attachments, reads every
// attachment back and resolves a deterministic CPU-lit RGBA8 image (the golden input).
//
//   * Shaders: `RasterShaders::Stock` uses shaders/raster/gbuffer.{vert,frag} unchanged (per-draw
//     NDC depth, screen-space scenes only). `RasterShaders::Projected` swaps only the vertex stage
//     for tests/harness/shaders/harness_gbuffer.vert (per-vertex NDC depth after the CPU camera
//     projection); the fragment stage and write_gbuffer() packing are the stock ones.
//   * Validation: VK_LAYER_KHRONOS_validation with synchronization validation is on by default
//     (VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true through the layer-settings environment, set before
//     the instance is created unless the caller already set it). FUSE_HARNESS_VALIDATION=0 turns it
//     off. Messages are counted by the instance messenger (vulkanValidationCounters()).
//   * Tier: from the device's RendererCaps (WP-0.1) when fuse_rhi has them, else inferred from the
//     device extensions with the same rules (T1 mesh shader, T2 + acceleration structure + ray
//     query, T3 + ray tracing pipeline + cooperative matrix); FUSE_RENDER_TIER_MAX caps both.
//
// In the stub backend create() returns nullptr with a reason (tests exit 77).
#pragma once

#include "image_io.hpp"
#include "scene.hpp"

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer::harness {

enum class RasterShaders : u8 {
    Stock = 0,     ///< gbuffer.vert + gbuffer.frag (screen-space batches only)
    Projected = 1, ///< harness_gbuffer.vert + gbuffer.frag
};

struct HarnessOptions {
    u32 width = 128;
    u32 height = 128;
    /// Khronos validation + sync validation (default on; FUSE_HARNESS_VALIDATION=0 disables).
    bool validation = true;
    /// Renderer tier cap for the device (-1: none). Passed to VulkanDeviceDesc::maxTier when the
    /// RendererCaps API is present; always applied to the reported tier.
    int tierCap = -1;
    /// Directory with the stock gbuffer.{vert,frag}.spv (b5_rhi_rows.cmake output).
    std::string shaderDir;
    /// Directory with harness_gbuffer.vert.spv (rp_harness.cmake output).
    std::string harnessShaderDir;
};

struct TierReport {
    u32 tier = 0;                  ///< 0..3 (effective, after caps)
    bool fromRendererCaps = false; ///< false: inferred by the harness
    std::string summary;
};

struct ValidationReport {
    bool layerRequested = false;
    bool syncRequested = false;
    u32 errors = 0;
    u32 warnings = 0;
    std::string lastError;
};

/// Decoded G-buffer plus the resolved image of one frame.
struct FrameCapture {
    u32 width = 0;
    u32 height = 0;
    std::vector<math::Vec3> normal;   ///< RT0 decoded (world), zero where uncovered
    std::vector<f32> ao;              ///< RT0.w
    std::vector<math::Vec4> albedo;   ///< RT1 (a = 1 where covered, 0 = clear)
    std::vector<math::Vec4> surface;  ///< RT2: roughness, metallic, emissive mask, shading / 255
    std::vector<f32> depth;           ///< RT4 (NDC depth, 0 = clear)
    std::vector<math::Vec3> emissive; ///< RT5
    std::vector<u8> hudMask;          ///< 1 where the shading model is kHudShadingModel
    ImageRgba8 lit;                   ///< CPU resolve (sRGB), the golden input

    u32 draws = 0;
    u64 triangles = 0;
    u64 vertices = 0;
    f64 projectMs = 0.0;
    f64 gpuFrameMs = 0.0; ///< record + submit + wait (wall clock)
    f64 readbackMs = 0.0;

    u32 coveredPixels() const;
};

/// Resolve parameters (fixed so goldens stay stable).
struct ResolveParams {
    math::Vec3 lightDir{0.35f, 0.85f, 0.4f};
    math::Vec3 background{0.02f, 0.02f, 0.03f};
};

/// Deterministic CPU lighting of a decoded G-buffer into `capture.lit` (half-Lambert + ambient,
/// emissive added, HUD pixels unlit, background where RT1.a == 0, sRGB encoded).
void resolveCapture(FrameCapture& capture, const math::Vec3& viewDir, const ResolveParams& params = {});

/// Writes an EXR with the decoded G-buffer channels (albedo.RGB, depth.Z, emissive.RGB, normal.XYZ)
/// for failure triage.
bool writeCaptureExr(const std::string& path, const FrameCapture& capture);

class HeadlessFrameRunner {
public:
    /// nullptr (with `reason`) when there is no Vulkan backend, device, or shader SPIR-V.
    static std::unique_ptr<HeadlessFrameRunner> create(const HarnessOptions& options, std::string& reason);
    ~HeadlessFrameRunner();

    HeadlessFrameRunner(const HeadlessFrameRunner&) = delete;
    HeadlessFrameRunner& operator=(const HeadlessFrameRunner&) = delete;

    /// Renders one frame of `scene` and fills `out` (G-buffer readback + resolve).
    bool render(const Scene& scene, RasterShaders shaders, FrameCapture& out, std::string& error);

    const TierReport& tier() const { return m_tier; }
    ValidationReport validation() const;
    void resetValidation();
    /// Negative control: records two unsynchronised writes to one buffer and returns how many
    /// validation errors that produced (> 0 proves synchronization validation is live).
    u32 runSyncHazardControl();
    const HarnessOptions& options() const { return m_options; }
    std::string deviceName() const;

    struct Impl;

private:
    HeadlessFrameRunner() = default;
    HarnessOptions m_options;
    TierReport m_tier;
    bool m_validationRequested = false;
    bool m_syncRequested = false;
    std::unique_ptr<Impl> m_impl;
};

/// Parses "T0".."T3" / "t0".."t3" / "0".."3"; -1 when malformed.
int parseTier(const std::string& text);

} // namespace fuse::renderer::harness
