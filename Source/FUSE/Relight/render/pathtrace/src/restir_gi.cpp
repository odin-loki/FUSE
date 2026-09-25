// FUSE Relight RL-5.3: ReSTIR GI - options and the CPU runner (see restir_gi.hpp).
#include <fuse/relight/render/pathtrace/restir_gi.hpp>

#include <fuse/relight/render/material/bsdf_host.hpp>

#include "restir_gi_cpp.hpp"

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::render::pathtrace {

// The public mirrors (restir_gi.hpp) == the single-source core's constants.
static_assert(kPtFlagRestirGi == ptk::kPtFlagRestirGi && kPtFlagGiRecord == ptk::kPtFlagGiRecord, "pt flags");
static_assert(kRgiParamWords == ptk::kRgiParamWords && kRgiSurfaceWords == ptk::kRgiSurfaceWords &&
                  kRgiReservoirWords == ptk::kRgiReservoirWords && kRgiOutWords == ptk::kRgiOutWords &&
                  kRgiMaxNeighbors == ptk::kRgiMaxNeighbors,
              "record sizes");
static_assert(kRgiFlagUnbiased == ptk::kRgiFlagUnbiased && kRgiFlagHistory == ptk::kRgiFlagHistory &&
                  kRgiFlagTemporal == ptk::kRgiFlagTemporal,
              "flags");

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &RestirGiOptions::useReSTIRGI,        &RestirGiOptions::useTemporalReuse,
    &RestirGiOptions::useSpatialReuse,    &RestirGiOptions::biasCorrectionMode,
    &RestirGiOptions::temporalHistoryLength, &RestirGiOptions::spatialSamples,
    &RestirGiOptions::spatialIterations,  &RestirGiOptions::spatialRadius,
};

/// Per-pixel stash of the surface pass's record hook.
struct RecordEntry {
    ptk::PtRawHit hit{};
    ptk::float3 dir{0.f, 0.f, 0.f};
    ptk::uint bounce = 0;
    bool have = false;
};

ptk::uint recordVertex(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, const ptk::PtRawHit& h,
                       const ptk::float3& d, ptk::uint bounce, ptk::float3&) {
    RecordEntry& e = static_cast<RecordEntry*>(user)[std::size_t(py) * P.width + px];
    e.hit = h;
    e.dir = d;
    e.bounce = bounce;
    e.have = true;
    return 2u;
}

struct ApplyState {
    const ptk::float4* output = nullptr;
    u32 width = 0;
    u32 height = 0;
};

ptk::uint applyVertex(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, const ptk::PtRawHit& h,
                      const ptk::float3&, ptk::uint, ptk::float3& indirect) {
    const ApplyState& s = *static_cast<const ApplyState*>(user);
    if (s.output == nullptr || P.width != s.width || P.height != s.height) {
        return 0u;
    }
    const std::size_t i = std::size_t(py) * s.width + px;
    const ptk::float4 k = s.output[i * 2u];
    const ptk::float4 v = s.output[i * 2u + 1u];
    if (!(v.w > 0.5f) || k.x != float(h.instance) || k.y != float(h.primitive) || k.z != h.u || k.w != h.v) {
        return 0u;
    }
    indirect = ptk::float3(v.x, v.y, v.z);
    return 1u;
}

using Stage = RestirGiCpu::Stage;

struct StageParams {
    const ptk::RgiCpuContext* gc = nullptr;
    const ptk::PtCpuContext* recordCtx = nullptr;
    const ptk::float4* params = nullptr;  ///< kPtParamWords (surface: the record-mode words)
    const ptk::float4* rparams = nullptr; ///< kRgiParamWords
    u32 stage = 0;
    u32 width = 0;
    u32 height = 0;
    RecordEntry* stash = nullptr;
    ptk::float4* out = nullptr;
    ptk::float4* history = nullptr; ///< shade: the final reservoir copy (null: none)
};

