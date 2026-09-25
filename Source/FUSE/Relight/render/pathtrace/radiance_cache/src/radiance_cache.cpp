// FUSE Relight RL-5.4: the hash-grid radiance cache - options, parameters and the CPU reference (see
// radiance_cache.hpp; the single-source core is kernels/radiance_cache_core.h / radiance_cache_path.h).
#include <fuse/relight/render/pathtrace/radiance_cache.hpp>

#include <fuse/relight/render/material/bsdf_host.hpp>

#include "radiance_cache_cpp.hpp"

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace fuse::relight::render::pathtrace {

// The public mirrors (radiance_cache.hpp) == the single-source core's constants.
static_assert(kPtFlagRadianceCache == ptk::kPtFlagRadianceCache && kPtFlagRcTrain == ptk::kPtFlagRcTrain, "flags");
static_assert(kRcParamWords == ptk::kRcParamWords && kRcSlotWords == ptk::kRcSlotWords &&
                  kRcHeaderWords == ptk::kRcHeaderWords && kRcRecordWords == ptk::kRcRecordWords &&
                  kRcMaxVertices == ptk::kRcMaxVertices && kRcFixedScale == ptk::kRcFixedScale,
              "record sizes");
static_assert(kRcCounterQueries == ptk::kRcCounterQueries && kRcCounterHits == ptk::kRcCounterHits &&
                  kRcCounterInserts == ptk::kRcCounterInserts && kRcCounterDropped == ptk::kRcCounterDropped &&
                  kRcCounterLive == ptk::kRcCounterLive && kRcCounterEvicted == ptk::kRcCounterEvicted &&
                  kRcCounterFresh == ptk::kRcCounterFresh && kRcFlagStats == ptk::kRcFlagStats,
              "counters");
static_assert(kRcParamWords * 4u <= ptk::kRcCounterQueries, "header: params before the counters");

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &RadianceCacheOptions::enable,       &RadianceCacheOptions::capacityLog2, &RadianceCacheOptions::cellSize,
    &RadianceCacheOptions::cellPixels,   &RadianceCacheOptions::trainStride,  &RadianceCacheOptions::minSamples,
    &RadianceCacheOptions::maxSamples,   &RadianceCacheOptions::maxAge,       &RadianceCacheOptions::minRoughness,
    &RadianceCacheOptions::spreadThreshold,
};

constexpr u32 kTrainSampleOffset = 0x40000000u; ///< training paths' sample index = sampleBase + this (decorrelated)

ptk::RcParams unpackParams(const Word* w) { return ptk::rcParamsUnpack(reinterpret_cast<const ptk::float4*>(w)); }

void bindContext(const PtCompiledScene& scene, std::vector<ptk::PtCpuTexture>& textures, ptk::PtCpuContext& ctx) {
    const material::AlbedoLut& lut = material::sharedAlbedoLut();
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
}

// --- hooks -------------------------------------------------------------------------------------------------------

struct TrainUser {
    ptk::RcParams R{};
    ptk::RcCpuContext rc{};
    ptk::RcPathState* states = nullptr; ///< per tile
};

ptk::RcPathState& trainState(TrainUser& u, ptk::uint px, ptk::uint py) { return u.states[ptk::rcCpuTile(u.rc, px, py)]; }

ptk::uint trainVertex(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, ptk::uint bounce,
                      ptk::uint vflags, const ptk::PtSurface& S, const ptk::float3& n, const ptk::float3& d, float tHit,
                      float prevPdf, const ptk::float3& thr, const ptk::float3& acc, ptk::float3& cached) {
    TrainUser& u = *static_cast<TrainUser*>(user);
    return ptk::rcHookVertex(u.rc, P, u.R, trainState(u, px, py), px, py, bounce, vflags, S, n, d, tHit, prevPdf, thr,
                             acc, cached);
}

void trainEnd(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, const ptk::float3& acc) {
    TrainUser& u = *static_cast<TrainUser*>(user);
    ptk::rcHookEnd(u.rc, P, u.R, trainState(u, px, py), px, py, acc);
}

struct QueryUser {
    ptk::RcParams R{};
    ptk::RcCpuContext rc{};
    ptk::RcPathState* states = nullptr; ///< per pixel
    u32 width = 0;
    u32 height = 0;
    RadianceCacheLookup lookup{};
};

