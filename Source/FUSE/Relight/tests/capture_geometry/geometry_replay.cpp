// FUSE Relight RL-1.3: replays a recorded tap event stream into GeometryCapture and prints what it
// captured, one line per draw. Driven by rl_capture_geometry.py, which converts the recording
// tap's JSON Lines (relight.tap.mode = record) into the line format below, restoring the buffer
// bytes the tap only hashed from the app's sidecar blobs (matched by SHA-256).
//
// Input (one event per line, `key=value` tokens; byte strings are hex, "-" = none):
//   bcreate id= kind=0|1 size= format=
//   bwrite  id= offset= size= flags= data=<whole buffer after the write>
//   bdestroy id=
//   tcreate id= type=
//   tupload id= face= level=
//   tcopy   src= dst= method=
//   present
//   draw n= call= prim= pc= sv= bv= mi= nv= si= ib=<buffer>:<format>|- st=<s>:<buffer>:<offset>:<stride>;...|-
//        el=<s>:<offset>:<type>:<method>:<usage>:<usageIndex>;... fvf= rs=<index>:<value>,...|-
//        tss=<stage>:<type>:<value>,...|- tex=<slot>:<id>,...|- vs=<id> ps=<id> xf=<slot>:<16 x f32 bits hex>;...|-
//        upv=<hex>|- ups= upi=<hex>|- upf=
// Output: `draw n= frame= di= status= ...` (see printDraw), then `stats ...`.
//
// Options: --rule STR (generation rule), --asset STR, --scale F, --sync (jobs inline), --workers N,
//          --no-mapping (bindings carry no CPU pointer: the capture's buffer shadows are used),
//          --no-memo (index memoization off).
#include <fuse/relight/capture/geometry/geometry_capture.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace fuse::relight;
using namespace fuse::relight::capture::geometry;

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    if (s.empty() || s == "-") {
        return out;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
        if (p == std::string_view::npos) {
            break;
        }
        start = p + 1;
    }
    return out;
}

std::uint64_t num(std::string_view s) {
    return std::strtoull(std::string(s).c_str(), nullptr, 0);
}

