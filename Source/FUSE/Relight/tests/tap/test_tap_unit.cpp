// FUSE Relight RL-1.1: CPU unit tests of the tap library (ctest rl_tap_unit): SHA-256 vectors,
// tap-mode parsing and option resolution (FUSE_RELIGHT=0, per-option environment variables),
// the null tap's defaults and the recording tap's JSON Lines (buffer shadows with DISCARD, state
// block deduplication, identity transforms omitted, UP data hashes).
#include <fuse/relight/tap/null_tap.hpp>
#include <fuse/relight/tap/recording_tap.hpp>
#include <fuse/relight/tap/tap_config.hpp>

#include "sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

using namespace fuse::relight::tap;

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string l; std::getline(in, l);) {
        lines.push_back(l);
    }
    return lines;
}

bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

int countPrefix(const std::vector<std::string>& lines, const std::string& prefix) {
    int n = 0;
    for (const std::string& l : lines) {
        n += l.compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
    }
    return n;
}

void testSha256() {
    CHECK(detail::sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(detail::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(detail::sha256Hex(two, std::strlen(two)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Incremental updates across block boundaries give the one-shot digest.
    std::vector<unsigned char> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    detail::Sha256 h;
    h.update(data.data(), 1);
    h.update(data.data() + 1, 63);
    h.update(data.data() + 64, 500);
    h.update(data.data() + 564, 436);
    CHECK(h.hexDigest() == detail::sha256Hex(data.data(), data.size()));
}

void testModes() {
    TapMode m = TapMode::Record;
    CHECK(parseTapMode("off", m) && m == TapMode::Off);
    CHECK(parseTapMode("NULL", m) && m == TapMode::Null);
    CHECK(parseTapMode("Record", m) && m == TapMode::Record);
    CHECK(parseTapMode("0", m) && m == TapMode::Off);
    m = TapMode::Null;
    CHECK(!parseTapMode("bogus", m) && m == TapMode::Null);
    CHECK(std::string(tapModeName(TapMode::Record)) == "record");
}

void testRuntimeConfig() {
    // Master switch: nothing else is read.
    setEnv("FUSE_RELIGHT", "0");
    setEnv("FUSE_RELIGHT_TAP_MODE", "record");
    RuntimeConfig off = resolveRuntimeConfig();
    CHECK(!off.relightEnabled);
    CHECK(off.tapMode == TapMode::Off);
    CHECK(!off.importDevice);
    CHECK(createTap(off, 0) == nullptr);

    // Options through their environment variables (RL-0.6 Environment layer).
    setEnv("FUSE_RELIGHT", nullptr);
    setEnv("FUSE_RELIGHT_TAP_MODE", "null");
    setEnv("FUSE_RELIGHT_DEVICE_IMPORT", "False");
    setEnv("FUSE_RELIGHT_TAP_RECORD_PATH", "rl_tap_unit_env.jsonl");
    RuntimeConfig on = resolveRuntimeConfig();
    CHECK(on.relightEnabled);
    CHECK(on.tapMode == TapMode::Null);
    CHECK(!on.importDevice);
    CHECK(on.recordPath == "rl_tap_unit_env.jsonl");
    CHECK(!on.vkValidation);

    RuntimeConfig cfg;
    cfg.tapMode = TapMode::Off;
    CHECK(createTap(cfg, 0) == nullptr);
    cfg.tapMode = TapMode::Null;
    auto nullTap = createTap(cfg, 0);
    CHECK(dynamic_cast<NullTap*>(nullTap.get()) != nullptr);
    cfg.tapMode = TapMode::Record;
    cfg.recordPath = "rl_tap_unit_factory.jsonl";
    {
        auto rec = createTap(cfg, 2);
        auto* r = dynamic_cast<RecordingTap*>(rec.get());
        CHECK(r != nullptr);
        CHECK(r && r->path() == "rl_tap_unit_factory.jsonl.2");
        CHECK(r && r->isOpen());
    }
    std::remove("rl_tap_unit_factory.jsonl.2");
}

void testNullTap() {
    NullTap tap;
    DrawCall call;
    DrawState state;
    CHECK(tap.onDraw(call, state) == DrawDecision::Raster);
    std::vector<std::uint32_t> out;
    CHECK(!tap.substituteVertexShader(ShaderModule{}, out));
    CHECK(out.empty());
    IRelightTap& base = tap; // every event has a no-op default
    base.onPresent(FrameEvent{});
    base.onBufferWrite(BufferWrite{});
}

void testRecordingTap() {
    const std::string path = "rl_tap_unit_record.jsonl";
    const std::uint16_t idx[3] = {0, 1, 2};
    const float verts[6] = {0, 0, 0, 1, 1, 1};
    {
        RecordingTap tap(path);
        CHECK(tap.isOpen());

        DeviceEvent dev;
        dev.present.backBufferWidth = 128;
        dev.present.backBufferHeight = 96;
        dev.backBuffer = 1;
        tap.onDeviceCreate(dev);

        BufferDesc vb;
        vb.id = 1;
        vb.kind = BufferKind::Vertex;
        vb.size = 8;
        tap.onBufferCreate(vb);
        const std::uint8_t first[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        BufferWrite w;
        w.buffer = 1;
        w.offset = 0;
        w.size = 8;
        w.data = first;
        w.bufferSize = 8;
        tap.onBufferWrite(w);
        // Partial write with DISCARD: the shadow is zeroed first (RL-0.4 sidecar model).
        const std::uint8_t second[2] = {9, 9};
        w.offset = 2;
        w.size = 2;
        w.data = second;
        w.lockFlags = 0x2000;
        tap.onBufferWrite(w);

        std::uint32_t renderStates[kRenderStateCount] = {};
        renderStates[7] = 1;   // ZENABLE
        renderStates[137] = 0; // LIGHTING
        float transforms[kTransformCount][16] = {};
        for (auto& m : transforms) {
            m[0] = m[5] = m[10] = m[15] = 1.0f;
        }
        transforms[kTransformWorld0][12] = 2.5f; // WORLD translation
        float vsF[4][4] = {};
        vsF[3][1] = 0.5f;

        DrawCall call;
        call.call = DrawCallType::DrawPrimitive;
        call.primitiveType = 4;
        call.primitiveCount = 1;
        call.vertexCount = 3;
        DrawState state;
        state.renderStates = renderStates;
        state.transforms = transforms;
        state.vsConstF = vsF;
        state.vsConstFCount = 4;
        state.streams[0].buffer = 1;
        state.streams[0].stride = 4;
        state.elementCount = 1;
        state.elements[0].type = 2;  // FLOAT3
        state.elements[0].usage = 0; // POSITION
        state.fvf = 0x002;
        CHECK(tap.onDraw(call, state) == DrawDecision::Raster);
        CHECK(tap.onDraw(call, state) == DrawDecision::Raster);

        DrawCall up;
        up.call = DrawCallType::DrawIndexedPrimitiveUP;
        up.primitiveType = 4;
        up.primitiveCount = 1;
        up.indexCount = 3;
        up.upVertexData = verts;
        up.upVertexStride = 12;
        up.upVertexBytes = sizeof verts;
        up.upIndexData = idx;
        up.upIndexFormat = 101;
        up.upIndexBytes = sizeof idx;
        renderStates[137] = 1; // a new state block
        tap.onDraw(up, state);

        FrameEvent f;
        f.frame = 0;
        f.backBuffer = 1;
        tap.onPresent(f);
        ClearEvent c;
        c.color = 0xff102030u;
        tap.onClear(c);
        tap.onDeviceDestroy();
    }
    const std::vector<std::string> lines = readLines(path);
    CHECK(!lines.empty() && contains(lines[0], "\"schema\":\"fuse.relight.tap_events/1\""));
    CHECK(countPrefix(lines, "{\"ev\":\"device_create\"") == 1);
    CHECK(countPrefix(lines, "{\"ev\":\"buffer_write\"") == 2);
    CHECK(countPrefix(lines, "{\"ev\":\"draw\"") == 3);
    CHECK(countPrefix(lines, "{\"ev\":\"state_block\"") == 2); // deduplicated

    const std::uint8_t shadow1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::uint8_t shadow2[8] = {0, 0, 9, 9, 0, 0, 0, 0};
    const std::string sha1 = detail::sha256Hex(shadow1, 8);
    const std::string sha2 = detail::sha256Hex(shadow2, 8);
    int writes = 0;
    for (const std::string& l : lines) {
        if (contains(l, "\"ev\":\"buffer_write\"")) {
            CHECK(contains(l, writes == 0 ? sha1 : sha2));
            ++writes;
        }
        if (contains(l, "\"ev\":\"draw\"") && contains(l, "\"call\":\"DrawPrimitive\"")) {
            CHECK(contains(l, "\"blob\":\"" + sha2 + "\""));   // stream 0 hashes the shadow
            CHECK(contains(l, "\"WORLD\":[1,0,0,0,0,1,0,0,0,0,1,0,2.5,0,0,1]"));
            CHECK(!contains(l, "\"VIEW\""));                  // identity transforms omitted
            CHECK(contains(l, "\"primitive\":\"TRIANGLELIST\""));
            CHECK(contains(l, "\"type\":\"FLOAT3\",\"size\":12"));
            CHECK(contains(l, "\"fvf\":2"));
        }
        if (contains(l, "\"call\":\"DrawIndexedPrimitiveUP\"")) {
            CHECK(contains(l, "\"vertex_blob\":\"" + detail::sha256Hex(verts, sizeof verts) + "\""));
            CHECK(contains(l, "\"index_blob\":\"" + detail::sha256Hex(idx, sizeof idx) + "\""));
            CHECK(contains(l, "\"index_format\":\"INDEX16\""));
            CHECK(contains(l, "\"streams\":[]"));
        }
        if (contains(l, "\"ev\":\"state_block\"") && contains(l, "\"index\":0")) {
            CHECK(contains(l, "\"ZENABLE\":1"));
            CHECK(contains(l, "\"vs_const_f\":[{\"register\":3,\"value\":[0,0.5,0,0]}]"));
        }
        if (contains(l, "\"ev\":\"clear\"")) {
            CHECK(contains(l, "\"frame\":1"));
            CHECK(contains(l, "\"color\":\"0xff102030\""));
        }
    }
    CHECK(writes == 2);
    std::remove(path.c_str());
}

} // namespace

int main() {
    testSha256();
    testModes();
    testRuntimeConfig();
    testNullTap();
    testRecordingTap();
    std::printf("rl_tap_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
