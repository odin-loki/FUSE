// FUSE Relight RL-5.2: ReSTIR DI - options, the tiles' light table and the CPU runner (see restir_di.hpp).
#include <fuse/relight/render/pathtrace/restir_di.hpp>

#include <fuse/relight/render/material/bsdf_host.hpp>

#include "restir_di_cpp.hpp"

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::render::pathtrace {

// The public mirrors (restir_di.hpp) == the single-source core's constants.
static_assert(kPtFlagRestirDi == ptk::kPtFlagRestirDi && kPtFlagDiRecord == ptk::kPtFlagDiRecord, "pt flags");
static_assert(kRdiParamWords == ptk::kRdiParamWords && kRdiSurfaceWords == ptk::kRdiSurfaceWords &&
                  kRdiReservoirWords == ptk::kRdiReservoirWords && kRdiOutWords == ptk::kRdiOutWords &&
                  kRdiMaxNeighbors == ptk::kRdiMaxNeighbors,
              "record sizes");
static_assert(kRdiFlagUnbiased == ptk::kRdiFlagUnbiased && kRdiFlagHistory == ptk::kRdiFlagHistory &&
                  kRdiFlagInitialVisibility == ptk::kRdiFlagInitialVisibility &&
                  kRdiFlagTemporal == ptk::kRdiFlagTemporal,
              "flags");

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &RestirDiOptions::useRTXDI,          &RestirDiOptions::initialSampleCount,
    &RestirDiOptions::lightTreeSampleCount, &RestirDiOptions::bsdfSampleCount, &RestirDiOptions::spatialSamples,
    &RestirDiOptions::spatialIterations, &RestirDiOptions::spatialRadius,
    &RestirDiOptions::maxHistoryLength,  &RestirDiOptions::enableTemporalReuse,
    &RestirDiOptions::enableInitialVisibility, &RestirDiOptions::enableRayTracedBiasCorrection,
    &RestirDiOptions::lightTileCount,    &RestirDiOptions::lightTileSize,
};

constexpr double kTwo24 = 16777216.0;

/// Per-pixel stash of the surface pass's record hook.
struct RecordEntry {
    ptk::PtRawHit hit{};
    ptk::float3 dir{0.f, 0.f, 0.f};
    bool have = false;
};

ptk::uint recordVertex(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, const ptk::PtRawHit& h,
                       const ptk::float3& d, ptk::float3&) {
    RecordEntry& e = static_cast<RecordEntry*>(user)[std::size_t(py) * P.width + px];
    e.hit = h;
    e.dir = d;
    e.have = true;
    return 2u;
}

struct ApplyState {
    const ptk::float4* output = nullptr;
    u32 width = 0;
    u32 height = 0;
};

ptk::uint applyVertex(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, const ptk::PtRawHit& h,
                      const ptk::float3&, ptk::float3& direct) {
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
    direct = ptk::float3(v.x, v.y, v.z);
    return 1u;
}

enum StageId : u32 {
    kStagePresample = 0u,
    kStageSurface = 1u,
    kStageInitial = 2u,
    kStageTemporal = 3u,
    kStageShade = 4u,
    kStageSpatial = 8u,
};

struct StageParams {
    const ptk::RdiCpuContext* rc = nullptr;
    const ptk::PtCpuContext* recordCtx = nullptr;
    const ptk::float4* params = nullptr;  ///< kPtParamWords (surface: the record-mode words)
    const ptk::float4* rparams = nullptr; ///< kRdiParamWords
    u32 stage = 0;
    u32 width = 0;
    u32 height = 0;
    u32 entries = 0;
    RecordEntry* stash = nullptr;
    ptk::float4* out = nullptr;
    ptk::float4* history = nullptr; ///< shade: the final reservoir copy
    ptk::uint* tiles = nullptr;
};

