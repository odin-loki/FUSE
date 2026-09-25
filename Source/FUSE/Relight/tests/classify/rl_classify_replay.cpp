// FUSE Relight RL-1.2: replays a recording tap stream (JSON Lines, "fuse.relight.tap_events/1",
// Source/FUSE/Relight/tap/include/fuse/relight/tap/recording_tap.hpp) through ClassifyTap and writes
// one JSON line per classified draw.
//
//   rl_classify_replay --stream relight_tap.jsonl [--conf rtx.conf] [--texture-hashes file] --out out.jsonl
//
// Every recorded event is turned back into the tap event struct DXVK's dispatcher passed
// (DeviceEvent, TextureDesc, DrawCall + DrawState, QueryEvent, FrameEvent), so the classifier sees
// what it would see live. What the recording does not carry: shader bytecode (a programmable shader
// then samples every slot) and Remix texture hashes, which the caller supplies per texture id in
// --texture-hashes ("<id> 0x<hash>" lines; rl_classify_capture.py computes them from the app sidecar).
// --conf is rtx.conf text applied as an option layer.
//
// Output lines: {"frame":F,"index":I,"draw_call_id":N,"status":"raytraced","reason":"RayTraced",
//                "inject":false,"categories":"Sky|...","decision":"ignore","sky_auto":false,
//                "using_rt_rt":false,"drawing_to_rt_rt":false,"color_texture":"0x..."}
#include <fuse/relight/scene/classify/classify_tap.hpp>
#include <fuse/relight/tap/d3d9_names.hpp>

#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace fuse::relight;

