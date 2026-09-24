// FUSE Relight RL-1.3: CPU unit tests of the geometry capture (ctest rl_capture_geometry_unit) on
// synthetic buffers and draws:
//   * index rebasing (u16 / u32) and the memoizer (exact hits, overlap invalidation, buffer writes);
//   * GeometryCapture vs the RL-0.5 reference hash::computeDrawGeometryHashes on random draws
//     (every primitive type, u16 / u32 / non-indexed, negative base vertex, 1-2 streams, every hash
//     component), sync and on JobScheduler workers, with and without CPU mappings (buffer shadows)
//     and memoization;
//   * UP draws equal their buffer twins; the DrawIndexedPrimitiveUP index-offset quirk;
//   * texcoordIndex selection (binning, DISABLE break, unused textures, volumes / cubes, lightmaps,
//     TCI generation flags, pixel shaders);
//   * skinning rules, bone range and bone hash; bounding box (with NaN); status codes; options.
#include <fuse/relight/capture/geometry/geometry_capture.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                       \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                   \
            std::fprintf(stderr, __VA_ARGS__);                                                     \
            std::fprintf(stderr, "\n");                                                            \
        }                                                                                          \
    } while (0)

using namespace fuse::relight;
using namespace fuse::relight::capture::geometry;
using hash::HashComponent;

constexpr std::uint32_t kD3DFmtIndex16 = 101, kD3DFmtIndex32 = 102;
constexpr std::uint8_t kFloat2 = 1, kFloat3 = 2, kFloat4 = 3, kD3DColor = 4, kUByte4 = 5, kShort2 = 6;
constexpr std::uint8_t kUsagePosition = 0, kUsageBlendWeight = 1, kUsageBlendIndices = 2, kUsageNormal = 3,
                       kUsageTexcoord = 5, kUsagePositionT = 9, kUsageColor = 10;

hash::HashRule allComponents() { return hash::HashRule{(1u << hash::kHashComponentCount) - 1}; }

/// Owns the device-side state a synthetic draw points into.
struct Scene {
    static constexpr std::size_t kMargin = 4096; // negative base vertices stay inside the allocation

    struct Buffer {
        tap::ResourceId id = 0;
        std::vector<std::uint8_t> storage;
        std::size_t size = 0;
        std::uint8_t* data() { return storage.data() + kMargin; }
        const std::uint8_t* data() const { return storage.data() + kMargin; }
    };
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::array<std::uint32_t, tap::kRenderStateCount> renderStates{};
    std::uint32_t stages[tap::kTextureStageCount][32]{};
    float transforms[tap::kTransformCount][16]{};
    tap::DrawCall call;
    tap::DrawState state;

    Scene() {
        for (auto& m : transforms) {
            const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(m, id, sizeof(id));
        }
        for (std::uint32_t s = 0; s < tap::kTextureStageCount; ++s) {
            stages[s][tss::kTexCoordIndex - 1] = s; // D3D9 default: stage i uses texcoord set i
            stages[s][tss::kColorOp - 1] = s == 0 ? 4u : 1u;
            stages[s][tss::kColorArg1 - 1] = 2; // D3DTA_TEXTURE
            stages[s][tss::kColorArg2 - 1] = 1; // D3DTA_CURRENT
            stages[s][tss::kAlphaOp - 1] = s == 0 ? 2u : 1u;
            stages[s][tss::kAlphaArg1 - 1] = 2;
            stages[s][tss::kAlphaArg2 - 1] = 1;
            stages[s][tss::kColorArg0 - 1] = 1;
            stages[s][tss::kAlphaArg0 - 1] = 1;
            stages[s][tss::kResultArg - 1] = 1;
        }
        state.renderStates = renderStates.data();
        state.textureStageStates = stages;
        state.transforms = transforms;
    }

    Buffer& addBuffer(std::size_t size, tap::ResourceId id) {
        auto b = std::make_unique<Buffer>();
        b->id = id;
        b->size = size;
        b->storage.assign(size + 2 * kMargin, 0);
        buffers.push_back(std::move(b));
        return *buffers.back();
    }
    Buffer* buffer(tap::ResourceId id) {
        for (auto& b : buffers) {
            if (b->id == id) {
                return b.get();
            }
        }
        return nullptr;
    }

    void bindStream(std::uint32_t stream, const Buffer& b, std::uint32_t offset, std::uint32_t stride) {
        tap::StreamBinding& s = state.streams[stream];
        s.buffer = b.id;
        s.offset = offset;
        s.stride = stride;
        s.frequency = 1;
        s.base = b.data();
        s.bufferSize = std::uint32_t(b.size);
    }
    void bindIndices(const Buffer& b, std::uint32_t format) {
        state.indices.buffer = b.id;
        state.indices.format = format;
        state.indices.base = b.data();
        state.indices.bufferSize = std::uint32_t(b.size);
    }
    void addElement(std::uint16_t stream, std::uint16_t offset, std::uint8_t type, std::uint8_t usage,
                    std::uint8_t usageIndex = 0) {
        tap::VertexElement& e = state.elements[state.elementCount++];
        e.stream = stream;
        e.offset = offset;
        e.type = type;
        e.usage = usage;
        e.usageIndex = usageIndex;
    }
    /// Announce every buffer to a capture (create + one full write).
    void announce(GeometryCapture& capture) {
        for (auto& b : buffers) {
            tap::BufferDesc d;
            d.id = b->id;
            d.size = std::uint32_t(b->size);
            capture.onBufferCreate(d);
            tap::BufferWrite w;
            w.buffer = b->id;
            w.offset = 0;
            w.size = std::uint32_t(b->size);
            w.data = b->data();
            w.base = b->data();
            w.bufferSize = std::uint32_t(b->size);
            capture.onBufferWrite(w);
        }
    }
    /// The same draw without CPU mappings (the capture's shadows must be used).
    tap::DrawState unmapped() const {
        tap::DrawState s = state;
        for (auto& st : s.streams) {
            st.base = nullptr;
            st.bufferSize = 0;
        }
        s.indices.base = nullptr;
        s.indices.bufferSize = 0;
        return s;
    }
};