struct StageKernel {
    void operator()(const kernel::LaunchIndex& idx, const StageParams& p) const {
        const ptk::RgiCpuContext& gc = *p.gc;
        const ptk::PtParams P = ptk::ptParamsUnpack(p.params);
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        if (x >= p.width || y >= p.height) {
            return;
        }
        const std::size_t pixel = std::size_t(y) * p.width + x;
        const ptk::uint px = static_cast<ptk::uint>(pixel);
        ptk::RgiParams R = ptk::rgiParamsUnpack(p.rparams);
        if (p.stage == Stage::kStageSurface) {
            RecordEntry& e = p.stash[pixel];
            e.have = false;
            (void)ptk::ptRenderSample(*p.recordCtx, P, x, y, P.sampleBase);
            ptk::rgiSurfaceWords(gc, P, e.have, e.hit, e.dir, e.bounce, p.out[pixel * 4u], p.out[pixel * 4u + 1u],
                                 p.out[pixel * 4u + 2u], p.out[pixel * 4u + 3u]);
            return;
        }
        if (p.stage == Stage::kStageShade) {
            const ptk::RgiReservoir r = ptk::rgiLoadReservoir(gc, 0u, px);
            p.out[pixel * 2u] = gc.surfaces[0][pixel * 4u];
            p.out[pixel * 2u + 1u] = ptk::rgiShade(gc, P, r, px);
            if (p.history != nullptr) {
                for (ptk::uint k = 0; k < ptk::kRgiReservoirWords; ++k) {
                    p.history[pixel * ptk::kRgiReservoirWords + k] = ptk::rgiReservoirPack(r, k);
                }
            }
            return;
        }
        ptk::RgiReservoir r;
        if (p.stage == Stage::kStageInitial) {
            r = ptk::rgiInitial(gc, P, x, y);
        } else {
            r = ptk::rgiReuse(gc, P, R, x, y, p.stage == Stage::kStageTemporal);
        }
        for (ptk::uint k = 0; k < ptk::kRgiReservoirWords; ++k) {
            p.out[pixel * ptk::kRgiReservoirWords + k] = ptk::rgiReservoirPack(r, k);
        }
    }
};

template <typename T>
void sizeTo(std::vector<T>& v, std::size_t n, const T& fill) {
    if (v.size() != n) {
        v.assign(n, fill);
    }
}

u32 giFrameFlags(const RestirGiSettings& gi, bool history) {
    return (gi.unbiased ? kRgiFlagUnbiased : 0u) | (history ? kRgiFlagHistory : 0u) |
           (gi.temporal ? kRgiFlagTemporal : 0u);
}

} // namespace

RestirGiSettings RestirGiSettings::fromOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
    RestirGiSettings s;
    s.enabled = RestirGiOptions::useReSTIRGI();
    s.temporal = RestirGiOptions::useTemporalReuse();
    s.unbiased = RestirGiOptions::biasCorrectionMode() >= 4;
    s.spatialSamples = static_cast<u32>(std::clamp(RestirGiOptions::spatialSamples(), 0, int(kRgiMaxNeighbors)));
    s.spatialIterations =
        RestirGiOptions::useSpatialReuse() ? static_cast<u32>(std::clamp(RestirGiOptions::spatialIterations(), 0, 4))
                                           : 0u;
    s.spatialRadius = std::clamp(RestirGiOptions::spatialRadius(), 1.f, 128.f);
    s.maxHistory = static_cast<float>(std::clamp(RestirGiOptions::temporalHistoryLength(), 0, 1024));
    return s;
}

void packRestirGiParams(const RestirGiSettings& s, u32 flags, u32 iteration, Word* out) {
    out[0] = Word(float(flags), float(std::min(s.spatialSamples, kRgiMaxNeighbors)), float(iteration), 0.f);
    out[1] = Word(s.spatialRadius, s.maxHistory, s.normalThreshold, s.depthThreshold);
}

struct RestirGiCpu::Impl {
    std::vector<ptk::PtCpuTexture> textures;
    std::vector<RecordEntry> stash;
    ApplyState apply{};
    ptk::PtGiHook applyHook{};
    ptk::PtGiHook recordHook{};
    ptk::PtCpuContext ctx{};
    ptk::PtCpuContext recordCtx{};