std::int64_t snum(std::string_view s) {
    return std::strtoll(std::string(s).c_str(), nullptr, 0);
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool unhex(std::string_view s, std::vector<std::uint8_t>& out) {
    out.clear();
    if (s == "-" || s.empty()) {
        return true;
    }
    if (s.size() % 2) {
        return false;
    }
    out.resize(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hexNibble(s[2 * i]), lo = hexNibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = std::uint8_t(hi * 16 + lo);
    }
    return true;
}

std::string h64(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string f32bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    char buf[12];
    std::snprintf(buf, sizeof buf, "%08x", b);
    return buf;
}

struct Options {
    std::string script;
    std::string rule{hash::rules::kDefaultGenerationRuleString};
    std::string asset{hash::rules::kDefaultAssetRuleString};
    float scale = 1.0f;
    bool sync = false;
    unsigned workers = 2;
    bool noMapping = false;
    bool noMemo = false;
};

class Replayer {
public:
    explicit Replayer(const Options& o) : m_opts(o), m_capture(makeConfig(o)) {
        m_identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }

    static GeometryCaptureConfig makeConfig(const Options& o) {
        GeometryCaptureConfig c = GeometryCaptureConfig::fromOptions();
        c.generationRule = hash::parseHashRule(o.rule);
        c.assetRule = hash::parseHashRule(o.asset);
        c.sceneScale = o.scale;
        c.asyncJobs = !o.sync;
        c.indexBufferMemoization = !o.noMemo;
        return c;
    }

    bool line(const std::string& text, std::size_t lineNo) {
        std::istringstream in(text);
        std::string ev;
        in >> ev;
        if (ev.empty() || ev[0] == '#') {
            return true;
        }
        m_kv.clear();
        for (std::string tok; in >> tok;) {
            const std::size_t eq = tok.find('=');
            if (eq != std::string::npos) {
                m_kv[tok.substr(0, eq)] = tok.substr(eq + 1);
            }
        }
        if (ev == "bcreate") {
            tap::BufferDesc d;
            d.id = tap::ResourceId(num(get("id")));
            d.kind = num(get("kind")) ? tap::BufferKind::Index : tap::BufferKind::Vertex;
            d.size = std::uint32_t(num(get("size")));
            d.format = std::uint32_t(num(get("format")));
            m_buffers[d.id].assign(d.size, 0);
            m_capture.onBufferCreate(d);
        } else if (ev == "bwrite") {
            const auto id = tap::ResourceId(num(get("id")));
            std::vector<std::uint8_t>& buf = m_buffers[id];
            if (!unhex(get("data"), buf)) {
                return fail(lineNo, "bad data");
            }
            tap::BufferWrite w;
            w.buffer = id;
            w.offset = std::uint32_t(num(get("offset")));
            w.size = std::uint32_t(num(get("size")));
            w.lockFlags = std::uint32_t(num(get("flags")));
            if (std::size_t(w.offset) + w.size > buf.size()) {
                return fail(lineNo, "write outside the buffer");
            }
            w.data = buf.data() + w.offset;
            w.base = buf.data();
            w.bufferSize = std::uint32_t(buf.size());
            m_capture.onBufferWrite(w);
        } else if (ev == "bdestroy") {
            const auto id = tap::ResourceId(num(get("id")));
            m_capture.onBufferDestroy(id);
            m_buffers.erase(id);
        } else if (ev == "tcreate") {
            tap::TextureDesc d;
            d.id = tap::ResourceId(num(get("id")));
            d.type = std::uint32_t(num(get("type")));
            m_capture.onTextureCreate(d);
        } else if (ev == "tupload") {
            tap::TextureUpload u;
            u.texture = tap::ResourceId(num(get("id")));
            u.face = std::uint32_t(num(get("face")));
            u.level = std::uint32_t(num(get("level")));
            m_capture.onTextureUpload(u);
        } else if (ev == "tcopy") {
            tap::TextureCopy c;
            c.source = tap::ResourceId(num(get("src")));
            c.destination = tap::ResourceId(num(get("dst")));
            c.method = tap::CopyMethod(num(get("method")));
            m_capture.onTextureCopy(c);
        } else if (ev == "present") {
            flush();
            m_capture.onPresent(tap::FrameEvent{});
        } else if (ev == "draw") {
            return draw(lineNo);
        } else {
            return fail(lineNo, "unknown event " + ev);
        }
        return true;
    }

    /// Prints the queued draws (waiting for their jobs): per frame, so the jobs of a frame overlap.
    void flush() {
        for (const auto& [draw, ordinal] : m_queue) {
            printDraw(*draw, ordinal);
        }
        m_queue.clear();
    }

    void finish() {
        flush();
        const GeometryCaptureStats s = m_capture.stats();
        std::printf("stats draws=%llu captured=%llu memo_hits=%llu memo_misses=%llu skinned=%llu bytes=%llu\n",
                    static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.captured),
                    static_cast<unsigned long long>(s.memoHits), static_cast<unsigned long long>(s.memoMisses),
                    static_cast<unsigned long long>(s.skinnedDraws), static_cast<unsigned long long>(s.streamBytesCopied));
    }

    bool failed() const { return m_failed; }

private:
    const std::string& get(const char* key) {
        static const std::string kNone = "-";
        auto it = m_kv.find(key);
        return it == m_kv.end() ? kNone : it->second;
    }

    bool fail(std::size_t lineNo, const std::string& what) {
        std::fprintf(stderr, "geometry_replay: line %zu: %s\n", lineNo, what.c_str());
        m_failed = true;
        return false;
    }

    bool draw(std::size_t lineNo) {
        static const std::map<std::string, tap::DrawCallType> kCalls = {
            {"0", tap::DrawCallType::DrawPrimitive},
            {"1", tap::DrawCallType::DrawIndexedPrimitive},
            {"2", tap::DrawCallType::DrawPrimitiveUP},
            {"3", tap::DrawCallType::DrawIndexedPrimitiveUP}};
        tap::DrawCall call;
        call.call = kCalls.at(get("call"));
        call.primitiveType = std::uint32_t(num(get("prim")));
        call.primitiveCount = std::uint32_t(num(get("pc")));
        call.startVertex = std::uint32_t(num(get("sv")));
        call.baseVertex = std::int32_t(snum(get("bv")));
        call.minIndex = std::uint32_t(num(get("mi")));
        call.numVertices = std::uint32_t(num(get("nv")));
        call.startIndex = std::uint32_t(num(get("si")));

        // State storage for this draw (DrawState views it).
        m_renderStates.fill(0);
        for (std::uint32_t s = 0; s < tap::kTextureStageCount; ++s) {
            for (std::uint32_t t = 0; t < 32; ++t) {
                m_tss[s][t] = 0;
            }
        }
        for (std::uint32_t slot = 0; slot < tap::kTransformCount; ++slot) {
            std::memcpy(m_transforms[slot], m_identity.data(), sizeof(m_transforms[slot]));
        }
        tap::DrawState state;
        state.renderStates = m_renderStates.data();
        state.textureStageStates = m_tss;
        state.transforms = m_transforms;

        for (std::string_view rs : split(get("rs"), ',')) {
            const auto p = split(rs, ':');
            m_renderStates.at(num(p.at(0))) = std::uint32_t(num(p.at(1)));
        }
        for (std::string_view t : split(get("tss"), ',')) {
            const auto p = split(t, ':');
            const auto stage = num(p.at(0)), type = num(p.at(1));
            if (stage < tap::kTextureStageCount && type >= 1 && type <= 32) {
                m_tss[stage][type - 1] = std::uint32_t(num(p.at(2)));
            }
        }
        for (std::string_view t : split(get("tex"), ',')) {
            const auto p = split(t, ':');
            state.textures[num(p.at(0))] = tap::ResourceId(num(p.at(1)));
        }
        for (std::string_view x : split(get("xf"), ';')) {
            const auto p = split(x, ':');
            std::vector<std::uint8_t> bytes;
            if (!unhex(p.at(1), bytes) || bytes.size() != 64) {
                return fail(lineNo, "bad transform");
            }
            std::memcpy(m_transforms[num(p.at(0))], bytes.data(), 64);
        }
        state.vertexShader.id = tap::ResourceId(num(get("vs")));
        state.pixelShader.id = tap::ResourceId(num(get("ps")));
        state.fvf = std::uint32_t(num(get("fvf")));

        for (std::string_view e : split(get("el"), ';')) {
            const auto p = split(e, ':');
            if (state.elementCount >= tap::kMaxVertexElements) {
                break;
            }
            tap::VertexElement& el = state.elements[state.elementCount++];
            el.stream = std::uint16_t(num(p.at(0)));
            el.offset = std::uint16_t(num(p.at(1)));
            el.type = std::uint8_t(num(p.at(2)));
            el.method = std::uint8_t(num(p.at(3)));
            el.usage = std::uint8_t(num(p.at(4)));
            el.usageIndex = std::uint8_t(num(p.at(5)));
        }
        for (std::string_view st : split(get("st"), ';')) {
            const auto p = split(st, ':');
            const auto s = num(p.at(0));
            tap::StreamBinding& b = state.streams[s];
            b.buffer = tap::ResourceId(num(p.at(1)));
            b.offset = std::uint32_t(num(p.at(2)));
            b.stride = std::uint32_t(num(p.at(3)));
            b.frequency = 1;
            if (auto it = m_buffers.find(b.buffer); it != m_buffers.end() && !m_opts.noMapping) {
                b.base = it->second.data();
                b.bufferSize = std::uint32_t(it->second.size());
            }
        }
        if (const auto ib = split(get("ib"), ':'); ib.size() == 2) {
            state.indices.buffer = tap::ResourceId(num(ib[0]));
            state.indices.format = std::uint32_t(num(ib[1]));
            if (auto it = m_buffers.find(state.indices.buffer); it != m_buffers.end() && !m_opts.noMapping) {
                state.indices.base = it->second.data();
                state.indices.bufferSize = std::uint32_t(it->second.size());
            }
        }
        std::vector<std::uint8_t> upv, upi;
        if (!unhex(get("upv"), upv) || !unhex(get("upi"), upi)) {
            return fail(lineNo, "bad UP data");
        }
        if (call.call == tap::DrawCallType::DrawPrimitiveUP || call.call == tap::DrawCallType::DrawIndexedPrimitiveUP) {
            call.upVertexData = upv.empty() ? nullptr : upv.data();
            call.upVertexBytes = std::uint32_t(upv.size());
            call.upVertexStride = std::uint32_t(num(get("ups")));
            call.upIndexData = upi.empty() ? nullptr : upi.data();
            call.upIndexBytes = std::uint32_t(upi.size());
            call.upIndexFormat = std::uint32_t(num(get("upf")));
        }

        m_capture.onDraw(call, state);
        for (CapturedDrawPtr& d : m_capture.takeDraws()) {
            m_queue.emplace_back(std::move(d), num(get("n")));
        }
        return true;
    }

    void printDraw(const CapturedDraw& d, std::uint64_t ordinal) {
        std::string out = "draw n=" + std::to_string(ordinal) + " frame=" + std::to_string(d.frame) +
                          " di=" + std::to_string(d.drawIndex) + " status=" + std::string(captureStatusName(d.status)) +
                          " tci=" + std::to_string(d.texcoord.texcoordIndex) +
                          " stage=" + std::to_string(d.texcoord.firstStage);
        if (d.captured()) {
            const hash::DrawGeometryHashes g = d.geometryHashes();
            out += " f=";
            for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
                out += (i ? "," : "") + h64(g.hashes.fields[i]);
            }
            out += " ic=" + std::to_string(g.indexCount) + " vc=" + std::to_string(g.vertexCount) +
                   " min=" + std::to_string(g.minIndex) + " max=" + std::to_string(g.maxIndex) +
                   " topo=" + std::to_string(g.topology) + " it=" + std::to_string(g.indexType) +
                   " ps=" + std::to_string(g.positionStride);
            out += " key=" + h64(d.assetHash(m_capture.config().assetRule));
            out += " leg0=" + h64(hash::meshReplacementHashLegacy(g, hash::rules::kLegacyAsset0, 0));
            out += " leg1=" + h64(hash::meshReplacementHashLegacy(g, hash::rules::kLegacyAsset1, 0));
            out += " memo=" + std::string(d.indicesMemoized ? "1" : "0");
            if (d.boundingBox.valid()) {
                const BoundingBox& b = d.boundingBox.get();
                out += " aabb=" + f32bits(b.minPos[0]) + "," + f32bits(b.minPos[1]) + "," + f32bits(b.minPos[2]) + "," +
                       f32bits(b.maxPos[0]) + "," + f32bits(b.maxPos[1]) + "," + f32bits(b.maxPos[2]);
            }
            if (d.skinning.valid()) {
                const SkinningData& s = d.skinning.get();
                out += " skin=" + std::to_string(s.numBones) + ":" + std::to_string(s.numBonesPerVertex) + ":" +
                       std::to_string(s.minBoneIndex) + ":" + h64(s.boneHash);
            } else {
                out += " skin=-";
            }
        }
        std::printf("%s\n", out.c_str());
    }

    const Options& m_opts;
    GeometryCapture m_capture;
    std::map<std::string, std::string> m_kv;
    std::map<tap::ResourceId, std::vector<std::uint8_t>> m_buffers;
    std::array<std::uint32_t, tap::kRenderStateCount> m_renderStates{};
    std::uint32_t m_tss[tap::kTextureStageCount][32]{};
    float m_transforms[tap::kTransformCount][16]{};
    std::array<float, 16> m_identity{};
    std::vector<std::pair<CapturedDrawPtr, std::uint64_t>> m_queue;
    bool m_failed = false;
};