void fillRandom(std::uint8_t* p, std::size_t n, std::mt19937& rng) {
    std::uniform_real_distribution<float> coord(-50.f, 50.f);
    for (std::size_t i = 0; i + 4 <= n; i += 4) {
        const float f = (rng() % 7 == 0) ? float(int(rng() % 9) - 4) : coord(rng);
        std::memcpy(p + i, &f, 4);
    }
}

GeometryCaptureConfig syncConfig(hash::HashRule rule = allComponents()) {
    GeometryCaptureConfig c;
    c.generationRule = rule;
    c.asyncJobs = false;
    return c;
}

/// The RL-0.5 reference for a scene's draw.
std::optional<hash::DrawGeometryHashes> reference(const Scene& sc, hash::HashRule rule, std::int32_t texcoordUsageIndex) {
    hash::DrawGeometryInput in;
    in.primitiveType = hash::D3DPrimitiveType(sc.call.primitiveType);
    in.primitiveCount = sc.call.primitiveCount;
    const bool indexed = sc.call.call == tap::DrawCallType::DrawIndexedPrimitive;
    std::int64_t baseVertex = sc.call.call == tap::DrawCallType::DrawPrimitive ? std::int64_t(sc.call.startVertex)
                                                                              : std::int64_t(sc.call.baseVertex);
    if (indexed) {
        const std::uint32_t isize = sc.state.indices.format == kD3DFmtIndex32 ? 4 : 2;
        in.indexType = isize == 4 ? hash::IndexType::Uint32 : hash::IndexType::Uint16;
        in.indexData = static_cast<const std::uint8_t*>(sc.state.indices.base) + std::size_t(sc.call.startIndex) * isize;
    }
    const auto element = [&](const tap::VertexElement& e) {
        hash::DrawVertexElement out;
        const tap::StreamBinding& b = sc.state.streams[e.stream];
        out.data = static_cast<const std::uint8_t*>(b.base) + std::int64_t(b.offset) + baseVertex * std::int64_t(b.stride) +
                   e.offset;
        out.stride = b.stride;
        out.type = hash::D3DDeclType(e.type);
        out.stream = e.stream;
        return out;
    };
    for (std::uint32_t i = 0; i < sc.state.elementCount; ++i) {
        const tap::VertexElement& e = sc.state.elements[i];
        if ((e.usage == kUsagePosition || e.usage == kUsagePositionT) && e.usageIndex == 0) {
            in.position = element(e);
        } else if (e.usage == kUsageTexcoord && std::int32_t(e.usageIndex) == texcoordUsageIndex) {
            in.texcoord = element(e);
        } else if (e.usage == kUsageNormal && e.usageIndex == 0) {
            in.normal = element(e);
        } else if (e.usage == kUsageColor && e.usageIndex == 0) {
            in.color0 = element(e);
        }
    }
    return hash::computeDrawGeometryHashes(in, rule);
}

bool sameGeometry(const CapturedDraw& d, const hash::DrawGeometryHashes& ref, std::string& why) {
    const hash::DrawGeometryHashes got = d.geometryHashes();
    for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
        if (got.hashes.fields[i] != ref.hashes.fields[i]) {
            why = "component " + std::string(hash::hashComponentName(HashComponent(i)));
            return false;
        }
    }
    if (got.indexCount != ref.indexCount || got.vertexCount != ref.vertexCount || got.minIndex != ref.minIndex ||
        got.maxIndex != ref.maxIndex || got.topology != ref.topology || got.indexType != ref.indexType ||
        got.positionStride != ref.positionStride || got.rebasedIndices != ref.rebasedIndices) {
        why = "raster geometry fields";
        return false;
    }
    return true;
}

// ---- random draws ---------------------------------------------------------------------------------

struct RandomDraw {
    Scene scene;
    std::int32_t texcoordUsageIndex = 0;
};