struct StageKernel {
    void operator()(const kernel::LaunchIndex& idx, const StageParams& p) const {
        const ptk::RdiCpuContext& rc = *p.rc;
        const ptk::PtParams P = ptk::ptParamsUnpack(p.params);
        if (p.stage == kStagePresample) {
            if (idx.global.x < p.entries) {
                p.tiles[idx.global.x] = ptk::rdiPresample(rc, P, idx.global.x);
            }
            return;
        }
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        if (x >= p.width || y >= p.height) {
            return;
        }
        const std::size_t pixel = std::size_t(y) * p.width + x;
        const ptk::RdiParams R = ptk::rdiParamsUnpack(p.rparams);
        if (p.stage == kStageSurface) {
            RecordEntry& e = p.stash[pixel];
            e.have = false;
            (void)ptk::ptRenderSample(*p.recordCtx, P, x, y, P.sampleBase);
            ptk::rdiSurfaceWords(rc, P, e.have, e.hit, e.dir, p.out[pixel * 4u], p.out[pixel * 4u + 1u],
                                 p.out[pixel * 4u + 2u], p.out[pixel * 4u + 3u]);
            return;
        }
        if (p.stage == kStageShade) {
            const ptk::RdiReservoir r = ptk::rdiLoadReservoir(rc, 0u, static_cast<ptk::uint>(pixel));
            p.out[pixel * 2u] = rc.surfaces[0][pixel * 4u];
            p.out[pixel * 2u + 1u] = ptk::rdiShade(rc, P, r, static_cast<ptk::uint>(pixel));
            p.history[pixel * 2u] = ptk::rdiReservoirWord0(r);
            p.history[pixel * 2u + 1u] = ptk::rdiReservoirWord1(r);
            return;
        }
        ptk::RdiReservoir r;
        if (p.stage == kStageInitial) {
            r = ptk::rdiInitial(rc, P, R, x, y);
        } else {
            r = ptk::rdiReuse(rc, P, R, x, y, p.stage == kStageTemporal);
        }
        p.out[pixel * 2u] = ptk::rdiReservoirWord0(r);
        p.out[pixel * 2u + 1u] = ptk::rdiReservoirWord1(r);
    }
};

template <typename T>
void sizeTo(std::vector<T>& v, std::size_t n, const T& fill) {
    if (v.size() != n) {
        v.assign(n, fill);
    }
}

} // namespace

RestirDiSettings RestirDiSettings::fromOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
    RestirDiSettings s;
    s.enabled = RestirDiOptions::useRTXDI();
    s.tileCandidates = static_cast<u32>(std::clamp(RestirDiOptions::initialSampleCount(), 0, 64));
    s.treeCandidates = static_cast<u32>(std::clamp(RestirDiOptions::lightTreeSampleCount(), 0, 64));
    s.bsdfCandidates = static_cast<u32>(std::clamp(RestirDiOptions::bsdfSampleCount(), 0, 16));
    s.spatialSamples = static_cast<u32>(std::clamp(RestirDiOptions::spatialSamples(), 0, int(kRdiMaxNeighbors)));
    s.spatialIterations = static_cast<u32>(std::clamp(RestirDiOptions::spatialIterations(), 0, 4));
    s.spatialRadius = std::clamp(RestirDiOptions::spatialRadius(), 1.f, 128.f);
    s.maxHistory = static_cast<float>(std::clamp(RestirDiOptions::maxHistoryLength(), 0, 1024));
    s.temporal = RestirDiOptions::enableTemporalReuse();
    s.initialVisibility = RestirDiOptions::enableInitialVisibility();
    s.unbiased = RestirDiOptions::enableRayTracedBiasCorrection();
    s.tileCount = static_cast<u32>(std::clamp(RestirDiOptions::lightTileCount(), 1, 4096));
    s.tileSize = static_cast<u32>(std::clamp(RestirDiOptions::lightTileSize(), 1, 4096));
    return s;
}

bool RestirDiLightTable::build(const PtCompiledScene& scene, float distantArea) {
    const std::vector<lk::RlLight>& lights = scene.lightRecords();
    const std::size_t n = lights.size();
    m_weights.resize(n);
    m_pmf.resize(n);
    m_cdf.resize(n);
    double total = 0.0;
    for (std::size_t l = 0; l < n; ++l) {
        const lk::RlLight& L = lights[l];
        const double lum = 0.2126 * L.radiance.x + 0.7152 * L.radiance.y + 0.0722 * L.radiance.z;
        const double area = L.kind == lk::kRlKindDistant ? double(distantArea) : double(L.area);
        double w = lum * area;
        if (!(w > 0.0) || !std::isfinite(w)) {
            w = 0.0;
        }
        m_weights[l] = w;
        total += w;
    }
    m_usable = n > 0u && total > 0.0 && std::isfinite(total);
    if (!m_usable) {
        std::fill(m_pmf.begin(), m_pmf.end(), 0.f);
        std::fill(m_cdf.begin(), m_cdf.end(), 1.f);
        return false;
    }
    double cum = 0.0;
    for (std::size_t l = 0; l < n; ++l) {
        cum += m_weights[l];
        m_cdf[l] = static_cast<float>(std::min(cum / total, 1.0));
    }
    m_cdf[n - 1u] = 1.f;
    // Exact bin widths of the 24-bit uniform the presample stage compares (u = k / 2^24, first l with u < cdf[l]).
    double prev = 0.0;
    for (std::size_t l = 0; l < n; ++l) {
        const double k = std::min(std::ceil(double(m_cdf[l]) * kTwo24), kTwo24);
        m_pmf[l] = static_cast<float>((k - prev) / kTwo24);
        prev = std::max(prev, k);
    }
    return true;
}