int usage() {
    std::fprintf(stderr, "usage: geometry_replay SCRIPT [--rule STR] [--asset STR] [--scale F] [--sync] [--workers N] "
                         "[--no-mapping] [--no-memo]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--rule") {
            o.rule = next();
        } else if (a == "--asset") {
            o.asset = next();
        } else if (a == "--scale") {
            o.scale = std::strtof(next().c_str(), nullptr);
        } else if (a == "--sync") {
            o.sync = true;
        } else if (a == "--workers") {
            o.workers = unsigned(std::strtoul(next().c_str(), nullptr, 10));
        } else if (a == "--no-mapping") {
            o.noMapping = true;
        } else if (a == "--no-memo") {
            o.noMemo = true;
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else {
            o.script = a;
        }
    }
    if (o.script.empty()) {
        return usage();
    }
    std::ifstream in(o.script, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "geometry_replay: cannot open %s\n", o.script.c_str());
        return 2;
    }
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    if (!o.sync && o.workers > 0) {
        scheduler.initialize(o.workers);
    }
    int status = 0;
    {
        Replayer replayer(o);
        std::string text;
        std::size_t lineNo = 0;
        while (std::getline(in, text)) {
            ++lineNo;
            if (!text.empty() && text.back() == '\r') {
                text.pop_back();
            }
            if (!replayer.line(text, lineNo)) {
                status = 1;
                break;
            }
        }
        replayer.finish();
        status = replayer.failed() ? 1 : status;
    }
    if (scheduler.isInitialized()) {
        scheduler.shutdown();
    }
    std::fflush(stdout);
    return status;
}