std::unique_ptr<RandomDraw> makeRandomDraw(std::mt19937& rng, int variant) {
    auto rd = std::make_unique<RandomDraw>();
    Scene& sc = rd->scene;
    const std::uint32_t prim = 1 + rng() % 6;
    const int indexMode = variant % 3; // 0 u16, 1 u32, 2 non-indexed
    const std::uint32_t vertices = 40 + rng() % 200;
    const bool twoStreams = rng() % 2;
    const bool positionT = rng() % 5 == 0;

    // Stream 0: position (+ maybe everything else); stream 1: texcoords / normal / colour.
    std::uint16_t off0 = 0, off1 = 0;
    sc.addElement(0, off0, positionT ? kFloat4 : (rng() % 3 == 0 ? kFloat4 : kFloat3),
                  positionT ? kUsagePositionT : kUsagePosition);
    off0 += sc.state.elements[0].type == kFloat4 ? 16 : 12;
    const auto place = [&](std::uint8_t type, std::uint8_t usage, std::uint8_t usageIndex, std::uint16_t size) {
        if (twoStreams && rng() % 2) {
            sc.addElement(1, off1, type, usage, usageIndex);
            off1 = std::uint16_t(off1 + size);
        } else {
            sc.addElement(0, off0, type, usage, usageIndex);
            off0 = std::uint16_t(off0 + size);
        }
    };
    if (rng() % 3) {
        place(kFloat3, kUsageNormal, 0, 12);
    }
    if (rng() % 3) {
        place(kD3DColor, kUsageColor, 0, 4);
    }
    const int texcoords = int(rng() % 3);
    for (int t = 0; t < texcoords; ++t) {
        place(rng() % 4 == 0 ? kShort2 : kFloat2, kUsageTexcoord, std::uint8_t(t), rng() % 4 == 0 ? 4 : 8);
    }
    const std::uint32_t stride0 = off0 + 4 * (rng() % 3);
    const std::uint32_t stride1 = std::max<std::uint32_t>(off1, 4) + 4 * (rng() % 2);
    const std::uint32_t streamOffset0 = 4 * (rng() % 4);

    auto& vb0 = sc.addBuffer(streamOffset0 + std::size_t(stride0) * vertices, 1);
    fillRandom(vb0.data(), vb0.size, rng);
    sc.bindStream(0, vb0, streamOffset0, stride0);
    if (twoStreams) {
        auto& vb1 = sc.addBuffer(std::size_t(stride1) * vertices, 2);
        fillRandom(vb1.data(), vb1.size, rng);
        sc.bindStream(1, vb1, 0, stride1);
    }

    // Texcoord selection: stage 0 bound, reading texcoord set 0 or 1.
    rd->texcoordUsageIndex = std::int32_t(rng() % 2);
    sc.stages[0][tss::kTexCoordIndex - 1] = std::uint32_t(rd->texcoordUsageIndex);

    std::uint32_t primitiveCount = 1 + rng() % 40;
    sc.call.primitiveType = prim;
    if (indexMode == 2) {
        std::uint32_t count = hash::d3dVertexCount(hash::D3DPrimitiveType(prim), primitiveCount);
        while (count > vertices) {
            primitiveCount /= 2;
            count = hash::d3dVertexCount(hash::D3DPrimitiveType(prim), primitiveCount);
        }
        sc.call.call = tap::DrawCallType::DrawPrimitive;
        sc.call.primitiveCount = primitiveCount;
        sc.call.startVertex = rng() % (vertices - count + 1);
        return rd;
    }
    sc.call.call = tap::DrawCallType::DrawIndexedPrimitive;
    sc.call.primitiveCount = primitiveCount;
    const std::uint32_t indexCount = hash::d3dVertexCount(hash::D3DPrimitiveType(prim), primitiveCount);
    const std::uint32_t isize = indexMode == 1 ? 4 : 2;
    // Base vertex in [-16, 16]; indices chosen so that base + index lands in [0, vertices).
    const std::int32_t baseVertex = std::int32_t(rng() % 33) - 16;
    const std::uint32_t lo = std::uint32_t(std::max(0, -baseVertex)) + rng() % 8;
    const std::uint32_t hi = std::uint32_t(std::int32_t(vertices) - 1 - std::max(0, baseVertex));
    const std::uint32_t startIndex = rng() % 5;
    auto& ib = sc.addBuffer(std::size_t(startIndex + indexCount) * isize + isize * (rng() % 3), 3);
    for (std::uint32_t i = 0; i < startIndex + indexCount; ++i) {
        const std::uint32_t v = lo + rng() % (hi - lo + 1);
        if (isize == 2) {
            const auto v16 = std::uint16_t(v);
            std::memcpy(ib.data() + std::size_t(i) * 2, &v16, 2);
        } else {
            std::memcpy(ib.data() + std::size_t(i) * 4, &v, 4);
        }
    }
    sc.bindIndices(ib, isize == 4 ? kD3DFmtIndex32 : kD3DFmtIndex16);
    sc.call.baseVertex = baseVertex;
    sc.call.startIndex = startIndex;
    sc.call.minIndex = lo;
    sc.call.numVertices = hi - lo + 1;
    return rd;
}

void testRandomDrawsMatchReference(bool async) {
    std::mt19937 rng(async ? 0xC0FFEEu : 0x5EEDu);
    int compared = 0, skipped = 0;
    for (int n = 0; n < 600; ++n) {
        const auto rd = makeRandomDraw(rng, n);
        Scene& sc = rd->scene;
        for (const hash::HashRule rule : {allComponents(), hash::parseHashRule(hash::rules::kDefaultGenerationRuleString)}) {
            GeometryCaptureConfig cfg = syncConfig(rule);
            cfg.asyncJobs = async;
            cfg.indexBufferMemoization = n % 2 == 0;
            GeometryCapture cap(cfg);
            sc.announce(cap);
            const CapturedDrawPtr d = cap.capture(sc.call, sc.state);
            const auto ref = reference(sc, rule, rd->texcoordUsageIndex);
            CHECK(d->texcoord.texcoordIndex == std::uint32_t(rd->texcoordUsageIndex));
            if (!ref) {
                CHECK_MSG(!d->captured(), "draw %d captured where the reference skips", n);
                ++skipped;
                continue;
            }
            std::string why;
            CHECK_MSG(d->captured() && sameGeometry(*d, *ref, why), "draw %d (%s): %s", n,
                      async ? "async" : "sync", why.c_str());
            // The same draw without CPU mappings goes through the capture's buffer shadows.
            const CapturedDrawPtr shadowed = cap.capture(sc.call, sc.unmapped());
            CHECK_MSG(shadowed->captured() && sameGeometry(*shadowed, *ref, why), "draw %d via shadows: %s", n, why.c_str());
            ++compared;
        }
    }
    CHECK(compared > 900);
    std::printf("  random draws (%s): %d compared with hash::computeDrawGeometryHashes, %d skipped by both\n",
                async ? "JobScheduler" : "inline", compared, skipped);
}

// ---- index rebasing + memoization -------------------------------------------------------------------