ptk::uint queryVertex(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, ptk::uint bounce,
                      ptk::uint vflags, const ptk::PtSurface& S, const ptk::float3& n, const ptk::float3& d, float tHit,
                      float prevPdf, const ptk::float3& thr, const ptk::float3& acc, ptk::float3& cached) {
    QueryUser& u = *static_cast<QueryUser*>(user);
    cached = ptk::float3(0.f, 0.f, 0.f);
    if (P.width != u.width || P.height != u.height || u.states == nullptr) {
        return 0u;
    }
    ptk::RcPathState& st = u.states[std::size_t(py) * u.width + px];
    if (ptk::rcHookStep(u.rc, P, u.R, st, px, py, bounce, vflags, S, n, d, tHit, prevPdf, thr, acc) == 0u) {
        return 0u;
    }
    if (u.lookup.fn == nullptr) {
        return ptk::rcHookQuery(u.rc, u.R, S.position, n, cached);
    }
    const float p[3] = {S.position.x, S.position.y, S.position.z};
    const float nn[3] = {n.x, n.y, n.z};
    float out[3] = {0.f, 0.f, 0.f};
    const bool hit = u.lookup.fn(u.lookup.user, p, nn, out);
    if ((u.R.flags & ptk::kRcFlagStats) != 0u) {
        ptk::rcTableAdd(u.rc, ptk::kRcCounterQueries, 1u);
        if (hit) {
            ptk::rcTableAdd(u.rc, ptk::kRcCounterHits, 1u);
        }
    }
    if (!hit) {
        return 0u;
    }
    cached = ptk::float3(out[0], out[1], out[2]);
    return 1u;
}

void queryEnd(void* user, const ptk::PtParams& P, ptk::uint px, ptk::uint py, const ptk::float3& acc) {
    QueryUser& u = *static_cast<QueryUser*>(user);
    if (P.width != u.width || P.height != u.height || u.states == nullptr) {
        return;
    }
    ptk::rcHookEnd(u.rc, P, u.R, u.states[std::size_t(py) * u.width + px], px, py, acc);
}

// --- kernels -----------------------------------------------------------------------------------------------------

struct TrainParams {
    const ptk::PtCpuContext* ctx = nullptr;
    const ptk::float4* params = nullptr;  ///< the training PtParams words
    const ptk::float4* rparams = nullptr; ///< RcParams words
    u32 width = 0;
    u32 height = 0;
    u32 tiles = 0;
};

struct TrainKernel {
    void operator()(const kernel::LaunchIndex& idx, const TrainParams& p) const {
        const u32 t = idx.global.x;
        if (t >= p.tiles) {
            return;
        }
        const ptk::PtParams P = ptk::ptParamsUnpack(p.params);
        const ptk::RcParams R = ptk::rcParamsUnpack(p.rparams);
        ptk::uint px = 0;
        ptk::uint py = 0;
        (void)ptk::rcTrainPixel(R, p.width, p.height, t, px, py);
        (void)ptk::ptRenderSample(*p.ctx, P, px, py, P.sampleBase + kTrainSampleOffset);
    }
};

struct TableParams {
    ptk::RcCpuContext rc{};
    const ptk::float4* rparams = nullptr;
    const ptk::float4* records = nullptr;
    u32 count = 0;
    bool resolve = false;
};

struct TableKernel {
    void operator()(const kernel::LaunchIndex& idx, const TableParams& p) const {
        const u32 i = idx.global.x;
        if (i >= p.count) {
            return;
        }
        const ptk::RcParams R = ptk::rcParamsUnpack(p.rparams);
        if (p.resolve) {
            (void)ptk::rcResolveSlot(p.rc, R, i);
        } else {
            (void)ptk::rcUpdateRecord(p.rc, R, p.records[i * 3u], p.records[i * 3u + 1u], p.records[i * 3u + 2u]);
        }
    }
};

bool launchTable(kernel::Backend backend, const char* name, const TableParams& p) {
    if (p.count == 0u) {
        return true;
    }
    return kernel::launch(backend, kernel::KernelLaunch{name, kernel::extent1(p.count), kernel::Dim3{64u, 1u, 1u}},
                          TableKernel{}, p)
        .ok;
}

u32 roundCapacity(u32 c) {
    const u32 clamped = std::clamp(c, 64u, 1u << 24);
    return std::bit_ceil(clamped);
}

} // namespace

// --- settings / params ---------------------------------------------------------------------------------------------

