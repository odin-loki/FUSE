// FUSE Relight RL-1.4: unit tests of texture tracking and hashing (ctest rl_capture_texture_unit),
// and the replay driver the Wine tests use (--replay, see rl_capture_texture_run.py).
//
// Unit checks: canonical repack (padded pitches, odd widths, block formats, partial rectangles),
// first-upload hashing (DEFAULT at unlock, MANAGED at the first sampling draw, subresource 0
// only), rtx.useObsoleteHashOnTextureUpload, which resources are hashed at all, UpdateTexture /
// UpdateSurface inheritance (full-size rule, source subresource 0, rectangle policy),
// rtx.recomputeTextureHashOnWrite with the keep lists, render-target counter and descriptor hashes,
// the external image registry (generations, release on destroy, reuse), and the rtx.* options.
#include <fuse/relight/capture/texture/canonical_mip0.hpp>
#include <fuse/relight/capture/texture/external_image_registry.hpp>
#include <fuse/relight/capture/texture/texture_options.hpp>
#include <fuse/relight/capture/texture/texture_tracker.hpp>
#include <fuse/relight/options/options.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::relight;
using namespace fuse::relight::capture::texture;
using hash::D3DFormat;
using hash::Hash64;

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

constexpr std::uint32_t kDefault = 0, kManaged = 1, kSystemMem = 2, kScratch = 3;
constexpr std::uint32_t kSurface = 1, kVolume = 4, kTexture = 3, kCube = 5;
constexpr std::uint32_t kRT = 1, kDS = 2;

std::uint32_t fmt(D3DFormat f) { return std::uint32_t(f); }

tap::TextureDesc desc(tap::ResourceId id, std::uint32_t type, std::uint32_t w, std::uint32_t h, D3DFormat f,
                      std::uint32_t pool, std::uint32_t usage = 0, std::uint32_t levels = 1) {
    tap::TextureDesc d;
    d.id = id;
    d.type = type;
    d.width = w;
    d.height = h;
    d.depth = 1;
    d.mipLevels = levels;
    d.arraySize = type == kCube ? 6 : 1;
    d.format = fmt(f);
    d.usage = usage;
    d.pool = pool;
    d.vkImage = (pool == kSystemMem || pool == kScratch) ? 0 : 0x10000 + id;
    return d;
}

std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = std::uint8_t(seed + i * 7 + (i >> 5));
    }
    return v;
}

/// A full lock + unlock of (face 0,) `level`, as the tap reports it: `rows` rows `pitch` apart.
void write(TextureTracker& t, tap::ResourceId id, std::uint32_t level, const std::vector<std::uint8_t>& data,
           std::uint32_t pitch, std::uint32_t rows, std::uint32_t face = 0) {
    tap::TextureWriteLock l;
    l.texture = id;
    l.face = face;
    l.level = level;
    t.onTextureWriteLock(l);
    tap::TextureUpload u;
    u.texture = id;
    u.face = face;
    u.level = level;
    u.data = data.data();
    u.rowPitch = pitch;
    u.slicePitch = pitch * rows;
    u.rows = rows;
    u.fullUpdate = true;
    t.onTextureUpload(u);
}

void writeRect(TextureTracker& t, tap::ResourceId id, const std::vector<std::uint8_t>& data, std::uint32_t pitch,
               tap::Box box) {
    tap::TextureWriteLock l;
    l.texture = id;
    t.onTextureWriteLock(l);
    tap::TextureUpload u;
    u.texture = id;
    u.data = data.data();
    u.rowPitch = pitch;
    u.fullUpdate = false;
    u.box = box;
    t.onTextureUpload(u);
}

/// A draw with `textures` bound (slot index), fixed function unless shaders are given.
void draw(TextureTracker& t, std::vector<std::pair<std::uint32_t, tap::ResourceId>> textures,
          tap::ResourceId ps = 0, tap::ResourceId vs = 0, int disabledStage = -1) {
    static std::uint32_t tss[tap::kTextureStageCount][32];
    for (std::uint32_t s = 0; s < tap::kTextureStageCount; ++s) {
        tss[s][0] = int(s) == disabledStage ? 1u : 4u; // COLOROP: DISABLE or MODULATE
    }
    tap::DrawState st;
    st.textureStageStates = tss;
    st.pixelShader.id = ps;
    st.vertexShader.id = vs;
    for (auto [slot, id] : textures) {
        st.textures[slot] = id;
    }
    t.onDraw(tap::DrawCall{}, st);
}

Hash64 xxh3Of(const std::vector<std::uint8_t>& v) { return hash::hashTextureMip0(v.data(), v.size()); }

void copy(TextureTracker& t, tap::CopyMethod m, tap::ResourceId src, tap::ResourceId dst, std::uint32_t srcLevel = 0,
          bool rect = false) {
    tap::TextureCopy c;
    c.method = m;
    c.source = src;
    c.destination = dst;
    c.sourceLevel = srcLevel;
    c.hasSourceRect = rect;
    t.onTextureCopy(c);
}