void testRebase() {
    const std::uint16_t i16[] = {7, 9, 8, 7, 12, 9};
    const RebasedIndicesPtr r = rebaseIndices(i16, 6, 2);
    CHECK(r->minIndex() == 7 && r->maxIndex() == 12 && r->indexSize() == 2 && r->bytes().size() == 12);
    CHECK(r->at(0) == 0 && r->at(1) == 2 && r->at(4) == 5);
    CHECK((r->uniqueIndices() == std::vector<std::uint32_t>{0, 1, 2, 5}));
    CHECK(r->indicesHash() == hash::hashContiguousMemory(r->bytes().data(), 12));
    CHECK(r->legacyIndicesHash() == hash::hashIndicesLegacy(r->bytes().data(), 6, 2));

    const std::uint32_t i32[] = {70000, 70002, 70001};
    const RebasedIndicesPtr r32 = rebaseIndices(i32, 3, 4);
    CHECK(r32->minIndex() == 70000 && r32->maxIndex() == 70002 && r32->at(1) == 2 && r32->bytes().size() == 12);

    const std::uint16_t zero[] = {0, 3, 1};
    CHECK(rebaseIndices(zero, 3, 2)->bytes() == std::vector<std::uint8_t>({0, 0, 3, 0, 1, 0}));
}

void testMemoizer() {
    IndexRangeMemoizer memo;
    int computed = 0;
    const std::uint16_t data[64] = {5, 6, 7, 8, 9, 10};
    const auto compute = [&](std::size_t start, std::size_t size) {
        ++computed;
        return rebaseIndices(reinterpret_cast<const std::uint8_t*>(data) + start, std::uint32_t(size / 2), 2);
    };
    const RebasedIndicesPtr a = memo.memoize(0, 6, compute);
    const RebasedIndicesPtr b = memo.memoize(0, 6, compute);
    CHECK(a == b && computed == 1 && memo.hits() == 1 && memo.misses() == 1);
    // Same start, different size: a miss that replaces the entry.
    memo.memoize(0, 8, compute);
    CHECK(computed == 2 && memo.entryCount() == 1);
    memo.memoize(40, 6, compute);
    CHECK(memo.entryCount() == 2);
    memo.invalidate(42, 2); // overlaps [40, 46)
    CHECK(memo.entryCount() == 1);
    memo.invalidate(100, 4); // overlaps nothing
    CHECK(memo.entryCount() == 1);
    memo.invalidateAll();
    CHECK(memo.entryCount() == 0);

    // Through the capture: a draw hits; a write to the range, or a DISCARD, invalidates.
    Scene sc;
    auto& vb = sc.addBuffer(16 * 12, 1);
    std::mt19937 rng(3);
    fillRandom(vb.data(), vb.size, rng);
    auto& ib = sc.addBuffer(64, 2);
    const std::uint16_t idx[] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    std::memcpy(ib.data(), idx, sizeof(idx));
    sc.bindStream(0, vb, 0, 12);
    sc.bindIndices(ib, kD3DFmtIndex16);
    sc.addElement(0, 0, kFloat3, kUsagePosition);
    sc.call.call = tap::DrawCallType::DrawIndexedPrimitive;
    sc.call.primitiveType = 4;
    sc.call.primitiveCount = 2;
    GeometryCapture cap(syncConfig());
    sc.announce(cap);
    CHECK(!cap.capture(sc.call, sc.state)->indicesMemoized);
    CHECK(cap.capture(sc.call, sc.state)->indicesMemoized);
    sc.call.startIndex = 6; // a different range of the same buffer
    CHECK(!cap.capture(sc.call, sc.state)->indicesMemoized);
    CHECK(cap.capture(sc.call, sc.state)->indicesMemoized);
    sc.call.startIndex = 0;
    CHECK(cap.capture(sc.call, sc.state)->indicesMemoized);

    const auto write = [&](std::uint32_t offset, std::uint32_t size, std::uint32_t flags) {
        tap::BufferWrite w;
        w.buffer = ib.id;
        w.offset = offset;
        w.size = size;
        w.lockFlags = flags;
        w.data = ib.data() + offset;
        w.base = ib.data();
        w.bufferSize = std::uint32_t(ib.size);
        cap.onBufferWrite(w);
    };
    write(40, 8, 0); // outside both cached ranges ([0,12) and [12,24))
    CHECK(cap.capture(sc.call, sc.state)->indicesMemoized);
    write(0, 2, kD3DLockReadOnly); // read-only locks do not invalidate
    CHECK(cap.capture(sc.call, sc.state)->indicesMemoized);
    // Change index 0 and report the write: the next draw must see the new data.
    const std::uint16_t changed = 3;
    std::memcpy(ib.data(), &changed, 2);
    write(0, 2, 0);
    const CapturedDrawPtr d = cap.capture(sc.call, sc.state);
    CHECK(!d->indicesMemoized && d->minIndex == 1 && d->indices->at(0) == 2);
    sc.call.startIndex = 6;
    CHECK(cap.capture(sc.call, sc.state)->indicesMemoized); // [12,24) survived the range write
    write(0, 4, kD3DLockDiscard);
    CHECK(!cap.capture(sc.call, sc.state)->indicesMemoized); // DISCARD drops every range
    CHECK(cap.stats().memoHits == 6 && cap.stats().memoMisses == 4 && cap.stats().draws == 0);
}

// ---- UP draws ------------------------------------------------------------------------------------