RadianceCacheSettings RadianceCacheSettings::fromOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
    RadianceCacheSettings s;
    s.enabled = RadianceCacheOptions::enable();
    s.capacity = 1u << std::clamp(RadianceCacheOptions::capacityLog2(), 6, 24);
    s.cellSize = std::clamp(RadianceCacheOptions::cellSize(), 1e-5f, 1e5f);
    s.cellPixels = std::clamp(RadianceCacheOptions::cellPixels(), 0.25f, 1024.f);
    s.trainStride = static_cast<u32>(std::clamp(RadianceCacheOptions::trainStride(), 1, 64));
    s.minSamples = std::clamp(RadianceCacheOptions::minSamples(), 1.f, 65536.f);
    s.maxSamples = std::clamp(RadianceCacheOptions::maxSamples(), 1.f, 1e7f);
    s.maxAge = static_cast<u32>(std::clamp(RadianceCacheOptions::maxAge(), 0, 1 << 20));
    s.minRoughness = std::clamp(RadianceCacheOptions::minRoughness(), 0.f, 1.f);
    s.spreadThreshold = std::clamp(RadianceCacheOptions::spreadThreshold(), 0.f, 1e6f);
    return s;
}

void packRadianceCacheParams(const RadianceCacheSettings& s, const Word* pt, u32 frame, Word* out) {
    const float ux = pt[2].x, uy = pt[2].y, uz = pt[2].z; // camUp x tan(fovY / 2)
    const float tanHalf = std::sqrt(ux * ux + uy * uy + uz * uz);
    const float height = std::max(pt[1].w, 1.f);
    const float cell = std::max(s.cellSize, 1e-6f);
    const u32 flags = s.stats ? kRcFlagStats : 0u;
    out[0] = Word(pt[0].x, pt[0].y, pt[0].z, 2.f * tanHalf / height);
    out[1] = Word(cell, 1.f / cell, std::max(s.cellPixels, 1e-3f), float(roundCapacity(s.capacity)));
    out[2] = Word(float(std::clamp(s.maxProbe, 1u, 64u)), float(std::max(s.trainStride, 1u)), float(frame & 0xFFFFFFu),
                  float(flags));
    out[3] = Word(std::max(s.minSamples, 1e-3f), s.spreadThreshold, std::max(s.maxSamples, 1.f), s.maxRadiance);
    out[4] = Word(float(std::min(s.maxAge, 0xFFFFFFu)), float(std::min(s.minBounce, 255u)), s.minRoughness,
                  float(std::clamp(s.maxVertices, 1u, kRcMaxVertices)));
}

void radianceCacheTrainParams(const Word* pt, Word* out) {
    for (u32 k = 0; k < kPtParamWords; ++k) {
        out[k] = pt[k];
    }
    const u32 flags = static_cast<u32>(pt[5].w);
    const u32 off = kPtFlagRadianceCache | ptk::kPtFlagRestirDi | ptk::kPtFlagDiRecord | ptk::kPtFlagRestirGi |
                    ptk::kPtFlagGiRecord;
    out[5].w = float((flags | kPtFlagRcTrain) & ~off); // PtParams.flags (ptParamsUnpack)
    out[10].z = 0.f;                                   // no table address: training never queries
    out[10].w = 0.f;
}

u32 radianceCacheTiles(const RadianceCacheSettings& s, u32 width, u32 height) {
    const u32 st = std::max(s.trainStride, 1u);
    return ((width + st - 1u) / st) * ((height + st - 1u) / st);
}

u64 radianceCacheTableWords(const RadianceCacheSettings& s) {
    return u64(kRcHeaderWords) + u64(roundCapacity(s.capacity)) * kRcSlotWords;
}

RadianceCacheStats radianceCacheCounters(const u32* t) {
    RadianceCacheStats s;
    if (t == nullptr) {
        return s;
    }
    s.queries = t[kRcCounterQueries];
    s.hits = t[kRcCounterHits];
    s.inserts = t[kRcCounterInserts];
    s.dropped = t[kRcCounterDropped];
    s.live = t[kRcCounterLive];
    s.evicted = t[kRcCounterEvicted];
    s.fresh = t[kRcCounterFresh];
    return s;
}

RadianceCacheCell radianceCacheCell(const Word* rcParams, const float p[3], const float n[3]) {
    const ptk::RcParams R = unpackParams(rcParams);
    const ptk::RcKey k = ptk::rcCellKey(R, ptk::float3(p[0], p[1], p[2]), ptk::float3(n[0], n[1], n[2]));
    return RadianceCacheCell{k.check, k.slot, k.level, k.bin};
}