void packRestirDiParams(const RestirDiSettings& s, u32 flags, u32 tileCount, u32 iteration, Word* out) {
    out[0] = Word(float(flags), float(tileCount), float(s.tileSize), float(s.tileCandidates));
    out[1] = Word(float(s.treeCandidates), float(std::min(s.spatialSamples, kRdiMaxNeighbors)), float(iteration),
                  float(s.bsdfCandidates));
    out[2] = Word(s.spatialRadius, s.maxHistory, s.normalThreshold, s.depthThreshold);
    out[3] = Word(0.f, 0.f, 0.f, 0.f);
}

struct RestirDiCpu::Impl {
    std::vector<ptk::PtCpuTexture> textures;
    std::vector<RecordEntry> stash;
    ApplyState apply{};
    ptk::PtDiHook applyHook{};
    ptk::PtDiHook recordHook{};
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
        recordCtx.diHook = &recordHook;
        return true;
    }

    static bool launch(kernel::Backend backend, const char* name, kernel::Dim3 grid, const StageParams& p) {
        const kernel::Dim3 group = grid.y > 1u ? kernel::Dim3{8u, 8u, 1u} : kernel::Dim3{64u, 1u, 1u};
        return kernel::launch(backend, kernel::KernelLaunch{name, grid, group}, StageKernel{}, p).ok;
    }
};

RestirDiCpu::RestirDiCpu() : m_impl(new Impl) {}
RestirDiCpu::~RestirDiCpu() { delete m_impl; }

const ptk::PtDiHook* RestirDiCpu::hook() const { return &m_impl->applyHook; }

namespace {
u32 frameFlags(const RestirDiSettings& di, bool history) {
    return (di.unbiased ? kRdiFlagUnbiased : 0u) | (history ? kRdiFlagHistory : 0u) |
           (di.initialVisibility ? kRdiFlagInitialVisibility : 0u) | (di.temporal ? kRdiFlagTemporal : 0u);
}
} // namespace

bool RestirDiCpu::frame(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                        u32 frameSeed, u32 sampleBase, const RestirDiSettings& di, kernel::Backend backend) {
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
    sizeTo(m_surfaces[0], pixels * kRdiSurfaceWords, zero);
    sizeTo(m_surfaces[1], pixels * kRdiSurfaceWords, zero);
    sizeTo(m_resInitial, pixels * kRdiReservoirWords, zero);
    sizeTo(m_resA, pixels * kRdiReservoirWords, zero);
    sizeTo(m_resB, pixels * kRdiReservoirWords, zero);
    sizeTo(m_history, pixels * kRdiReservoirWords, zero);
    sizeTo(m_output, pixels * kRdiOutWords, zero);
    if (I.stash.size() != pixels) {
        I.stash.assign(pixels, RecordEntry{});
    }
    if (!I.bind(scene)) {
        return false;
    }
    const bool usable = m_table.build(scene, di.distantArea);
    const u32 tileCount = usable && di.tileCandidates > 0u ? di.tileCount : 0u;
    m_tiles.resize(std::size_t(tileCount) * di.tileSize);
    const u32 flags = frameFlags(di, m_historyValid);

    Word params[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, params);
    Word record[kPtParamWords];
    for (u32 k = 0; k < kPtParamWords; ++k) {
        record[k] = params[k];
    }
    const u32 recordFlags = (settings.flags | kPtFlagDiRecord) & ~(kPtFlagNee | kPtFlagRestirDi);
    record[5].w = float(recordFlags); // PtParams.flags (ptParamsUnpack)
    Word rparams[kRdiParamWords];
    packRestirDiParams(di, flags, tileCount, 0u, rparams);

    m_cur ^= 1u;
    ptk::RdiCpuContext rc{};
    rc.pt = &I.ctx;
    rc.surfaces[0] = m_surfaces[m_cur].data();
    rc.surfaces[1] = m_surfaces[m_cur ^ 1u].data();
    rc.tiles = m_tiles.data();
    rc.pmf = m_table.pmf().empty() ? nullptr : m_table.pmf().data();
    rc.cdf = m_table.cdf().empty() ? nullptr : m_table.cdf().data();

    StageParams p{};
    p.rc = &rc;
    p.recordCtx = &I.recordCtx;
    p.params = params;
    p.rparams = rparams;
    p.width = width;
    p.height = height;
    const kernel::Dim3 grid = kernel::extent2(width, height);
    bool ok = true;
    // presample
    if (tileCount > 0u) {
        p.stage = kStagePresample;
        p.entries = static_cast<u32>(m_tiles.size());
        p.tiles = m_tiles.data();
        ok = ok && Impl::launch(backend, "relight.restir_di.presample", kernel::extent1(p.entries), p);
    }
    // surface
    p.stage = kStageSurface;
    p.params = record;
    p.stash = I.stash.data();
    p.out = m_surfaces[m_cur].data();
    ok = ok && Impl::launch(backend, "relight.restir_di.surface", grid, p);
    p.params = params;
    // initial
    p.stage = kStageInitial;
    p.out = m_resInitial.data();
    ok = ok && Impl::launch(backend, "relight.restir_di.initial", grid, p);
    // temporal
    rc.reservoirs[0] = m_resInitial.data();
    rc.reservoirs[1] = m_history.data();
    p.stage = kStageTemporal;
    p.out = m_resA.data();
    ok = ok && Impl::launch(backend, "relight.restir_di.temporal", grid, p);
    // spatial
    std::vector<Word>* src = &m_resA;
    std::vector<Word>* dst = &m_resB;
    Word spatialParams[kRdiParamWords];
    for (u32 it = 0; it < di.spatialIterations && di.spatialSamples > 0u; ++it) {
        packRestirDiParams(di, flags, tileCount, it, spatialParams);
        p.rparams = spatialParams;
        rc.reservoirs[0] = src->data();
        p.stage = kStageSpatial;
        p.out = dst->data();
        ok = ok && Impl::launch(backend, "relight.restir_di.spatial", grid, p);
        std::swap(src, dst);
    }
    p.rparams = rparams;
    // shade (+ history)
    rc.reservoirs[0] = src->data();
    p.stage = kStageShade;
    p.out = m_output.data();
    p.history = m_history.data();
    ok = ok && Impl::launch(backend, "relight.restir_di.shade", grid, p);

    I.apply.output = m_output.data();
    I.apply.width = width;
    I.apply.height = height;
    I.applyHook.vertex = &applyVertex;
    I.applyHook.user = &I.apply;
    m_historyValid = ok;
    ++m_stats.frames;
    m_stats.history = (flags & kRdiFlagHistory) != 0u;
    m_stats.surfaces = 0;
    m_stats.samples = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        m_stats.surfaces += m_output[i * 2u + 1u].w > 0.5f ? 1u : 0u;
        m_stats.samples += m_history[i * 2u + 1u].x >= 0.f && m_history[i * 2u].w > 0.f ? 1u : 0u;
    }
    return ok;
}