void testUpDraws() {
    std::mt19937 rng(11);
    // DrawPrimitiveUP == DrawPrimitive over the same bytes at StartVertex 0.
    Scene sc;
    const std::uint32_t stride = 24, count = 6; // strip of 4 triangles
    auto& vb = sc.addBuffer(stride * count, 1);
    fillRandom(vb.data(), vb.size, rng);
    sc.addElement(0, 0, kFloat3, kUsagePosition);
    sc.addElement(0, 12, kFloat2, kUsageTexcoord);
    sc.addElement(0, 20, kD3DColor, kUsageColor);
    sc.bindStream(0, vb, 0, stride);
    sc.call.call = tap::DrawCallType::DrawPrimitive;
    sc.call.primitiveType = 5;
    sc.call.primitiveCount = 4;
    GeometryCapture cap(syncConfig());
    sc.announce(cap);
    const CapturedDrawPtr buffered = cap.capture(sc.call, sc.state);

    tap::DrawCall up = sc.call;
    up.call = tap::DrawCallType::DrawPrimitiveUP;
    up.upVertexData = vb.data();
    up.upVertexStride = stride;
    up.upVertexBytes = stride * count;
    tap::DrawState upState = sc.state;
    upState.streams[0] = tap::StreamBinding{}; // UP draws do not use the stream 0 binding
    const CapturedDrawPtr upDraw = cap.capture(up, upState);
    CHECK(upDraw->captured() && buffered->captured());
    CHECK(upDraw->hashes.get().fields == buffered->hashes.get().fields);
    CHECK(upDraw->vertexCount == 6 && upDraw->indexCount == 0 && upDraw->indexType == 0);

    // DrawIndexedPrimitiveUP == DrawIndexedPrimitive (BaseVertexIndex 0, StartIndex 0).
    Scene ix;
    auto& vb2 = ix.addBuffer(16 * 10, 1);
    fillRandom(vb2.data(), vb2.size, rng);
    auto& ib2 = ix.addBuffer(12, 2);
    const std::uint16_t idx[] = {4, 5, 6, 6, 5, 7};
    std::memcpy(ib2.data(), idx, sizeof(idx));
    ix.addElement(0, 0, kFloat3, kUsagePosition);
    ix.addElement(0, 12, kD3DColor, kUsageColor);
    ix.bindStream(0, vb2, 0, 16);
    ix.bindIndices(ib2, kD3DFmtIndex16);
    ix.call.call = tap::DrawCallType::DrawIndexedPrimitive;
    ix.call.primitiveType = 4;
    ix.call.primitiveCount = 2;
    ix.call.minIndex = 4;
    ix.call.numVertices = 4;
    GeometryCapture cap2(syncConfig());
    ix.announce(cap2);
    const CapturedDrawPtr dip = cap2.capture(ix.call, ix.state);
    tap::DrawCall iup = ix.call;
    iup.call = tap::DrawCallType::DrawIndexedPrimitiveUP;
    iup.upVertexData = vb2.data();
    iup.upVertexStride = 16;
    iup.upVertexBytes = 16 * 8; // MinVertexIndex + NumVertices vertices
    iup.upIndexData = idx;
    iup.upIndexFormat = kD3DFmtIndex16;
    iup.upIndexBytes = sizeof(idx);
    const CapturedDrawPtr dipUp = cap2.capture(iup, ix.state);
    CHECK(dipUp->captured() && dip->captured());
    CHECK(dipUp->hashes.get().fields == dip->hashes.get().fields);
    CHECK(dipUp->minIndex == 4 && dipUp->maxIndex == 7 && dipUp->vertexIndexOffset == 4 && !dipUp->indicesMemoized);

    // Quirk: stream 0 of the declaration (FLOAT4 at offset 12 = 28 bytes) is wider than the stride
    // (16): GetUPBufferSize pads 12 zero bytes after the vertices, the indices go after the padding,
    // and Remix reads them at GetUPDataSize - i.e. starting with 6 zero indices' worth of padding.
    Scene q;
    q.addElement(0, 0, kFloat3, kUsagePosition);
    q.addElement(0, 12, kFloat4, kUsageTexcoord);
    std::vector<std::uint8_t> verts(16 * 8);
    fillRandom(verts.data(), verts.size(), rng);
    const std::uint16_t qidx[] = {1, 2, 3, 4, 5, 6, 7, 1, 2};
    tap::DrawCall qc;
    qc.call = tap::DrawCallType::DrawIndexedPrimitiveUP;
    qc.primitiveType = 4;
    qc.primitiveCount = 3;
    qc.minIndex = 0;
    qc.numVertices = 8;
    qc.upVertexData = verts.data();
    qc.upVertexStride = 16;
    qc.upVertexBytes = std::uint32_t(verts.size());
    qc.upIndexData = qidx;
    qc.upIndexFormat = kD3DFmtIndex16;
    qc.upIndexBytes = sizeof(qidx);
    GeometryCapture cap3(syncConfig());
    const CapturedDrawPtr qd = cap3.capture(qc, q.state);
    // Read indices: 6 zero indices (12 bytes of padding), then qidx[0..2].
    CHECK(qd->captured() && qd->indexCount == 9 && qd->minIndex == 0 && qd->maxIndex == 3);
    CHECK(qd->indices->at(5) == 0 && qd->indices->at(6) == 1 && qd->indices->at(8) == 3);
}

// ---- texcoord selection ---------------------------------------------------------------------------