// ---- canonical repack ----------------------------------------------------------------------------

void testCanonicalRepack() {
    // A8R8G8B8 8x8 through a padded pitch of 48 bytes: hash = XXH3 of the tight 32-byte rows.
    {
        const auto tight = pattern(8 * 32, 1);
        std::vector<std::uint8_t> padded(8 * 48, 0xEE);
        for (int r = 0; r < 8; ++r) {
            std::memcpy(padded.data() + r * 48, tight.data() + r * 32, 32);
        }
        CanonicalMip0 m(D3DFormat::A8R8G8B8, 8, 8);
        CHECK(m.layout().rowBytes == 32 && m.layout().rowCount == 8);
        m.writeFull(padded.data(), 48, 8);
        CHECK(m.bytes() == tight);
        CHECK(m.hash(false) == xxh3Of(tight));
        CHECK(m.hash(true) == hash::hashTextureMip0Obsolete(tight.data(), tight.size()));
    }
    // R5G6B5 5x3: rows are align(10, 4) = 12 bytes; a tight 10-byte source pitch zero-pads each row.
    {
        const auto src = pattern(3 * 10, 9);
        CanonicalMip0 m(D3DFormat::R5G6B5, 5, 3);
        CHECK(m.layout().rowBytes == 12 && m.layout().size == 36);
        m.writeFull(src.data(), 10, 3);
        CHECK(m.bytes() == hash::packTextureMip0(m.layout(), src.data(), 10));
        CHECK(m.bytes()[10] == 0 && m.bytes()[11] == 0);
    }
    // DXT1 8x8 = 2x2 blocks of 8 bytes; DXT5 6x6 = 2x2 blocks of 16 bytes.
    {
        CanonicalMip0 d1(D3DFormat::DXT1, 8, 8);
        CHECK(d1.layout().rowBytes == 16 && d1.layout().rowCount == 2);
        CanonicalMip0 d5(D3DFormat::DXT5, 6, 6);
        CHECK(d5.layout().rowBytes == 32 && d5.layout().rowCount == 2);
        const auto blocks = pattern(64, 3);
        d5.writeFull(blocks.data(), 32, 2);
        CHECK(d5.bytes() == blocks);
    }
    // Partial rectangles land at their block offset; the rest keeps earlier content.
    {
        CanonicalMip0 m(D3DFormat::A8R8G8B8, 8, 8);
        const auto base = pattern(256, 5);
        m.writeFull(base.data(), 32, 8);
        const std::vector<std::uint8_t> rect(2 * 8, 0xFF); // 2x2 texels, pitch 8
        CHECK(m.writeRect(rect.data(), 8, TexelRect{2, 2, 4, 4}));
        auto expect = base;
        for (int y = 2; y < 4; ++y) {
            std::memset(expect.data() + y * 32 + 8, 0xFF, 8);
        }
        CHECK(m.bytes() == expect);
        CHECK(!m.writeRect(rect.data(), 8, TexelRect{4, 4, 4, 6})); // empty
        CHECK(!m.writeRect(rect.data(), 8, TexelRect{8, 8, 12, 12})); // outside
    }
    {
        CanonicalMip0 m(D3DFormat::DXT1, 8, 8);
        const std::vector<std::uint8_t> block(8, 0xAB);
        CHECK(m.writeRect(block.data(), 16, TexelRect{4, 4, 8, 8}));
        std::vector<std::uint8_t> expect(32, 0);
        std::memset(expect.data() + 16 + 8, 0xAB, 8);
        CHECK(m.bytes() == expect);
    }
    // Planar NV12: both planes (min(planeCount, 2)) are rows of the canonical buffer.
    {
        CanonicalMip0 m(D3DFormat::NV12, 4, 4);
        CHECK(m.layout().planes == 2);
        CHECK(m.layout().rowCount == 2 * m.layout().blocksHigh);
        const auto all = pattern(std::size_t(m.layout().size), 11);
        m.writeFull(all.data(), std::size_t(m.layout().rowBytes), m.layout().rowCount);
        CHECK(m.bytes() == all);
    }
    // A never-written buffer hashes as zeros (DXVK zero-initialises staging buffers).
    {
        CanonicalMip0 m(D3DFormat::L8, 4, 4);
        CHECK(!m.allocated());
        CHECK(m.hash(false) == xxh3Of(std::vector<std::uint8_t>(16, 0)));
        CHECK(m.allocated());
    }
}

// ---- first-upload hashing ------------------------------------------------------------------------

TextureTrackerConfig testConfig(std::atomic<std::uint32_t>* counter) {
    TextureTrackerConfig c;
    c.renderTargetCounter = counter;
    return c;
}