    bool bind(const PtCompiledScene& scene) {
        const material::AlbedoLut& lut = material::sharedAlbedoLut();
        if (!lut.valid()) {
            return false;
        }
        textures.resize(scene.textures().size());
        for (std::size_t i = 0; i < textures.size(); ++i) {
            const PtTextureImage& t = scene.textures()[i];
            textures[i].handle = t.handle;
            textures[i].width = t.width;
            textures[i].height = t.height;
            textures[i].texels = t.texels.empty() ? nullptr : t.texels.data();
        }
        ctx = ptk::PtCpuContext{};
        ctx.lut = lut.data();
        ctx.rt = &scene.reference();
        ctx.lights = &scene.lightSet();
        ctx.lightRecords = scene.lightRecords().empty() ? nullptr : scene.lightRecords().data();
        ctx.instances = scene.instanceWords().data();
        ctx.instanceCount = static_cast<u32>(scene.instanceWords().size() / kPtInstanceWords);
        ctx.triangles = scene.triangleWords().data();
        ctx.triangleCount = static_cast<u32>(scene.triangleWords().size() / kPtTriangleWords);
        ctx.materials = scene.materialWords().data();
        ctx.materialCount = static_cast<u32>(scene.materialWords().size() / kPtMaterialWords);
        ctx.portals = scene.portalWords().data();
        ctx.portalCount = static_cast<u32>(scene.portalWords().size() / kPtPortalWords);
        ctx.lightMap = scene.lightMap().empty() ? nullptr : scene.lightMap().data();
        ctx.lightMapCount = static_cast<u32>(scene.lightMap().size());
        ctx.textures = textures.empty() ? nullptr : textures.data();
        ctx.textureCount = static_cast<u32>(textures.size());
        recordCtx = ctx;
        recordHook.vertex = &recordVertex;
        recordHook.user = stash.data();
        recordCtx.giHook = &recordHook;
        return true;
    }

    static bool launch(kernel::Backend backend, const char* name, u32 w, u32 h, const StageParams& p) {
        return kernel::launch(backend, kernel::KernelLaunch{name, kernel::extent2(w, h), kernel::Dim3{8u, 8u, 1u}},
                              StageKernel{}, p)
            .ok;
    }
};

RestirGiCpu::RestirGiCpu() : m_impl(new Impl) {}
RestirGiCpu::~RestirGiCpu() { delete m_impl; }

const ptk::PtGiHook* RestirGiCpu::hook() const { return &m_impl->applyHook; }