void testTexcoordSelection() {
    Scene sc;
    TexcoordSelectInput in;
    in.textureStageStates = sc.stages;
    const StageTexture tex2d{true, 3, true, false};

    // Nothing bound: firstStage 0, stage 0's index (0).
    TexcoordSelection s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 0 && s.texcoordIndex == 0 && s.colorTextureStages[0] == kInvalidStage);

    // Stage 0 bound, reading texcoord set 1.
    sc.stages[0][tss::kTexCoordIndex - 1] = 1;
    in.textures[0] = tex2d;
    s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 0 && s.texcoordIndex == 1 && s.usageIndex() == 1 && s.colorTextureStages[0] == 0);

    // Lowest texcoord index wins across stages: stage 0 reads set 3, stage 1 reads set 1.
    sc.stages[0][tss::kTexCoordIndex - 1] = 3;
    sc.stages[1][tss::kTexCoordIndex - 1] = 1;
    sc.stages[1][tss::kColorOp - 1] = 4; // MODULATE(texture, current)
    in.textures[1] = tex2d;
    s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 1 && s.texcoordIndex == 1 && s.colorTextureStages[0] == 1 && s.colorTextureStages[1] == 0);

    // Stage 0 COLOROP DISABLE stops the scan: no texture, firstStage 0.
    sc.stages[0][tss::kColorOp - 1] = 1;
    s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 0 && s.texcoordIndex == 3 && s.colorTextureStages[0] == kInvalidStage);
    sc.stages[0][tss::kColorOp - 1] = 4;

    // A stage whose ops do not read the texture is skipped (SELECTARG2 of CURRENT).
    sc.stages[1][tss::kColorOp - 1] = 3;
    sc.stages[1][tss::kAlphaOp - 1] = 3;
    s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 0 && s.texcoordIndex == 3);
    // ... unless an argument selects the texture (arg2 = D3DTA_TEXTURE | D3DTA_COMPLEMENT).
    sc.stages[1][tss::kColorArg2 - 1] = 2 | 0x10;
    s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 1);
    sc.stages[1][tss::kColorOp - 1] = 1;
    sc.stages[1][tss::kAlphaOp - 1] = 1;

    // Volumes are skipped; cubes only with rtx.allowCubemaps; lightmaps are skipped.
    in.textures[0] = StageTexture{true, 4, true, false};
    CHECK(selectTexcoordIndex(in).colorTextureStages[0] == kInvalidStage);
    in.textures[0] = StageTexture{true, 5, true, false};
    CHECK(selectTexcoordIndex(in).colorTextureStages[0] == kInvalidStage);
    in.allowCubemaps = true;
    CHECK(selectTexcoordIndex(in).colorTextureStages[0] == 0);
    in.allowCubemaps = false;
    in.textures[0] = StageTexture{true, 3, true, true};
    CHECK(selectTexcoordIndex(in).colorTextureStages[0] == kInvalidStage);

    // Texture 0 without an image hash is reported.
    in.textures[0] = StageTexture{true, 3, false, false};
    CHECK(selectTexcoordIndex(in).texture0HashMissing);
    in.textures[0] = tex2d;
    CHECK(!selectTexcoordIndex(in).texture0HashMissing);

    // TCI generation flags: the raw value is kept and selects no stream.
    sc.stages[0][tss::kTexCoordIndex - 1] = 0x00010000u | 1u; // D3DTSS_TCI_CAMERASPACENORMAL | 1
    s = selectTexcoordIndex(in);
    CHECK(s.texcoordIndex == 0x00010001u && s.usageIndex() == -1);

    // Pixel shader: firstStage 0 regardless of the stage states.
    sc.stages[0][tss::kTexCoordIndex - 1] = 2;
    sc.stages[0][tss::kColorOp - 1] = 1;
    in.fixedFunctionPixel = false;
    s = selectTexcoordIndex(in);
    CHECK(s.firstStage == 0 && s.texcoordIndex == 2 && s.colorTextureStages[0] == 0);

    // End to end: TEXCOORD[selected] feeds the texcoords hash; generation flags give the "no
    // texcoord stream" hash (XXH3 of 0 bytes per unique index).
    Scene d;
    auto& vb = d.addBuffer(28 * 4, 1);
    std::mt19937 rng(5);
    fillRandom(vb.data(), vb.size, rng);
    d.addElement(0, 0, kFloat3, kUsagePosition);
    d.addElement(0, 12, kFloat2, kUsageTexcoord, 0);
    d.addElement(0, 20, kFloat2, kUsageTexcoord, 1);
    d.bindStream(0, vb, 0, 28);
    d.call.call = tap::DrawCallType::DrawPrimitive;
    d.call.primitiveType = 6;
    d.call.primitiveCount = 2;
    d.state.textures[0] = 42;
    GeometryCaptureConfig cfg = syncConfig();
    cfg.textureHashKnown = [](tap::ResourceId) { return true; };
    GeometryCapture cap(cfg);
    tap::TextureDesc td;
    td.id = 42;
    td.type = 3;
    cap.onTextureCreate(td);
    d.announce(cap);
    for (std::uint32_t tci : {0u, 1u, 0x00030000u}) {
        d.stages[0][tss::kTexCoordIndex - 1] = tci;
        const CapturedDrawPtr got = cap.capture(d.call, d.state);
        const auto ref = reference(d, allComponents(), tci <= 15 ? std::int32_t(tci) : -1);
        std::string why;
        CHECK_MSG(got->captured() && ref && sameGeometry(*got, *ref, why), "tci %u: %s", tci, why.c_str());
        CHECK(got->vertices.texcoord.defined() == (tci <= 15));
    }
}

// ---- skinning --------------------------------------------------------------------------------------