bool RestirDiCpu::replayStage(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                              u32 frameSeed, u32 sampleBase, const RestirDiSettings& di, u32 flags, u32 stage,
                              u32 iteration, const std::vector<Word>& surfaces, const std::vector<Word>& previous,
                              const std::vector<Word>& source, const std::vector<Word>& history,
                              const std::vector<u32>& tiles, std::vector<Word>& out, kernel::Backend backend) {
    const std::size_t pixels = std::size_t(width) * height;
    if (!scene.valid() || pixels == 0u || surfaces.size() < pixels * kRdiSurfaceWords ||
        (stage != kStageInitial && source.size() < pixels * kRdiReservoirWords)) {
        return false;
    }
    Impl& I = *m_impl;
    if (!I.bind(scene)) {
        return false;
    }
    const bool usable = m_table.build(scene, di.distantArea);
    const u32 tileCount = usable && di.tileCandidates > 0u ? di.tileCount : 0u;
    if (tileCount > 0u && tiles.size() < std::size_t(tileCount) * di.tileSize) {
        return false;
    }
    Word params[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, params);
    Word rparams[kRdiParamWords];
    packRestirDiParams(di, flags, tileCount, iteration, rparams);
    out.assign(pixels * kRdiReservoirWords, Word(0.f, 0.f, 0.f, 0.f));
    ptk::RdiCpuContext rc{};
    rc.pt = &I.ctx;
    rc.surfaces[0] = surfaces.data();
    rc.surfaces[1] = previous.size() >= surfaces.size() ? previous.data() : surfaces.data();
    rc.reservoirs[0] = source.empty() ? nullptr : source.data();
    rc.reservoirs[1] = history.size() >= pixels * kRdiReservoirWords ? history.data() : rc.reservoirs[0];
    rc.tiles = tiles.empty() ? nullptr : tiles.data();
    rc.pmf = m_table.pmf().empty() ? nullptr : m_table.pmf().data();
    rc.cdf = m_table.cdf().empty() ? nullptr : m_table.cdf().data();
    StageParams p{};
    p.rc = &rc;
    p.params = params;
    p.rparams = rparams;
    p.width = width;
    p.height = height;
    p.stage = stage;
    p.out = out.data();
    return Impl::launch(backend, "relight.restir_di.replay", kernel::extent2(width, height), p);
}

} // namespace fuse::relight::render::pathtrace