bool radianceCacheLookup(const Word* rcParams, const u32* table, const float p[3], const float n[3], float out[3],
                         float* samples) {
    const ptk::RcParams R = unpackParams(rcParams);
    ptk::RcCpuContext rc{};
    rc.table = const_cast<u32*>(table); // rcLookup only loads
    ptk::float3 L(0.f, 0.f, 0.f);
    float count = 0.f;
    const bool hit = ptk::rcLookup(rc, R, ptk::float3(p[0], p[1], p[2]), ptk::float3(n[0], n[1], n[2]), L, count);
    out[0] = L.x;
    out[1] = L.y;
    out[2] = L.z;
    if (samples != nullptr) {
        *samples = count;
    }
    return hit;
}

void radianceCacheEntries(const u32* table, u32 capacity, std::vector<RadianceCacheEntry>& out) {
    out.clear();
    for (u32 s = 0; s < capacity; ++s) {
        const u32* w = table + kRcHeaderWords + std::size_t(s) * kRcSlotWords;
        if (w[0] == 0u) {
            continue;
        }
        RadianceCacheEntry e;
        e.check = w[0];
        std::memcpy(e.words, w + 1, sizeof(e.words));
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [](const RadianceCacheEntry& a, const RadianceCacheEntry& b) { return a.check < b.check; });
}

// --- RadianceCacheCpu ----------------------------------------------------------------------------------------------

struct RadianceCacheCpu::Impl {
    std::vector<ptk::PtCpuTexture> textures;
    ptk::PtCpuContext ctx{};
    std::vector<ptk::RcPathState> trainStates;
    std::vector<ptk::RcPathState> queryStates;
    TrainUser train{};
    QueryUser query{};
    ptk::PtRcHook trainHook{};
    ptk::PtRcHook queryHook{};
    Word trainParams[kPtParamWords] = {};
};

RadianceCacheCpu::RadianceCacheCpu() : m_impl(new Impl) { configure(RadianceCacheSettings{}); }
RadianceCacheCpu::~RadianceCacheCpu() { delete m_impl; }

bool RadianceCacheCpu::configure(const RadianceCacheSettings& settings) {
    RadianceCacheSettings s = settings;
    s.capacity = roundCapacity(s.capacity);
    s.maxVertices = std::clamp(s.maxVertices, 1u, kRcMaxVertices);
    s.trainStride = std::max(s.trainStride, 1u);
    const bool resize = s.capacity != m_settings.capacity || m_table.empty();
    m_settings = s;
    if (resize) {
        m_table.assign(radianceCacheTableWords(s), 0u);
        m_frames = 0;
    }
    return true;
}

void RadianceCacheCpu::clear() {
    std::fill(m_table.begin(), m_table.end(), 0u);
    m_frames = 0;
}

bool RadianceCacheCpu::train(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                             u32 frameSeed, u32 sampleBase, kernel::Backend backend) {
    if (!scene.valid() || width == 0u || height == 0u || !material::sharedAlbedoLut().valid()) {
        return false;
    }
    Impl& I = *m_impl;
    m_width = width;
    m_height = height;
    Word pt[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, pt);
    packRadianceCacheParams(m_settings, pt, m_frame, m_params);
    ++m_frame;
    radianceCacheTrainParams(pt, I.trainParams);
    const u32 tiles = radianceCacheTiles(m_settings, width, height);
    const std::size_t recordWords = std::size_t(tiles) * kRcMaxVertices * kRcRecordWords;
    if (m_records.size() != recordWords) {
        m_records.assign(recordWords, Word(0.f, 0.f, 0.f, 0.f));
    }
    const std::size_t pathWords = std::size_t(tiles) * kRcMaxVertices * 3u;
    if (m_paths.size() != pathWords) {
        m_paths.assign(pathWords, Word(0.f, 0.f, 0.f, 0.f));
    }
    if (I.trainStates.size() != tiles) {
        I.trainStates.assign(tiles, ptk::RcPathState{});
    }
    bindContext(scene, I.textures, I.ctx);
    I.train.R = unpackParams(m_params);
    I.train.rc.table = m_table.data();
    I.train.rc.records = reinterpret_cast<ptk::float4*>(m_records.data());
    I.train.rc.paths = reinterpret_cast<ptk::float4*>(m_paths.data());
    I.train.rc.width = width;
    I.train.rc.stride = m_settings.trainStride;
    I.train.states = I.trainStates.data();
    I.trainHook.vertex = &trainVertex;
    I.trainHook.end = &trainEnd;
    I.trainHook.user = &I.train;
    I.ctx.rcHook = &I.trainHook;
    // Header: the frame's params and zeroed counters (the GPU's train stage, item 0).
    ptk::rcWriteHeader(I.train.rc, reinterpret_cast<const ptk::float4*>(m_params));
    TrainParams p{};
    p.ctx = &I.ctx;
    p.params = reinterpret_cast<const ptk::float4*>(I.trainParams);
    p.rparams = reinterpret_cast<const ptk::float4*>(m_params);
    p.width = width;
    p.height = height;
    p.tiles = tiles;
    return kernel::launch(backend,
                          kernel::KernelLaunch{"relight.radiance_cache.train", kernel::extent1(tiles),
                                               kernel::Dim3{64u, 1u, 1u}},
                          TrainKernel{}, p)
        .ok;
}