// ---- minimal JSON reader (the recording tap's output only: no escapes beyond \" \\ \uXXXX) ------------
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* get(const char* key) const {
        for (const auto& kv : o) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
    double num(const char* key, double fallback = 0) const {
        const Json* v = get(key);
        return v && v->kind == Number ? v->n : fallback;
    }
    std::uint32_t u32(const char* key, std::uint32_t fallback = 0) const {
        return static_cast<std::uint32_t>(num(key, fallback));
    }
    bool flag(const char* key) const {
        const Json* v = get(key);
        return v && v->kind == Bool && v->b;
    }
    std::string str(const char* key) const {
        const Json* v = get(key);
        return v && v->kind == String ? v->s : std::string();
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : m_t(text) {}
    bool parse(Json& out) {
        m_ok = true;
        value(out);
        ws();
        return m_ok && m_i == m_t.size();
    }

private:
    void ws() {
        while (m_i < m_t.size() && (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\r' || m_t[m_i] == '\n')) {
            ++m_i;
        }
    }
    bool eat(char c) {
        ws();
        if (m_i < m_t.size() && m_t[m_i] == c) {
            ++m_i;
            return true;
        }
        return false;
    }
    void fail() { m_ok = false; }
    void string(std::string& out) {
        if (!eat('"')) {
            return fail();
        }
        while (m_i < m_t.size() && m_t[m_i] != '"') {
            char c = m_t[m_i++];
            if (c == '\\' && m_i < m_t.size()) {
                const char e = m_t[m_i++];
                if (e == 'u' && m_i + 4 <= m_t.size()) {
                    c = static_cast<char>(std::strtoul(m_t.substr(m_i, 4).c_str(), nullptr, 16));
                    m_i += 4;
                } else {
                    c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
                }
            }
            out += c;
        }
        if (m_i >= m_t.size()) {
            return fail();
        }
        ++m_i;
    }
    void value(Json& v) {
        ws();
        if (m_i >= m_t.size()) {
            return fail();
        }
        const char c = m_t[m_i];
        if (c == '{') {
            ++m_i;
            v.kind = Json::Object;
            if (eat('}')) {
                return;
            }
            do {
                std::pair<std::string, Json> kv;
                ws();
                string(kv.first);
                if (!eat(':')) {
                    return fail();
                }
                value(kv.second);
                v.o.push_back(std::move(kv));
            } while (m_ok && eat(','));
            if (!eat('}')) {
                fail();
            }
        } else if (c == '[') {
            ++m_i;
            v.kind = Json::Array;
            if (eat(']')) {
                return;
            }
            do {
                v.a.emplace_back();
                value(v.a.back());
            } while (m_ok && eat(','));
            if (!eat(']')) {
                fail();
            }
        } else if (c == '"') {
            v.kind = Json::String;
            string(v.s);
        } else if (m_t.compare(m_i, 4, "true") == 0) {
            v.kind = Json::Bool;
            v.b = true;
            m_i += 4;
        } else if (m_t.compare(m_i, 5, "false") == 0) {
            v.kind = Json::Bool;
            m_i += 5;
        } else if (m_t.compare(m_i, 4, "null") == 0) {
            m_i += 4;
        } else {
            char* end = nullptr;
            v.kind = Json::Number;
            v.n = std::strtod(m_t.c_str() + m_i, &end);
            if (end == m_t.c_str() + m_i) {
                return fail();
            }
            m_i = static_cast<std::size_t>(end - m_t.c_str());
        }
    }
    const std::string& m_t;
    std::size_t m_i = 0;
    bool m_ok = true;
};

template <std::size_t N>
std::uint32_t valueOf(const tap::names::NameEntry (&table)[N], const std::string& name, std::uint32_t fallback) {
    for (const auto& e : table) {
        if (name == e.name) {
            return e.value;
        }
    }
    return fallback;
}

std::uint32_t poolValue(const std::string& name) { return valueOf(tap::names::kPools, name, 0); }

/// A recorded state block turned back into the arrays tap::DrawState points at.
struct StateBlock {
    std::uint32_t renderStates[tap::kRenderStateCount] = {};
    std::uint32_t textureStageStates[tap::kTextureStageCount][32] = {};
    std::uint32_t samplerStates[tap::kSamplerSlotCount][tap::kSamplerStateCount] = {};
    tap::ResourceId textures[tap::kSamplerSlotCount] = {};
    tap::Viewport viewport;
    tap::ResourceId renderTargets[tap::kRenderTargetCount] = {};
    tap::ResourceId depthStencil = tap::kNoResource;
};

std::unique_ptr<StateBlock> parseStateBlock(const Json& st) {
    auto b = std::make_unique<StateBlock>();
    if (const Json* rs = st.get("render_states")) {
        for (const auto& kv : rs->o) {
            b->renderStates[valueOf(tap::names::kRenderStates, kv.first, 0)] = static_cast<std::uint32_t>(kv.second.n);
        }
    }
    if (const Json* stages = st.get("texture_stages")) {
        for (const Json& s : stages->a) {
            const std::uint32_t stage = s.u32("stage");
            if (const Json* states = s.get("states"); states && stage < tap::kTextureStageCount) {
                for (const auto& kv : states->o) {
                    const std::uint32_t type = valueOf(tap::names::kTextureStageStates, kv.first, 0);
                    if (type >= 1 && type <= 32) {
                        b->textureStageStates[stage][type - 1] = static_cast<std::uint32_t>(kv.second.n);
                    }
                }
            }
        }
    }
    if (const Json* samplers = st.get("samplers")) {
        for (const Json& s : samplers->a) {
            const std::uint32_t slot = tap::samplerSlot(s.u32("sampler"));
            if (const Json* states = s.get("states"); states && slot < tap::kSamplerSlotCount) {
                for (const auto& kv : states->o) {
                    const std::uint32_t type = valueOf(tap::names::kSamplerStates, kv.first, 0);
                    if (type < tap::kSamplerStateCount) {
                        b->samplerStates[slot][type] = static_cast<std::uint32_t>(kv.second.n);
                    }
                }
            }
        }
    }
    if (const Json* textures = st.get("textures")) {
        for (const Json& t : textures->a) {
            const std::uint32_t slot = tap::samplerSlot(t.u32("stage"));
            if (slot < tap::kSamplerSlotCount) {
                b->textures[slot] = t.u32("texture");
            }
        }
    }
    if (const Json* vp = st.get("viewport")) {
        b->viewport.x = vp->u32("x");
        b->viewport.y = vp->u32("y");
        b->viewport.width = vp->u32("width");
        b->viewport.height = vp->u32("height");
        b->viewport.minZ = static_cast<float>(vp->num("min_z"));
        b->viewport.maxZ = static_cast<float>(vp->num("max_z", 1));
    }
    if (const Json* rts = st.get("render_targets")) {
        for (std::size_t i = 0; i < rts->a.size() && i < tap::kRenderTargetCount; ++i) {
            b->renderTargets[i] = rts->a[i].kind == Json::Number ? static_cast<tap::ResourceId>(rts->a[i].n) : 0;
        }
    }
    if (const Json* ds = st.get("depth_stencil"); ds && ds->kind == Json::Number) {
        b->depthStencil = static_cast<tap::ResourceId>(ds->n);
    }
    return b;
}

std::uint32_t transformSlot(const std::string& name) {
    if (name == "VIEW") {
        return tap::kTransformView;
    }
    if (name == "PROJECTION") {
        return tap::kTransformProjection;
    }
    if (name == "WORLD") {
        return tap::kTransformWorld0;
    }
    if (name.rfind("TEXTURE", 0) == 0) {
        return tap::kTransformTexture0 + static_cast<std::uint32_t>(std::atoi(name.c_str() + 7));
    }
    if (name.rfind("WORLDMATRIX_", 0) == 0) {
        return tap::kTransformWorld0 + static_cast<std::uint32_t>(std::atoi(name.c_str() + 12));
    }
    return tap::kTransformCount;
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

const char* decisionName(tap::DrawDecision d) {
    switch (d) {
    case tap::DrawDecision::Raster: return "raster";
    case tap::DrawDecision::Ignore: return "ignore";
    case tap::DrawDecision::RayTracedPreserveRaster: return "raytraced_preserve_raster";
    }
    return "?";
}

int usage() {
    std::fprintf(stderr, "usage: rl_classify_replay --stream <relight_tap.jsonl> [--conf <rtx.conf>] "
                         "[--texture-hashes <file>] --out <classified.jsonl>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string streamPath, confPath, hashesPath, outPath;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--stream") {
            streamPath = argv[i + 1];
        } else if (k == "--conf") {
            confPath = argv[i + 1];
        } else if (k == "--texture-hashes") {
            hashesPath = argv[i + 1];
        } else if (k == "--out") {
            outPath = argv[i + 1];
        } else {
            return usage();
        }
    }
    if (streamPath.empty() || outPath.empty()) {
        return usage();
    }
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    options::OptionLayerHandle layer;
    if (!confPath.empty()) {
        bool found = false;
        const options::OptionConfig config = options::OptionConfig::loadFile(confPath, {}, nullptr, &found);
        if (!found) {
            std::fprintf(stderr, "rl_classify_replay: cannot read %s\n", confPath.c_str());
            return 1;
        }
        layer = options::OptionManager::acquireLayer("", {10000u, "rl_classify_replay"}, 1.0f, 0.1f, false, &config);
    }
    options::OptionManager::applyPendingValues(nullptr, false);

    std::FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "rl_classify_replay: cannot write %s\n", outPath.c_str());
        return 1;
    }
    scene::ClassifyTap classify(nullptr, [out](const scene::ClassifiedDraw& d) {
        const scene::DrawClassification& r = d.result;
        std::fprintf(out,
                     "{\"frame\":%llu,\"index\":%u,\"draw_call_id\":%u,\"status\":\"%s\",\"reason\":\"%s\",\"inject\":%s,"
                     "\"categories\":\"%s\",\"decision\":\"%s\",\"sky_auto\":%s,\"using_rt_rt\":%s,\"drawing_to_rt_rt\":%s,"
                     "\"color_texture\":\"%s\"}\n",
                     static_cast<unsigned long long>(d.frame), d.indexInFrame, r.drawCallId,
                     scene::geometryStatusName(r.status), scene::classifyReasonName(r.reason),
                     r.triggerRtxInjection ? "true" : "false", r.categories.toString().c_str(),
                     decisionName(scene::toTapDecision(r.prepareFlags)), r.skyAutoDetected ? "true" : "false",
                     r.isUsingRaytracedRenderTarget ? "true" : "false",
                     r.isDrawingToRaytracedRenderTarget ? "true" : "false", hex64(r.colorTextureHash).c_str());
    });

    if (!hashesPath.empty()) {
        std::ifstream in(hashesPath);
        unsigned long id = 0;
        std::string h;
        while (in >> id >> h) {
            classify.tracker().setTextureHash(static_cast<tap::ResourceId>(id), std::strtoull(h.c_str(), nullptr, 16));
        }
    }

    std::ifstream in(streamPath);
    if (!in) {
        std::fprintf(stderr, "rl_classify_replay: cannot read %s\n", streamPath.c_str());
        return 1;
    }
    std::vector<std::unique_ptr<StateBlock>> blocks;
    std::vector<float> identity(tap::kTransformCount * 16, 0.0f);
    for (std::uint32_t t = 0; t < tap::kTransformCount; ++t) {
        for (std::uint32_t k = 0; k < 4; ++k) {
            identity[t * 16 + k * 5] = 1.0f;
        }
    }
    std::vector<float> transforms;
    std::size_t lineNo = 0, draws = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        if (line.empty()) {
            continue;
        }
        Json ev;
        if (!Parser(line).parse(ev) || ev.kind != Json::Object) {
            std::fprintf(stderr, "rl_classify_replay: %s:%zu: malformed JSON\n", streamPath.c_str(), lineNo);
            return 1;
        }
        const std::string type = ev.str("ev");
        if (type == "device_create" || type == "device_reset") {
            tap::DeviceEvent e;
            e.d3d8 = ev.flag("d3d8");
            if (const Json* p = ev.get("present")) {
                e.present.backBufferWidth = p->u32("back_buffer_width");
                e.present.backBufferHeight = p->u32("back_buffer_height");
                e.present.backBufferFormat = p->u32("back_buffer_format");
            }
            type == "device_create" ? classify.onDeviceCreate(e) : classify.onDeviceReset(e);
        } else if (type == "texture_create") {
            tap::TextureDesc d;
            d.id = ev.u32("id");
            d.type = ev.u32("type");
            d.width = ev.u32("width");
            d.height = ev.u32("height");
            d.depth = ev.u32("depth");
            d.mipLevels = ev.u32("levels");
            d.arraySize = ev.u32("array_size");
            d.format = ev.u32("format");
            d.usage = ev.u32("usage");
            d.pool = poolValue(ev.str("pool"));
            d.multiSample = ev.u32("multisample");
            d.isBackBuffer = ev.flag("back_buffer");
            d.isAttachmentOnly = ev.flag("attachment_only");
            d.vkImage = ev.flag("has_image") ? 1 : 0;
            classify.onTextureCreate(d);
        } else if (type == "image_destroy") {
            tap::ImageDestroy d;
            d.texture = ev.u32("texture");
            classify.onImageDestroy(d);
        } else if (type == "state_block") {
            const std::size_t index = static_cast<std::size_t>(ev.num("index"));
            if (blocks.size() <= index) {
                blocks.resize(index + 1);
            }
            if (const Json* st = ev.get("state")) {
                blocks[index] = parseStateBlock(*st);
            }
        } else if (type == "draw") {
            const std::size_t index = static_cast<std::size_t>(ev.num("state"));
            if (index >= blocks.size() || !blocks[index]) {
                std::fprintf(stderr, "rl_classify_replay: %s:%zu: unknown state block %zu\n", streamPath.c_str(), lineNo,
                             index);
                return 1;
            }
            const StateBlock& b = *blocks[index];
            tap::DrawCall call;
            const std::string callName = ev.str("call");
            call.call = callName == "DrawIndexedPrimitive"     ? tap::DrawCallType::DrawIndexedPrimitive
                        : callName == "DrawPrimitiveUP"        ? tap::DrawCallType::DrawPrimitiveUP
                        : callName == "DrawIndexedPrimitiveUP" ? tap::DrawCallType::DrawIndexedPrimitiveUP
                                                               : tap::DrawCallType::DrawPrimitive;
            call.primitiveType = valueOf(tap::names::kPrimitiveTypes, ev.str("primitive"), 0);
            call.primitiveCount = ev.u32("prim_count");
            call.startVertex = ev.u32("start_vertex");
            call.vertexCount = ev.u32("vertex_count");
            call.baseVertex = static_cast<std::int32_t>(ev.num("base_vertex"));
            call.minIndex = ev.u32("min_index");
            call.numVertices = ev.u32("num_vertices");
            call.startIndex = ev.u32("start_index");
            call.indexCount = ev.u32("index_count");
            call.instanceCount = ev.u32("instance_count", 1);

            tap::DrawState s;
            s.renderStates = b.renderStates;
            s.textureStageStates = b.textureStageStates;
            s.samplerStates = b.samplerStates;
            std::memcpy(s.textures, b.textures, sizeof s.textures);
            std::memcpy(s.renderTargets, b.renderTargets, sizeof s.renderTargets);
            s.depthStencil = b.depthStencil;
            s.viewport = b.viewport;
            if (const Json* elems = ev.get("elements")) {
                for (const Json& e : elems->a) {
                    if (s.elementCount >= tap::kMaxVertexElements) {
                        break;
                    }
                    tap::VertexElement& ve = s.elements[s.elementCount++];
                    ve.stream = static_cast<std::uint16_t>(e.u32("stream"));
                    ve.offset = static_cast<std::uint16_t>(e.u32("offset"));
                    ve.type = static_cast<std::uint8_t>(valueOf(tap::names::kDeclTypes, e.str("type"), 17));
                    ve.method = static_cast<std::uint8_t>(e.u32("method"));
                    ve.usage = static_cast<std::uint8_t>(valueOf(tap::names::kDeclUsages, e.str("usage"), 0));
                    ve.usageIndex = static_cast<std::uint8_t>(e.u32("usage_index"));
                }
            }
            if (const Json* fvf = ev.get("fvf"); fvf && fvf->kind == Json::Number) {
                s.fvf = static_cast<std::uint32_t>(fvf->n);
            }
            transforms = identity;
            if (const Json* t = ev.get("transforms")) {
                for (const auto& kv : t->o) {
                    const std::uint32_t slot = transformSlot(kv.first);
                    for (std::size_t k = 0; slot < tap::kTransformCount && k < 16 && k < kv.second.a.size(); ++k) {
                        transforms[slot * 16 + k] = static_cast<float>(kv.second.a[k].n);
                    }
                }
            }
            s.transforms = reinterpret_cast<const float(*)[16]>(transforms.data());
            // Shaders: identity and version only (the recording keeps a digest of the bytecode).
            if (const Json* vs = ev.get("vertex_shader"); vs && vs->kind == Json::Object) {
                s.vertexShader.id = vs->u32("id");
            }
            if (const Json* ps = ev.get("pixel_shader"); ps && ps->kind == Json::Object) {
                s.pixelShader.id = ps->u32("id");
            }
            classify.onDraw(call, s);
            ++draws;
        } else if (type == "query_begin" || type == "query_end") {
            tap::QueryEvent q;
            q.type = ev.u32("type");
            type == "query_begin" ? classify.onQueryBegin(q) : classify.onQueryEnd(q);
        } else if (type == "present") {
            tap::FrameEvent f;
            f.frame = static_cast<std::uint64_t>(ev.num("frame"));
            f.width = ev.u32("width");
            f.height = ev.u32("height");
            classify.onPresent(f);
        }
    }
    std::fclose(out);
    // Leave no option dirty at exit: the options registry's dirty list is a function-local static
    // that is destroyed before the (earlier constructed) options, whose destructors still touch it.
    layer = options::OptionLayerHandle();
    options::OptionManager::applyPendingValues(nullptr, false);
    std::printf("rl_classify_replay: %zu draw(s) classified from %s\n", draws, streamPath.c_str());
    return 0;
}