void testSkinning() {
    Scene sc;
    // POSITION, BLENDWEIGHT (FLOAT2), BLENDINDICES (UBYTE4): 24-byte vertices.
    const std::uint32_t stride = 24, count = 5;
    auto& vb = sc.addBuffer(stride * count, 1);
    std::mt19937 rng(9);
    fillRandom(vb.data(), vb.size, rng);
    const std::uint8_t boneIdx[count][4] = {{3, 4, 5, 0}, {3, 3, 3, 9}, {6, 4, 3, 0}, {4, 5, 6, 0}, {3, 6, 5, 0}};
    for (std::uint32_t v = 0; v < count; ++v) {
        std::memcpy(vb.data() + v * stride + 20, boneIdx[v], 4);
    }
    sc.addElement(0, 0, kFloat3, kUsagePosition);
    sc.addElement(0, 12, kFloat2, kUsageBlendWeight);
    sc.addElement(0, 20, kUByte4, kUsageBlendIndices);
    sc.bindStream(0, vb, 0, stride);
    sc.call.call = tap::DrawCallType::DrawPrimitive;
    sc.call.primitiveType = 5;
    sc.call.primitiveCount = 3;
    for (std::uint32_t n = 0; n < 10; ++n) {
        for (int k = 0; k < 16; ++k) {
            sc.transforms[tap::kTransformWorld0 + n][k] = float(n * 100 + k) * 0.5f;
        }
    }
    GeometryCapture cap(syncConfig());
    sc.announce(cap);

    const auto skin = [&]() { return cap.capture(sc.call, sc.state); };
    CHECK(!skin()->skinning.valid()); // D3DVBF_DISABLE

    // 2 weights (3 bones per vertex), indexed: bytes 0..2 of each vertex -> min 3, max 6.
    sc.renderStates[kD3DRSVertexBlend] = kD3DVbf2Weights;
    sc.renderStates[kD3DRSIndexedVertexBlendEnable] = 1;
    CapturedDrawPtr d = skin();
    CHECK(d->skinning.valid());
    const SkinningData& s = d->skinning.get();
    CHECK(s.numBonesPerVertex == 3 && s.minBoneIndex == 3 && s.numBones == 7 && s.boneMatrices.size() == 7);
    CHECK(s.boneHash == hash::xxh3_64(sc.transforms[tap::kTransformWorld0 + 3], 4 * sizeof(Matrix4)));
    CHECK(std::memcmp(s.boneMatrices[6].data(), sc.transforms[tap::kTransformWorld0 + 6], 64) == 0);
    CHECK(s.blendIndices.defined() && s.blendWeights.defined());

    // Not indexed: numBones = bones per vertex, from WORLDMATRIX(0).
    sc.renderStates[kD3DRSIndexedVertexBlendEnable] = 0;
    d = skin();
    CHECK(d->skinning.valid() && d->skinning.get().numBones == 3 && d->skinning.get().minBoneIndex == 0);
    CHECK(d->skinning.get().boneHash == hash::xxh3_64(sc.transforms[tap::kTransformWorld0], 3 * sizeof(Matrix4)));

    // 3 weights, indexed: 4 bytes per vertex -> max 9.
    sc.renderStates[kD3DRSVertexBlend] = kD3DVbf3Weights;
    sc.renderStates[kD3DRSIndexedVertexBlendEnable] = 1;
    d = skin();
    CHECK(d->skinning.get().numBones == 10 && d->skinning.get().minBoneIndex == 0);

    // 0WEIGHTS needs indexed blending; tweening has 0 bones per vertex.
    sc.renderStates[kD3DRSVertexBlend] = kD3DVbf0Weights;
    CHECK(skin()->skinning.get().numBonesPerVertex == 1 && skin()->skinning.get().minBoneIndex == 3);
    sc.renderStates[kD3DRSIndexedVertexBlendEnable] = 0;
    CHECK(!skin()->skinning.valid());
    sc.renderStates[kD3DRSVertexBlend] = kD3DVbfTweening;
    d = skin();
    CHECK(d->skinning.valid() && d->skinning.get().numBones == 0 && d->skinning.get().boneHash == 0);

    // A programmable VS never skins; weights without a BLENDWEIGHT stream give nothing.
    sc.renderStates[kD3DRSVertexBlend] = kD3DVbf1Weights;
    sc.state.vertexShader.id = 7;
    CHECK(!skin()->skinning.valid());
    sc.state.vertexShader.id = 0;
    sc.state.elements[1].usage = kUsageNormal; // no BLENDWEIGHT any more
    CHECK(!skin()->skinning.valid());

    // getMinMaxBoneIndices edge case.
    int lo = 0, hi = 0;
    CHECK(!minMaxBoneIndices(nullptr, 4, 0, 2, lo, hi));
}

// ---- bounding box, statuses, async, options -------------------------------------------------------

void testBoundingBoxAndStatus() {
    Scene sc;
    auto& vb = sc.addBuffer(12 * 4, 1);
    const float p[12] = {1, -2, 3, -4, 5, -6, 0.5f, 0.25f, 9, 2, 2, std::numeric_limits<float>::quiet_NaN()};
    std::memcpy(vb.data(), p, sizeof(p));
    sc.addElement(0, 0, kFloat3, kUsagePosition);
    sc.bindStream(0, vb, 0, 12);
    sc.call.call = tap::DrawCallType::DrawPrimitive;
    sc.call.primitiveType = 6;
    sc.call.primitiveCount = 2;
    GeometryCapture cap(syncConfig());
    sc.announce(cap);
    CapturedDrawPtr d = cap.capture(sc.call, sc.state);
    const BoundingBox& b = d->boundingBox.get();
    CHECK(b.minPos[0] == -4 && b.minPos[1] == -2 && b.maxPos[0] == 2 && b.maxPos[1] == 5);
    // z: the last vertex is NaN, which replaces both running values (MINPS / MAXPS semantics).
    CHECK(std::isnan(b.minPos[2]) && std::isnan(b.maxPos[2]));

    // Out of bounds: StartVertex past the end; negative window.
    sc.call.startVertex = 1;
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::OutOfBounds);
    sc.call.startVertex = 0;
    sc.state.streams[0].offset = 4;
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::OutOfBounds);
    sc.state.streams[0].offset = 0;

    // No position; no vertices.
    sc.state.elements[0].usage = kUsageNormal;
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::NoPosition);
    sc.state.elements[0].usage = kUsagePosition;
    sc.call.primitiveCount = 0;
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::NoVertices);
    sc.call.primitiveCount = 2;

    // Degenerate indices (max == min) and a negative base vertex below the buffer.
    auto& ib = sc.addBuffer(12, 2);
    const std::uint16_t same[] = {2, 2, 2, 2, 2, 2};
    std::memcpy(ib.data(), same, sizeof(same));
    sc.bindIndices(ib, kD3DFmtIndex16);
    sc.announce(cap);
    sc.call.call = tap::DrawCallType::DrawIndexedPrimitive;
    sc.call.primitiveType = 4;
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::DegenerateIndices);
    const std::uint16_t tri[] = {1, 2, 3, 3, 2, 1};
    std::memcpy(ib.data(), tri, sizeof(tri));
    tap::BufferWrite w;
    w.buffer = ib.id;
    w.size = 12;
    w.data = ib.data();
    w.base = ib.data();
    w.bufferSize = 12;
    cap.onBufferWrite(w);
    sc.call.baseVertex = -1; // window starts at vertex 0: fine
    d = cap.capture(sc.call, sc.state);
    CHECK(d->captured() && d->vertexIndexOffset == 0 && d->vertexCount == 3);
    sc.call.baseVertex = -2; // window starts at vertex -1: out of bounds
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::OutOfBounds);
    sc.call.baseVertex = 0;
    sc.call.startIndex = 4; // indices past the end of the index buffer
    CHECK(cap.capture(sc.call, sc.state)->status == CaptureStatus::OutOfBounds);
    sc.call.startIndex = 0;
    tap::DrawState noIb = sc.unmapped();
    noIb.indices.buffer = 99;
    CHECK(cap.capture(sc.call, noIb)->status == CaptureStatus::NoIndexData);

    // onDraw records (frame, draw index); the sink sees every draw; Raster is always returned.
    std::vector<CapturedDrawPtr> seen;
    cap.setDrawSink([&](const CapturedDrawPtr& p) { seen.push_back(p); });
    CHECK(cap.onDraw(sc.call, sc.state) == tap::DrawDecision::Raster);
    cap.onPresent(tap::FrameEvent{});
    CHECK(cap.onDraw(sc.call, sc.state) == tap::DrawDecision::Raster);
    CHECK(cap.onDraw(sc.call, sc.state) == tap::DrawDecision::Raster);
    CHECK(seen.size() == 3 && seen[0]->frame == 0 && seen[1]->frame == 1 && seen[2]->drawIndex == 1);
    CHECK(cap.stats().draws == 3 && cap.stats().captured == 3);
}