bool RadianceCacheCpu::update(kernel::Backend backend) {
    TableParams p{};
    p.rc.table = m_table.data();
    p.rparams = reinterpret_cast<const ptk::float4*>(m_params);
    p.records = reinterpret_cast<const ptk::float4*>(m_records.data());
    p.count = static_cast<u32>(m_records.size() / kRcRecordWords);
    return launchTable(backend, "relight.radiance_cache.update", p);
}

bool RadianceCacheCpu::resolve(kernel::Backend backend) {
    TableParams p{};
    p.rc.table = m_table.data();
    p.rparams = reinterpret_cast<const ptk::float4*>(m_params);
    p.count = m_settings.capacity;
    p.resolve = true;
    const bool ok = launchTable(backend, "relight.radiance_cache.resolve", p);
    ++m_frames;
    return ok;
}

bool RadianceCacheCpu::frame(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                             u32 frameSeed, u32 sampleBase, kernel::Backend backend) {
    return train(scene, settings, width, height, frameSeed, sampleBase, backend) && update(backend) &&
           resolve(backend);
}

bool RadianceCacheCpu::replayUpdate(const Word* rcParams, const Word* records, u32 recordCount,
                                    std::vector<u32>& table, kernel::Backend backend) const {
    const ptk::RcParams R = unpackParams(rcParams);
    if (table.size() < u64(kRcHeaderWords) + u64(R.capacity) * kRcSlotWords || !std::has_single_bit(R.capacity)) {
        return false;
    }
    TableParams p{};
    p.rc.table = table.data();
    p.rparams = reinterpret_cast<const ptk::float4*>(rcParams);
    p.records = reinterpret_cast<const ptk::float4*>(records);
    p.count = recordCount;
    return launchTable(backend, "relight.radiance_cache.replay_update", p);
}

bool RadianceCacheCpu::replayResolve(const Word* rcParams, std::vector<u32>& table, kernel::Backend backend) const {
    const ptk::RcParams R = unpackParams(rcParams);
    if (table.size() < u64(kRcHeaderWords) + u64(R.capacity) * kRcSlotWords || !std::has_single_bit(R.capacity)) {
        return false;
    }
    TableParams p{};
    p.rc.table = table.data();
    p.rparams = reinterpret_cast<const ptk::float4*>(rcParams);
    p.count = R.capacity;
    p.resolve = true;
    return launchTable(backend, "relight.radiance_cache.replay_resolve", p);
}

const ptk::PtRcHook* RadianceCacheCpu::hook(u32 width, u32 height) {
    Impl& I = *m_impl;
    const std::size_t pixels = std::size_t(width) * height;
    if (I.queryStates.size() != pixels) {
        I.queryStates.assign(pixels, ptk::RcPathState{});
    }
    I.query.R = unpackParams(m_params);
    I.query.rc.table = m_table.data();
    I.query.states = I.queryStates.data();
    I.query.width = width;
    I.query.height = height;
    I.query.lookup = m_lookup;
    I.queryHook.vertex = &queryVertex;
    I.queryHook.end = &queryEnd;
    I.queryHook.user = &I.query;
    return &I.queryHook;
}

bool RadianceCacheCpu::adopt(const Word* rcParams, const u32* table, u64 words) {
    const ptk::RcParams R = unpackParams(rcParams);
    if (table == nullptr || !std::has_single_bit(R.capacity) ||
        words < u64(kRcHeaderWords) + u64(R.capacity) * kRcSlotWords) {
        return false;
    }
    m_settings.capacity = R.capacity;
    m_table.assign(table, table + words);
    for (u32 k = 0; k < kRcParamWords; ++k) {
        m_params[k] = rcParams[k];
    }
    return true;
}

RadianceCacheStats RadianceCacheCpu::stats() const {
    RadianceCacheStats s = radianceCacheCounters(m_table.empty() ? nullptr : m_table.data());
    s.frames = m_frames;
    for (std::size_t i = 0; i < m_records.size(); i += kRcRecordWords) {
        s.records += m_records[i].w > 0.5f ? 1u : 0u;
    }
    return s;
}

} // namespace fuse::relight::render::pathtrace