void testDefaultPoolFirstUpload() {
    std::atomic<std::uint32_t> counter{0};
    TextureTracker t(testConfig(&counter));
    t.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault, 0, 4));
    const auto a = pattern(256, 1), b = pattern(256, 2);
    // Other subresources do not hash.
    write(t, 1, 1, pattern(64, 3), 16, 4);
    CHECK(t.imageHash(1) == 0);
    write(t, 1, 0, a, 32, 8);
    CHECK(t.imageHash(1) == xxh3Of(a));
    auto s = t.find(1);
    CHECK(s && s->origin == HashOrigin::Upload && s->hashCount == 1 && !s->obsoleteHash && s->hashable);
    // Set once: a later write does not change it.
    write(t, 1, 0, b, 32, 8);
    CHECK(t.imageHash(1) == xxh3Of(a));
    CHECK(t.find(1)->hashCount == 1);
    // An empty rectangle is not a dirty box: no flush, no hash.
    t.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    writeRect(t, 2, a, 32, tap::Box{3, 3, 3, 5, 0, 1});
    CHECK(t.imageHash(2) == 0);
    // A partial first write hashes the whole (zero-initialised) buffer.
    writeRect(t, 2, std::vector<std::uint8_t>(8, 0x7F), 8, tap::Box{0, 0, 2, 1, 0, 1});
    std::vector<std::uint8_t> expect(256, 0);
    std::memset(expect.data(), 0x7F, 8);
    CHECK(t.imageHash(2) == xxh3Of(expect));
}

