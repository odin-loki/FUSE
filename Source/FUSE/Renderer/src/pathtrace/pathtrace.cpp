// WP-7.3 path-tracing mode: settings, frame constants, emitter map, SBT layout, capability gate, reconstruction
// choice. See include/fuse/renderer/pathtrace/pathtrace.hpp.
#include <fuse/renderer/pathtrace/pathtrace.hpp>

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/rt_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::renderer::pathtrace {

namespace {

f32 clampf(f32 v, f32 lo, f32 hi) {
    if (!(v >= lo)) {
        return lo;
    }
    return v > hi ? hi : v;
}

bool powerOfTwo(u32 v) { return v != 0u && (v & (v - 1u)) == 0u; }

u64 alignUp(u64 v, u64 a) { return (v + a - 1u) / a * a; }

} // namespace

PtSettings ptSanitize(const PtSettings& in) {
    PtSettings s = in;
    s.maxBounces = std::min(s.maxBounces, kPtMaxBounces);
    s.rrStartBounce = std::min(s.rrStartBounce, kPtMaxBounces);
    s.samplesPerFrame = std::clamp(s.samplesPerFrame, 1u, kPtMaxSamplesPerFrame);
    s.clampRadiance = clampf(s.clampRadiance, 0.f, 1.0e30f);
    for (f32& v : s.sky) {
        v = clampf(v, 0.f, 1.0e30f);
    }
    s.rayTMin = clampf(s.rayTMin, 0.f, 1.f);
    s.normalBias = clampf(s.normalBias, 0.f, 1.f);
    s.viewBias = clampf(s.viewBias, 0.f, 1.f);
    s.farDistance = clampf(s.farDistance, 1.0e-3f, 1.0e30f);
    s.minRoughness = clampf(s.minRoughness, 1.0e-3f, 1.f);
    s.cullMask &= rt::kRtMaskAll;
    if (s.cullMask == 0u) {
        s.cullMask = rt::kRtMaskVisible | rt::kRtMaskShadow;
    }
    return s;
}

bool ptInvert(const f32 in[16], f64 out[16]) {
    f64 a[4][8];
    for (u32 r = 0; r < 4u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            a[r][c] = in[c * 4u + r];
            a[r][c + 4u] = r == c ? 1.0 : 0.0;
        }
    }
    for (u32 c = 0; c < 4u; ++c) {
        u32 pivot = c;
        for (u32 r = c + 1u; r < 4u; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) {
                pivot = r;
            }
        }
        if (!(std::fabs(a[pivot][c]) > 1e-30)) {
            return false;
        }
        for (u32 k = 0; k < 8u; ++k) {
            std::swap(a[c][k], a[pivot][k]);
        }
        const f64 d = a[c][c];
        for (u32 k = 0; k < 8u; ++k) {
            a[c][k] /= d;
        }
        for (u32 r = 0; r < 4u; ++r) {
            if (r != c) {
                const f64 f = a[r][c];
                for (u32 k = 0; k < 8u; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (u32 r = 0; r < 4u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            out[c * 4u + r] = a[r][c + 4u];
        }
    }
    return true;
}

bool buildPtFrameConstants(const PtSettings& settingsIn, const PtCamera& camera, const PtFrameParams& p, PtFrameConstants& c) {
    c = PtFrameConstants{};
    f64 inv[16];
    if (p.width == 0u || p.height == 0u || !ptInvert(camera.viewProj, inv)) {
        return false;
    }
    const PtSettings s = ptSanitize(settingsIn);
    for (u32 i = 0; i < 16u; ++i) {
        c.invViewProj[i] = static_cast<f32>(inv[i]);
    }
    const f64 fl = std::sqrt(static_cast<f64>(camera.forward[0]) * camera.forward[0] +
                             static_cast<f64>(camera.forward[1]) * camera.forward[1] +
                             static_cast<f64>(camera.forward[2]) * camera.forward[2]);
    if (!(fl > 1e-20)) {
        return false;
    }
    for (u32 k = 0; k < 3u; ++k) {
        c.cameraPosition[k] = camera.position[k];
        c.cameraForward[k] = static_cast<f32>(camera.forward[k] / fl);
        c.sky[k] = s.sky[k];
    }
    c.width = p.width;
    c.height = p.height;
    c.frameIndex = p.frameIndex;
    c.seed = p.seed;
    c.samplesPerFrame = s.samplesPerFrame;
    c.maxBounces = s.maxBounces;
    c.rrStartBounce = s.rrStartBounce;
    c.lightCount = p.lightTree ? p.lightCount : 0u;
    c.cullMask = s.cullMask;
    c.emitterMapSlots = p.emitterMap ? p.emitterMapSlots : 0u;
    c.rayTMin = s.rayTMin;
    c.normalBias = s.normalBias;
    c.viewBias = s.viewBias;
    c.clampRadiance = s.clampRadiance;
    c.farDistance = s.farDistance;
    c.minRoughness = s.minRoughness;
    c.sampleBase = p.sampleBase;
    u32 flags = 0u;
    const bool lights = p.lightTree && p.lightCount > 0u;
    if (lights && s.strategy != PtStrategy::BsdfOnly) {
        flags |= kPtFlagNee;
    }
    if (lights && s.strategy != PtStrategy::NeeOnly) {
        flags |= kPtFlagEmitterHits;
    }
    flags |= s.russianRoulette ? kPtFlagRussianRoulette : 0u;
    flags |= s.accumulate ? kPtFlagAccumulate : 0u;
    flags |= s.guides ? kPtFlagGuides : 0u;
    flags |= s.clampRadiance > 0.f ? kPtFlagClamp : 0u;
    c.flags = flags;
    return true;
}

// --- emitter map -------------------------------------------------------------------------------------------

bool buildPtEmitterMap(const u32* triangleCounts, u32 instanceSlots, const PtEmitterRef* refs, u32 count, std::vector<u32>& out) {
    out.clear();
    if (instanceSlots == 0u || (count > 0u && refs == nullptr) || triangleCounts == nullptr) {
        return count == 0u;
    }
    std::vector<bool> used(instanceSlots, false);
    for (u32 i = 0; i < count; ++i) {
        if (refs[i].instance >= instanceSlots || refs[i].triangle >= triangleCounts[refs[i].instance] || refs[i].emitter == kPtInvalid) {
            return false;
        }
        used[refs[i].instance] = true;
    }
    u64 words = instanceSlots;
    for (u32 s = 0; s < instanceSlots; ++s) {
        words += used[s] ? triangleCounts[s] : 0u;
    }
    if (words >= kPtInvalid) {
        return false;
    }
    out.assign(static_cast<size_t>(words), kPtInvalid);
    u32 cursor = instanceSlots;
    for (u32 s = 0; s < instanceSlots; ++s) {
        if (used[s]) {
            out[s] = cursor;
            cursor += triangleCounts[s];
        }
    }
    for (u32 i = 0; i < count; ++i) {
        u32& word = out[out[refs[i].instance] + refs[i].triangle];
        if (word != kPtInvalid) {
            out.clear();
            return false;
        }
        word = refs[i].emitter;
    }
    return true;
}

bool buildPtEmitterMap(const gpu_scene::GpuScene& scene, const PtEmitterRef* refs, u32 count, std::vector<u32>& out) {
    const u32 slots = scene.instanceHighWater();
    std::vector<u32> triangles(slots, 0u);
    for (u32 s = 0; s < slots; ++s) {
        const gpu_scene::GpuInstance& inst = scene.instance(s);
        if ((inst.flags & gpu_scene::kInstanceValid) != 0u && inst.mesh < scene.meshCount()) {
            triangles[s] = scene.mesh(inst.mesh).triangleCount;
        }
    }
    return buildPtEmitterMap(triangles.data(), slots, refs, count, out);
}

u32 ptEmitterRefsFromTree(const light_tree::LightTree& tree, const std::vector<light_tree::EmissiveTriangleRef>& adapterRefs,
                          std::vector<PtEmitterRef>& out) {
    out.clear();
    const std::vector<light_tree::LightTreeEmitter>& emitters = tree.emitters();
    for (u32 e = 0; e < emitters.size(); ++e) {
        const light_tree::LightTreeEmitter& em = emitters[e];
        if (em.kind != light_tree::kLtKindTriangle || em.source >= adapterRefs.size()) {
            continue;
        }
        const light_tree::EmissiveTriangleRef& r = adapterRefs[em.source];
        if (r.instance == light_tree::kLtInvalid) {
            continue;
        }
        out.push_back(PtEmitterRef{r.instance, r.triangle, e});
    }
    return static_cast<u32>(out.size());
}

u32 ptEmitterLookup(const u32* words, u32 wordCount, u32 slots, u32 instance, u32 triangle) {
    if (words == nullptr || instance >= slots || instance >= wordCount) {
        return kPtInvalid;
    }
    const u32 base = words[instance];
    if (base == kPtInvalid || base >= wordCount || triangle >= wordCount - base) {
        return kPtInvalid;
    }
    return words[base + triangle];
}

// --- SBT ------------------------------------------------------------------------------------------------------

PtSbtLayout computePtSbtLayout(u32 handleSize, u32 handleAlignment, u32 baseAlignment, u32 maxStride, u32 missRecords,
                               u32 hitRecords) {
    PtSbtLayout l{};
    if (handleSize == 0u || !powerOfTwo(handleAlignment) || !powerOfTwo(baseAlignment) || missRecords == 0u || hitRecords == 0u) {
        return l;
    }
    l.handleSize = handleSize;
    l.recordStride = static_cast<u32>(alignUp(handleSize, handleAlignment));
    if (l.recordStride > maxStride) {
        return l;
    }
    u64 cursor = 0;
    // Ray generation: one record; size == stride, and both a multiple of the base alignment keeps the next
    // region's start aligned.
    l.raygen.offset = cursor;
    l.raygen.stride = alignUp(l.recordStride, baseAlignment);
    l.raygen.size = l.raygen.stride;
    if (l.raygen.stride > maxStride) {
        return l;
    }
    cursor = alignUp(cursor + l.raygen.size, baseAlignment);
    l.miss.offset = cursor;
    l.miss.stride = l.recordStride;
    l.miss.size = alignUp(static_cast<u64>(missRecords) * l.recordStride, baseAlignment);
    cursor = alignUp(cursor + l.miss.size, baseAlignment);
    l.hit.offset = cursor;
    l.hit.stride = l.recordStride;
    l.hit.size = alignUp(static_cast<u64>(hitRecords) * l.recordStride, baseAlignment);
    cursor = alignUp(cursor + l.hit.size, baseAlignment);
    l.bytes = cursor;
    l.valid = true;
    return l;
}

// --- capability gate --------------------------------------------------------------------------------------------

PtCapabilities evaluatePtCapabilities(const RendererCaps& caps) {
    PtCapabilities out{};
    const rt::RtCapabilities rt = rt::evaluateRtCapabilities(caps);
    if (!rt.usable) {
        out.reason = rt.reason;
        out.pipelineReason = rt.reason;
        return out;
    }
    out.rayQuery = true;
    out.reason = "ok";
    if (!caps.rayTracingPipeline) {
        out.pipelineReason = caps.tierCap < RenderTier::T3 ? "tier capped below T3 (VulkanDeviceDesc::maxTier / FUSE_RENDER_TIER_MAX)"
                                                          : "VK_KHR_ray_tracing_pipeline not supported / not enabled";
        return out;
    }
    out.rayTracingPipeline = true;
    out.pipelineReason = "ok";
    return out;
}

// --- reconstruction -----------------------------------------------------------------------------------------------

PtReconstruction selectPtReconstruction(const upscale::UpscalerRegistry* upscalers, const denoise::DenoiserRegistry* denoisers,
                                        const PtReconstructionRequest& req) {
    PtReconstruction r{};
    const char* rrReason = "ray reconstruction not requested";
    if (req.allowRayReconstruction) {
        const upscale::UpscalerCaps* rr = upscalers != nullptr ? upscalers->find(kPtRayReconstructionBackend) : nullptr;
        if (rr == nullptr) {
            rrReason = "no ray-reconstruction backend registered (NVIDIA plugin not loaded)";
        } else if (rr->stub || !rr->temporal || !rr->supports_api(upscale::UpscalerApi::Vulkan)) {
            rrReason = "ray-reconstruction backend is a stub / not a Vulkan temporal backend";
        } else {
            r.kind = PtReconstructionKind::RayReconstruction;
            r.backend = rr->name;
            r.reason = "DLSS Ray Reconstruction (upscaler registry)";
            return r;
        }
    }
    if (req.allowDenoiser && denoisers != nullptr) {
        denoise::DenoiserRequirements dr{};
        dr.signal = denoise::DenoiseSignal::Gi;
        dr.method = req.denoiserMethod;
        dr.allow_fallback = req.allowInTreeFallback;
        dr.have_native_frame = req.haveNativeFrame;
        dr.have_hit_distance = req.haveHitDistance;
        dr.have_gradients = false;
        const denoise::DenoiserSelection sel = denoisers->select(dr);
        if (sel.caps != nullptr) {
            r.kind = PtReconstructionKind::Denoiser;
            r.backend = sel.caps->name;
            r.method = sel.method;
            r.fallback = sel.fallback || req.allowRayReconstruction;
            r.reason = sel.reason;
            return r;
        }
        rrReason = sel.reason;
    }
    r.kind = PtReconstructionKind::Accumulate;
    r.fallback = req.allowRayReconstruction || req.allowDenoiser;
    r.reason = rrReason;
    return r;
}

} // namespace fuse::renderer::pathtrace