bool RestirGiCpu::frame(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                        u32 frameSeed, u32 sampleBase, const RestirGiSettings& gi, kernel::Backend backend) {
    if (!scene.valid() || width == 0u || height == 0u) {
        return false;
    }
    Impl& I = *m_impl;
    const std::size_t pixels = std::size_t(width) * height;
    if (width != m_width || height != m_height) {
        m_historyValid = false;
        m_width = width;
        m_height = height;
    }
    const Word zero(0.f, 0.f, 0.f, 0.f);
    sizeTo(m_surfaces[0], pixels * kRgiSurfaceWords, zero);
    sizeTo(m_surfaces[1], pixels * kRgiSurfaceWords, zero);
    sizeTo(m_resInitial, pixels * kRgiReservoirWords, zero);
    sizeTo(m_resA, pixels * kRgiReservoirWords, zero);
    sizeTo(m_resB, pixels * kRgiReservoirWords, zero);
    sizeTo(m_history, pixels * kRgiReservoirWords, zero);
    sizeTo(m_output, pixels * kRgiOutWords, zero);
    if (I.stash.size() != pixels) {
        I.stash.assign(pixels, RecordEntry{});
    }
    if (!I.bind(scene)) {
        return false;
    }
    const u32 flags = giFrameFlags(gi, m_historyValid);
    m_flags = flags;

    Word params[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, params);
    Word record[kPtParamWords];
    for (u32 k = 0; k < kPtParamWords; ++k) {
        record[k] = params[k];
    }
    const u32 recordFlags = (settings.flags | kPtFlagGiRecord) &
                            ~(u32(kPtFlagNee) | ptk::kPtFlagRestirDi | ptk::kPtFlagDiRecord | kPtFlagRestirGi);
    record[5].w = float(recordFlags); // PtParams.flags (ptParamsUnpack)
    Word rparams[kRgiParamWords];
    packRestirGiParams(gi, flags, 0u, rparams);

    m_cur ^= 1u;
    ptk::RgiCpuContext gc{};
    gc.pt = &I.ctx;
    gc.surfaces[0] = m_surfaces[m_cur].data();
    gc.surfaces[1] = m_surfaces[m_cur ^ 1u].data();

    StageParams p{};
    p.gc = &gc;
    p.recordCtx = &I.recordCtx;
    p.params = record;
    p.rparams = rparams;
    p.width = width;
    p.height = height;
    bool ok = true;
    // surface
    p.stage = Stage::kStageSurface;
    p.stash = I.stash.data();
    p.out = m_surfaces[m_cur].data();
    ok = ok && Impl::launch(backend, "relight.restir_gi.surface", width, height, p);
    p.params = params;
    // initial
    p.stage = Stage::kStageInitial;
    p.out = m_resInitial.data();
    ok = ok && Impl::launch(backend, "relight.restir_gi.initial", width, height, p);
    // temporal
    gc.reservoirs[0] = m_resInitial.data();
    gc.reservoirs[1] = m_history.data();
    p.stage = Stage::kStageTemporal;
    p.out = m_resA.data();
    ok = ok && Impl::launch(backend, "relight.restir_gi.temporal", width, height, p);
    // spatial
    std::vector<Word>* src = &m_resA;
    std::vector<Word>* dst = &m_resB;
    Word spatialParams[kRgiParamWords];
    for (u32 it = 0; it < gi.spatialIterations && gi.spatialSamples > 0u; ++it) {
        packRestirGiParams(gi, flags, it, spatialParams);
        p.rparams = spatialParams;
        gc.reservoirs[0] = src->data();
        p.stage = Stage::kStageSpatial;
        p.out = dst->data();
        ok = ok && Impl::launch(backend, "relight.restir_gi.spatial", width, height, p);
        std::swap(src, dst);
    }
    p.rparams = rparams;
    // shade (+ history)
    gc.reservoirs[0] = src->data();
    p.stage = Stage::kStageShade;
    p.out = m_output.data();
    p.history = m_history.data();
    ok = ok && Impl::launch(backend, "relight.restir_gi.shade", width, height, p);

    I.apply.output = m_output.data();
    I.apply.width = width;
    I.apply.height = height;
    I.applyHook.vertex = &applyVertex;
    I.applyHook.user = &I.apply;
    m_historyValid = ok;
    ++m_stats.frames;
    m_stats.history = (flags & kRgiFlagHistory) != 0u;
    m_stats.surfaces = 0;
    m_stats.samples = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        m_stats.surfaces += m_output[i * 2u + 1u].w > 0.5f ? 1u : 0u;
        const Word& w1 = m_history[i * kRgiReservoirWords + 1u];
        m_stats.samples += (u32(w1.z) & 1u) != 0u && w1.x > 0.f ? 1u : 0u;
    }
    return ok;
}

bool RestirGiCpu::replayStage(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                              u32 frameSeed, u32 sampleBase, const RestirGiSettings& gi, u32 flags, u32 stage,
                              u32 iteration, const std::vector<Word>& surfaces, const std::vector<Word>& previous,
                              const std::vector<Word>& source, const std::vector<Word>& history,
                              std::vector<Word>& out, kernel::Backend backend) {
    const std::size_t pixels = std::size_t(width) * height;
    if (!scene.valid() || pixels == 0u || surfaces.size() < pixels * kRgiSurfaceWords || stage == kStageSurface ||
        (stage != kStageInitial && source.size() < pixels * kRgiReservoirWords)) {
        return false;
    }
    Impl& I = *m_impl;
    if (!I.bind(scene)) {
        return false;
    }
    Word params[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, params);
    Word rparams[kRgiParamWords];
    packRestirGiParams(gi, flags, iteration, rparams);
    out.assign(pixels * (stage == kStageShade ? kRgiOutWords : kRgiReservoirWords), Word(0.f, 0.f, 0.f, 0.f));
    ptk::RgiCpuContext gc{};
    gc.pt = &I.ctx;
    gc.surfaces[0] = surfaces.data();
    gc.surfaces[1] = previous.size() >= surfaces.size() ? previous.data() : surfaces.data();
    gc.reservoirs[0] = source.empty() ? nullptr : source.data();
    gc.reservoirs[1] = history.size() >= pixels * kRgiReservoirWords ? history.data() : gc.reservoirs[0];
    StageParams p{};
    p.gc = &gc;
    p.params = params;
    p.rparams = rparams;
    p.width = width;
    p.height = height;
    p.stage = stage;
    p.out = out.data();
    return Impl::launch(backend, "relight.restir_gi.replay", width, height, p);
}

} // namespace fuse::relight::render::pathtrace