void testManagedHashedAtFirstDraw() {
    std::atomic<std::uint32_t> counter{0};
    TextureTracker t(testConfig(&counter));
    t.onTextureCreate(desc(3, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    const auto a = pattern(256, 1), b = pattern(256, 2), c = pattern(256, 3);
    write(t, 3, 0, a, 32, 8);
    CHECK(t.imageHash(3) == 0);
    CHECK(t.find(3)->flushPending);
    CHECK(t.previewImageHash(3) == xxh3Of(a));
    write(t, 3, 0, b, 32, 8); // before the first draw: still part of the hash
    // A draw that does not sample it (stage 1 behind a disabled stage 0) does not upload it.
    draw(t, {{1, 3}}, 0, 0, 0);
    CHECK(t.imageHash(3) == 0);
    draw(t, {{0, 3}});
    CHECK(t.imageHash(3) == xxh3Of(b));
    CHECK(!t.find(3)->flushPending);
    write(t, 3, 0, c, 32, 8);
    draw(t, {{0, 3}});
    CHECK(t.imageHash(3) == xxh3Of(b));
    CHECK(t.find(3)->hashCount == 1);

    // Pixel shaders sample every bound pixel slot; vertex slots only with a vertex shader.
    t.onTextureCreate(desc(4, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    t.onTextureCreate(desc(5, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    write(t, 4, 0, a, 32, 8);
    write(t, 5, 0, c, 32, 8);
    draw(t, {{5, 4}, {16, 5}}, /*ps*/ 7, /*vs*/ 0);
    CHECK(t.imageHash(4) == xxh3Of(a));
    CHECK(t.imageHash(5) == 0);
    draw(t, {{17, 5}}, 0, /*vs*/ 9);
    CHECK(t.imageHash(5) == xxh3Of(c));

    // Explicit flushes.
    t.onTextureCreate(desc(6, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    t.onTextureCreate(desc(7, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    write(t, 6, 0, a, 32, 8);
    write(t, 7, 0, b, 32, 8);
    t.flushManaged(6);
    CHECK(t.imageHash(6) == xxh3Of(a) && t.imageHash(7) == 0);
    t.flushAllPending();
    CHECK(t.imageHash(7) == xxh3Of(b));

    // A draw of a managed texture whose subresource 0 was never locked clears NeedsUpload; a later
    // write re-arms it.
    t.onTextureCreate(desc(8, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    draw(t, {{0, 8}});
    CHECK(t.imageHash(8) == 0);
    write(t, 8, 0, a, 32, 8);
    draw(t, {{0, 8}});
    CHECK(t.imageHash(8) == xxh3Of(a));

    // d3d9.evictManagedOnUnlock: managed textures flush at unlock.
    TextureTrackerConfig evict = testConfig(&counter);
    evict.evictManagedOnUnlock = true;
    TextureTracker te(evict);
    te.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    write(te, 1, 0, a, 32, 8);
    CHECK(te.imageHash(1) == xxh3Of(a));
}

void testObsoleteHash() {
    std::atomic<std::uint32_t> counter{0};
    TextureTrackerConfig c = testConfig(&counter);
    c.useObsoleteHashOnTextureUpload = true;
    TextureTracker t(c);
    const auto a = pattern(256, 4);
    t.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    t.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    write(t, 1, 0, a, 32, 8);
    write(t, 2, 0, a, 32, 8);
    draw(t, {{0, 1}});
    // Managed textures still "need upload" when flushed: XXH64. DEFAULT never does: XXH3.
    CHECK(t.imageHash(1) == hash::hashTextureMip0Obsolete(a.data(), a.size()));
    CHECK(t.find(1)->obsoleteHash);
    CHECK(t.imageHash(2) == xxh3Of(a));
    CHECK(!t.find(2)->obsoleteHash);
}

void testNotHashed() {
    std::atomic<std::uint32_t> counter{0};
    TextureTracker t(testConfig(&counter));
    const auto a = pattern(256, 1);
    tap::TextureDesc cube = desc(1, kCube, 8, 8, D3DFormat::A8R8G8B8, kDefault);
    tap::TextureDesc vol = desc(2, kVolume, 8, 8, D3DFormat::A8R8G8B8, kDefault);
    tap::TextureDesc surf = desc(3, kSurface, 8, 8, D3DFormat::A8R8G8B8, kDefault);
    tap::TextureDesc depth = desc(4, kTexture, 8, 8, D3DFormat::D16_LOCKABLE, kDefault, kDS);
    tap::TextureDesc sys = desc(5, kTexture, 8, 8, D3DFormat::A8R8G8B8, kSystemMem);
    tap::TextureDesc scratch = desc(6, kTexture, 8, 8, D3DFormat::A8R8G8B8, kScratch);
    tap::TextureDesc nullFmt = desc(7, kTexture, 8, 8, D3DFormat(hash::makeFourCC('N', 'U', 'L', 'L')), kDefault);
    for (const auto& d : {cube, vol, surf, depth, sys, scratch, nullFmt}) {
        t.onTextureCreate(d);
        write(t, d.id, 0, a, 32, 8);
        draw(t, {{0, d.id}});
        CHECK(t.imageHash(d.id) == 0);
        CHECK(t.descriptorHash(d.id) == 0);
    }
    CHECK(!t.find(1)->hashable && !t.find(5)->hashable);
    // Unknown ids are counted, never crash.
    write(t, 99, 0, a, 32, 8);
    CHECK(t.stats().unknownTexture == 2);
}

// ---- Update* inheritance ---------------------------------------------------------------------------

void testUpdateTextureInheritance() {
    std::atomic<std::uint32_t> counter{0};
    TextureTracker t(testConfig(&counter));
    t.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kSystemMem, 0, 4));
    t.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault, 0, 4));
    const auto l0 = pattern(256, 1);
    write(t, 1, 0, l0, 32, 8);
    write(t, 1, 1, pattern(64, 2), 16, 4);
    CHECK(t.imageHash(1) == 0); // SYSTEMMEM: no image, no hash of its own
    copy(t, tap::CopyMethod::UpdateTexture, 1, 2);
    CHECK(t.imageHash(2) == xxh3Of(l0));
    auto s = t.find(2);
    CHECK(s && s->origin == HashOrigin::Inherited && s->inheritedFrom == 1);
    // Already hashed: kept even when the source changes.
    write(t, 1, 0, pattern(256, 9), 32, 8);
    copy(t, tap::CopyMethod::UpdateTexture, 1, 2);
    CHECK(t.imageHash(2) == xxh3Of(l0));
    // An upload first, then UpdateTexture: the upload hash stays.
    t.onTextureCreate(desc(3, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    const auto own = pattern(256, 5);
    write(t, 3, 0, own, 32, 8);
    copy(t, tap::CopyMethod::UpdateTexture, 1, 3);
    CHECK(t.imageHash(3) == xxh3Of(own));
    // A never-written SYSTEMMEM source has zero-filled buffers from creation.
    t.onTextureCreate(desc(4, kTexture, 8, 8, D3DFormat::A8R8G8B8, kSystemMem));
    t.onTextureCreate(desc(5, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    copy(t, tap::CopyMethod::UpdateTexture, 4, 5);
    CHECK(t.imageHash(5) == xxh3Of(std::vector<std::uint8_t>(256, 0)));
    // Cube destinations are not hashed.
    t.onTextureCreate(desc(6, kCube, 8, 8, D3DFormat::A8R8G8B8, kSystemMem));
    t.onTextureCreate(desc(7, kCube, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    copy(t, tap::CopyMethod::UpdateTexture, 6, 7);
    CHECK(t.imageHash(7) == 0);
    CHECK(t.stats().inherited == 2);
}

void testUpdateSurfaceInheritance() {
    std::atomic<std::uint32_t> counter{0};
    TextureTracker t(testConfig(&counter));
    const auto a = pattern(8 * 16, 1);
    t.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::R5G6B5, kSystemMem));
    t.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::R5G6B5, kDefault));
    write(t, 1, 0, a, 16, 8);
    copy(t, tap::CopyMethod::UpdateSurface, 1, 2);
    CHECK(t.imageHash(2) == xxh3Of(a));
    // Source level 1 of a 16x16 into an 8x8 destination: full size, and (NV-DXVK) the hash is
    // the source's subresource 0, not the copied level.
    t.onTextureCreate(desc(3, kTexture, 16, 16, D3DFormat::R5G6B5, kSystemMem, 0, 2));
    t.onTextureCreate(desc(4, kTexture, 8, 8, D3DFormat::R5G6B5, kDefault));
    const auto big = pattern(16 * 32, 2);
    write(t, 3, 0, big, 32, 16);
    write(t, 3, 1, a, 16, 8);
    copy(t, tap::CopyMethod::UpdateSurface, 3, 4, 1);
    CHECK(t.imageHash(4) == xxh3Of(big));
    // Level 0 of the 16x16 into the 8x8: not the destination's extent, no inheritance.
    t.onTextureCreate(desc(5, kTexture, 8, 8, D3DFormat::R5G6B5, kDefault));
    copy(t, tap::CopyMethod::UpdateSurface, 3, 5, 0);
    CHECK(t.imageHash(5) == 0);
    // With a rectangle: assumed full when the extents match (default policy)...
    t.onTextureCreate(desc(6, kTexture, 8, 8, D3DFormat::R5G6B5, kDefault));
    copy(t, tap::CopyMethod::UpdateSurface, 1, 6, 0, true);
    CHECK(t.imageHash(6) == xxh3Of(a));
    // ...or never, with SourceRectPolicy::NeverInherit.
    TextureTrackerConfig c = testConfig(&counter);
    c.sourceRectPolicy = SourceRectPolicy::NeverInherit;
    TextureTracker t2(c);
    t2.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::R5G6B5, kSystemMem));
    t2.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::R5G6B5, kDefault));
    write(t2, 1, 0, a, 16, 8);
    copy(t2, tap::CopyMethod::UpdateSurface, 1, 2, 0, true);
    CHECK(t2.imageHash(2) == 0);
    copy(t2, tap::CopyMethod::UpdateSurface, 1, 2, 0, false);
    CHECK(t2.imageHash(2) == xxh3Of(a));
    // A SYSTEMMEM plain surface is a valid source too.
    t2.onTextureCreate(desc(3, kSurface, 8, 8, D3DFormat::R5G6B5, kSystemMem));
    t2.onTextureCreate(desc(4, kTexture, 8, 8, D3DFormat::R5G6B5, kDefault));
    const auto b = pattern(8 * 16, 3);
    write(t2, 3, 0, b, 16, 8);
    copy(t2, tap::CopyMethod::UpdateSurface, 3, 4);
    CHECK(t2.imageHash(4) == xxh3Of(b));
}

// ---- recompute on write -----------------------------------------------------------------------------

void testRecomputeOnWrite() {
    std::atomic<std::uint32_t> counter{0};
    const auto a = pattern(256, 1), b = pattern(256, 2);
    {
        TextureTracker off(testConfig(&counter));
        off.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
        write(off, 1, 0, a, 32, 8);
        write(off, 1, 0, b, 32, 8);
        CHECK(off.imageHash(1) == xxh3Of(a));
        CHECK(off.mip0Bytes(1).empty()); // shadow dropped once hashed (nothing can re-hash it)
    }
    TextureTrackerConfig c = testConfig(&counter);
    c.recomputeTextureHashOnWrite = true;
    const Hash64 pinned = xxh3Of(pattern(256, 7));
    c.keepHashOnWrite = [pinned](Hash64 h) { return h == pinned; };
    TextureTracker t(c);
    t.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault, 0, 2));
    write(t, 1, 0, a, 32, 8);
    CHECK(t.imageHash(1) == xxh3Of(a));
    write(t, 1, 1, pattern(64, 3), 16, 4); // mip 1: kept
    CHECK(t.imageHash(1) == xxh3Of(a));
    write(t, 1, 0, b, 32, 8); // mip 0: cleared at lock, re-hashed at unlock
    CHECK(t.imageHash(1) == xxh3Of(b));
    auto s = t.find(1);
    CHECK(s && s->clearCount == 1 && s->hashCount == 2);
    // A partial rewrite re-hashes the whole retained buffer.
    writeRect(t, 1, std::vector<std::uint8_t>(4, 0), 4, tap::Box{0, 0, 1, 1, 0, 1});
    auto expect = b;
    std::memset(expect.data(), 0, 4);
    CHECK(t.imageHash(1) == xxh3Of(expect));
    // A listed hash is kept.
    t.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    write(t, 2, 0, pattern(256, 7), 32, 8);
    write(t, 2, 0, b, 32, 8);
    CHECK(t.imageHash(2) == pinned);
    // Managed: cleared at the lock, re-hashed at the next sampling draw.
    t.onTextureCreate(desc(3, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    write(t, 3, 0, a, 32, 8);
    draw(t, {{0, 3}});
    CHECK(t.imageHash(3) == xxh3Of(a));
    write(t, 3, 0, b, 32, 8);
    CHECK(t.imageHash(3) == 0 && t.find(3)->flushPending);
    draw(t, {{0, 3}});
    CHECK(t.imageHash(3) == xxh3Of(b));
}

// ---- render targets ------------------------------------------------------------------------------

Hash64 rtHash(std::uint32_t w, std::uint32_t h, std::uint32_t n) {
    const std::uint32_t extent[3] = {w, h, 1};
    return hash::xxh3_64(&n, 4, hash::xxh3_64(extent, 12));
}

hash::TextureDescriptor rtDesc(const tap::TextureDesc& d, std::uint32_t quality = 0) {
    hash::TextureDescriptor o;
    o.width = d.width;
    o.height = d.height;
    o.depth = d.depth;
    o.arraySize = d.arraySize;
    o.mipLevels = d.mipLevels;
    o.usage = d.usage;
    o.format = d.format;
    o.pool = d.pool;
    o.multiSample = d.multiSample;
    o.multisampleQuality = quality;
    o.isBackBuffer = d.isBackBuffer;
    o.isAttachmentOnly = d.isAttachmentOnly;
    return o;
}

void testRenderTargets() {
    std::atomic<std::uint32_t> counter{0};
    TextureTrackerConfig c = testConfig(&counter);
    c.recomputeTextureHashOnWrite = true;
    TextureTracker t(c);
    tap::TextureDesc bb = desc(1, kSurface, 128, 96, D3DFormat::X8R8G8B8, kDefault, kRT);
    bb.isBackBuffer = true;
    tap::TextureDesc ds = desc(2, kSurface, 128, 96, D3DFormat::D24S8, kDefault, kDS);
    ds.isAttachmentOnly = true;
    tap::TextureDesc rtt = desc(3, kTexture, 64, 32, D3DFormat::A8R8G8B8, kDefault, kRT);
    t.onTextureCreate(bb);
    t.onTextureCreate(ds);
    t.onTextureCreate(rtt);
    CHECK(t.imageHash(1) == rtHash(128, 96, 0));
    CHECK(t.descriptorHash(1) == hash::hashTextureDescriptor(rtDesc(bb)));
    CHECK(t.imageHash(2) == 0 && t.descriptorHash(2) == 0);
    CHECK(t.imageHash(3) == rtHash(64, 32, 1));
    CHECK(t.descriptorHash(3) == hash::hashTextureDescriptor(rtDesc(rtt)));
    CHECK(t.find(3)->origin == HashOrigin::RenderTarget);
    CHECK(counter.load() == 2);
    // The swap chain's multisample quality arrives with the device event.
    tap::DeviceEvent dev;
    dev.backBuffer = 1;
    dev.present.multiSampleQuality = 2;
    t.onDeviceCreate(dev);
    CHECK(t.descriptorHash(1) == hash::hashTextureDescriptor(rtDesc(bb, 2)));
    CHECK(t.imageHash(1) == rtHash(128, 96, 0));
    // Recompute-on-write clears both hashes; a render-target *texture* is re-hashed from its
    // content with a fresh descriptor hash, a render-target surface is not.
    const auto px = pattern(64 * 32 * 4, 1);
    write(t, 3, 0, px, 256, 32);
    CHECK(t.imageHash(3) == xxh3Of(px));
    CHECK(t.descriptorHash(3) == hash::hashTextureDescriptor(rtDesc(rtt)));
    CHECK(t.find(3)->origin == HashOrigin::Upload);
    write(t, 1, 0, pattern(128 * 96 * 4, 2), 512, 96);
    CHECK(t.imageHash(1) == 0 && t.descriptorHash(1) == 0);
    // Without recompute-on-write a render target's counter hash never changes.
    TextureTracker t2(testConfig(&counter));
    t2.onTextureCreate(rtt);
    write(t2, 3, 0, px, 256, 32);
    CHECK(t2.imageHash(3) == rtHash(64, 32, 2));
}

// ---- registry ------------------------------------------------------------------------------------

void testRegistry() {
    std::atomic<std::uint32_t> counter{0};
    ExternalImageRegistry reg;
    std::vector<ExternalImageHandle> releasedHandles;
    reg.setReleaseListener([&](ExternalImageHandle h, const ExternalImageInfo&) { releasedHandles.push_back(h); });
    TextureTracker t(testConfig(&counter), &reg);
    const auto a = pattern(256, 1);
    t.onTextureCreate(desc(1, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    t.onTextureCreate(desc(2, kTexture, 8, 8, D3DFormat::A8R8G8B8, kSystemMem)); // no image
    t.onTextureCreate(desc(3, kTexture, 8, 8, D3DFormat::A8R8G8B8, kManaged));
    CHECK(reg.liveCount() == 2);
    const ExternalImageHandle h1 = t.find(1)->image;
    CHECK(h1.valid() && !t.find(2)->image.valid());
    CHECK(reg.lookup(h1)->vkImage == 0x10001 && reg.lookup(h1)->imageHash == 0);
    write(t, 1, 0, a, 32, 8);
    write(t, 3, 0, a, 32, 8);
    draw(t, {{0, 3}});
    CHECK(reg.lookup(h1)->imageHash == xxh3Of(a));
    CHECK(reg.findByImageHash(xxh3Of(a)).size() == 2);
    // Destroy: the handle goes stale, the listener runs once.
    t.onImageDestroy(tap::ImageDestroy{1, 0x10001});
    CHECK(!reg.lookup(h1).has_value());
    CHECK(releasedHandles.size() == 1 && releasedHandles[0] == h1);
    CHECK(reg.liveCount() == 1 && t.liveCount() == 2);
    // The slot is reused with a new generation; the old handle stays stale.
    t.onTextureCreate(desc(4, kTexture, 8, 8, D3DFormat::A8R8G8B8, kDefault));
    const ExternalImageHandle h4 = t.find(4)->image;
    CHECK(h4.index == h1.index && h4.generation != h1.generation);
    CHECK(!reg.lookup(h1).has_value() && reg.lookup(h4)->texture == 4);
    CHECK(reg.capacity() == 2);
    // Re-registering a texture with another VkImage releases the old slot.
    ExternalImageInfo info = *reg.lookup(h4);
    info.vkImage = 0x99999;
    const ExternalImageHandle h4b = reg.registerImage(info);
    CHECK(!(h4b == h4) && !reg.lookup(h4).has_value() && reg.handleOf(4) == h4b);
    CHECK(releasedHandles.size() == 2);
    info.vkImage = 0;
    CHECK(!reg.registerImage(info).valid());
    // Device destruction releases everything.
    t.onDeviceDestroy();
    CHECK(reg.liveCount() == 0 && t.liveCount() == 0);
    CHECK(releasedHandles.size() == 4);
}

// ---- options -------------------------------------------------------------------------------------

struct TestListOptions {
    FUSE_RELIGHT_OPTION("rtx", options::HashSet, terrainTextures, {}, "test declaration of the terrain list");
};

void testOptions() {
    const TextureTrackerConfig defaults = textureTrackerConfigFromOptions();
    CHECK(!defaults.useObsoleteHashOnTextureUpload && !defaults.recomputeTextureHashOnWrite);
    CHECK(!isHashKeptOnWrite(0x1234));
    const options::OptionConfig cfg = options::OptionConfig::parse(
        "rtx.useObsoleteHashOnTextureUpload = True\n"
        "relight.recomputeTextureHashOnWrite = True\n"
        "rtx.terrainTextures = 0x0000000000001234, 0x00000000DEADBEEF\n");
    options::OptionLayerHandle layer =
        options::OptionManager::acquireLayer("", options::OptionLayerKey{10000, "RL14TestLayer"}, 1.0f, 0.1f, false, &cfg);
    options::OptionManager::applyPendingValues(nullptr, false);
    const TextureTrackerConfig c = textureTrackerConfigFromOptions();
    CHECK(c.useObsoleteHashOnTextureUpload && c.recomputeTextureHashOnWrite);
    CHECK(isHashKeptOnWrite(0x1234) && isHashKeptOnWrite(0xDEADBEEF) && !isHashKeptOnWrite(0x5678));
    CHECK(c.keepHashOnWrite && c.keepHashOnWrite(0x1234));
    layer.release();
    options::OptionManager::applyPendingValues(nullptr, false);
    CHECK(!textureTrackerConfigFromOptions().recomputeTextureHashOnWrite);
}

// ---- replay driver (Wine tests) ------------------------------------------------------------------
//
// A line-based event script written by rl_capture_texture_run.py from a recording-tap stream
// (relight_tap.jsonl) and the app sidecar's blobs:
//   create id type w h d levels array format usage pool ms backbuffer attachment vkimage
//   device backbuffer msquality
//   wlock id face level flags
//   upload id face level w h rowpitch rows flags full left top right bottom <hex|->
//   copy UpdateTexture|UpdateSurface src dst sface slevel dface dlevel hasrect
//   draw ps vs disabledStage slot:id...
//   destroy id
//   flushall
//   snapshot label           -> one "tex" line per live texture
// Output: "snapshot <label>" then "tex id=.. hash=.. desc=.. origin=.. from=.. pending=.. obsolete=..
// preview=.." lines.

std::vector<std::uint8_t> fromHex(const std::string& s) {
    std::vector<std::uint8_t> out;
    if (s == "-") {
        return out;
    }
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(std::uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    }
    return out;
}

int replay(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    TextureTrackerConfig config; // defaults = upstream defaults
    ExternalImageRegistry registry;
    TextureTracker t(config, &registry);
    static std::uint32_t tss[tap::kTextureStageCount][32];
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ss(line);
        std::string op;
        if (!(ss >> op) || op[0] == '#') {
            continue;
        }
        if (op == "create") {
            tap::TextureDesc d;
            std::uint32_t bb = 0, at = 0;
            std::uint64_t vk = 0;
            ss >> d.id >> d.type >> d.width >> d.height >> d.depth >> d.mipLevels >> d.arraySize >> d.format >> d.usage >>
                d.pool >> d.multiSample >> bb >> at >> vk;
            d.isBackBuffer = bb != 0;
            d.isAttachmentOnly = at != 0;
            d.vkImage = vk;
            t.onTextureCreate(d);
        } else if (op == "device") {
            tap::DeviceEvent e;
            ss >> e.backBuffer >> e.present.multiSampleQuality;
            t.onDeviceCreate(e);
        } else if (op == "wlock") {
            tap::TextureWriteLock l;
            ss >> l.texture >> l.face >> l.level >> l.lockFlags;
            t.onTextureWriteLock(l);
        } else if (op == "upload") {
            tap::TextureUpload u;
            std::uint32_t full = 0;
            std::string hex;
            ss >> u.texture >> u.face >> u.level >> u.width >> u.height >> u.rowPitch >> u.rows >> u.lockFlags >> full >>
                u.box.left >> u.box.top >> u.box.right >> u.box.bottom >> hex;
            const std::vector<std::uint8_t> data = fromHex(hex);
            u.fullUpdate = full != 0;
            u.box.back = 1;
            u.slicePitch = u.rowPitch * u.rows;
            u.data = data.empty() ? nullptr : data.data();
            t.onTextureUpload(u);
        } else if (op == "copy") {
            tap::TextureCopy c;
            std::string method;
            std::uint32_t rect = 0;
            ss >> method >> c.source >> c.destination >> c.sourceFace >> c.sourceLevel >> c.destFace >> c.destLevel >> rect;
            c.method = method == "UpdateTexture" ? tap::CopyMethod::UpdateTexture : tap::CopyMethod::UpdateSurface;
            c.hasSourceRect = rect != 0;
            t.onTextureCopy(c);
        } else if (op == "draw") {
            tap::DrawState st;
            int disabled = 8;
            ss >> st.pixelShader.id >> st.vertexShader.id >> disabled;
            for (std::uint32_t s = 0; s < tap::kTextureStageCount; ++s) {
                tss[s][0] = int(s) == disabled ? 1u : 2u;
            }
            st.textureStageStates = tss;
            std::string binding;
            while (ss >> binding) {
                const auto colon = binding.find(':');
                const std::uint32_t slot = std::uint32_t(std::stoul(binding.substr(0, colon)));
                if (slot < tap::kSamplerSlotCount) {
                    st.textures[slot] = tap::ResourceId(std::stoul(binding.substr(colon + 1)));
                }
            }
            t.onDraw(tap::DrawCall{}, st);
        } else if (op == "destroy") {
            tap::ImageDestroy d;
            ss >> d.texture;
            t.onImageDestroy(d);
        } else if (op == "flushall") {
            t.flushAllPending();
        } else if (op == "snapshot") {
            std::string label;
            ss >> label;
            std::printf("snapshot %s\n", label.c_str());
            for (const TrackedTexture& x : t.textures()) {
                std::printf("tex id=%u hash=%016" PRIX64 " desc=%016" PRIX64 " origin=%s from=%u pending=%d obsolete=%d "
                            "preview=%016" PRIX64 " registered=%d\n",
                            x.desc.id, x.imageHash, x.descriptorHash, hashOriginName(x.origin), x.inheritedFrom,
                            x.flushPending ? 1 : 0, x.obsoleteHash ? 1 : 0, t.previewImageHash(x.desc.id),
                            (x.image.valid() && registry.lookup(x.image).has_value() &&
                             registry.lookup(x.image)->imageHash == x.imageHash)
                                ? 1
                                : 0);
            }
        } else {
            std::fprintf(stderr, "%s:%d: unknown op '%s'\n", path, lineNo, op.c_str());
            return 2;
        }
    }
    const TextureTrackerStats s = t.stats();
    std::printf("stats uploads=%" PRIu64 " mip0_writes=%" PRIu64 " hashes=%" PRIu64 " inherited=%" PRIu64
                " unknown=%" PRIu64 " live=%zu registry_live=%zu\n",
                s.uploads, s.mip0Writes, s.hashesComputed, s.inherited, s.unknownTexture, t.liveCount(),
                registry.liveCount());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--replay") == 0) {
        return replay(argv[2]);
    }
    testCanonicalRepack();
    testDefaultPoolFirstUpload();
    testManagedHashedAtFirstDraw();
    testObsoleteHash();
    testNotHashed();
    testUpdateTextureInheritance();
    testUpdateSurfaceInheritance();
    testRecomputeOnWrite();
    testRenderTargets();
    testRegistry();
    testOptions();
    std::printf("rl_capture_texture_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