void testVertexShaderHook() {
    Scene sc;
    auto& vb = sc.addBuffer(12 * 3, 1);
    std::mt19937 rng(1);
    fillRandom(vb.data(), vb.size, rng);
    sc.addElement(0, 0, kFloat3, kUsagePosition);
    sc.bindStream(0, vb, 0, 12);
    sc.call.call = tap::DrawCallType::DrawPrimitive;
    sc.call.primitiveType = 4;
    sc.call.primitiveCount = 1;
    sc.state.vertexShader.id = 3;
    GeometryCaptureConfig cfg = syncConfig();
    cfg.vertexShaderHash = [](const tap::DrawState&) { return std::optional<hash::Hash64>(0x1234); };
    GeometryCapture cap(cfg);
    sc.announce(cap);
    CHECK(cap.capture(sc.call, sc.state)->hashes.get()[HashComponent::VertexShader] == 0x1234);
    // Upstream gates the component on the geometrydescriptor bit of the generation rule.
    cfg.generationRule = hash::parseHashRule("positions,vertexshader");
    GeometryCapture gated(cfg);
    gated.onBufferCreate(tap::BufferDesc{});
    CHECK(gated.capture(sc.call, sc.state)->hashes.get()[HashComponent::VertexShader] == 0);
    // Fixed function: never.
    sc.state.vertexShader.id = 0;
    CHECK(cap.capture(sc.call, sc.state)->hashes.get()[HashComponent::VertexShader] == 0);
}

void testOptionsAndConfig() {
    const GeometryCaptureConfig c = GeometryCaptureConfig::fromOptions();
    CHECK(c.generationRule == hash::parseHashRule(hash::rules::kDefaultGenerationRuleString));
    CHECK(c.assetRule == hash::parseHashRule(hash::rules::kDefaultAssetRuleString));
    CHECK(c.indexBufferMemoization);
    CHECK(captureStatusName(CaptureStatus::OutOfBounds) == "out_of_bounds");
    // JobFuture: empty, ready and scheduled futures.
    CHECK(!JobFuture<int>().valid());
    CHECK(JobFuture<int>::fromValue(5).get() == 5);
    CHECK(JobFuture<int>::schedule([] { return 7; }, false).get() == 7);
}

} // namespace

int main() {
    std::printf("rl_capture_geometry_unit\n");
    testRebase();
    testMemoizer();
    testUpDraws();
    testTexcoordSelection();
    testSkinning();
    testBoundingBoxAndStatus();
    testVertexShaderHook();
    testOptionsAndConfig();
    testRandomDrawsMatchReference(false);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.initialize(2);
    testRandomDrawsMatchReference(true);
    // Many in-flight jobs: every future resolves to the inline result.
    {
        std::mt19937 rng(77);
        std::vector<std::unique_ptr<RandomDraw>> draws;
        std::vector<CapturedDrawPtr> async, inline_;
        GeometryCaptureConfig acfg = syncConfig();
        acfg.asyncJobs = true;
        GeometryCapture a(acfg), s(syncConfig());
        for (int i = 0; i < 200; ++i) {
            draws.push_back(makeRandomDraw(rng, i));
            draws.back()->scene.announce(a);
            draws.back()->scene.announce(s);
            async.push_back(a.capture(draws.back()->scene.call, draws.back()->scene.state));
            inline_.push_back(s.capture(draws.back()->scene.call, draws.back()->scene.state));
        }
        int same = 0;
        for (std::size_t i = 0; i < async.size(); ++i) {
            if (!async[i]->captured()) {
                continue;
            }
            CHECK(async[i]->hashes.get().fields == inline_[i]->hashes.get().fields);
            CHECK(std::memcmp(&async[i]->boundingBox.get(), &inline_[i]->boundingBox.get(), sizeof(BoundingBox)) == 0);
            ++same;
        }
        CHECK(same > 100);
    }
    scheduler.shutdown();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
