// FUSE Relight RL-1.7: replays a recorded Relight run through the scene model and writes, per committed
// draw, the instance it became (id, ReplacementInstance, BLAS, how it was matched) with its current and
// previous objectToWorld, and per frame the scene statistics.
//
//   rl_instances_replay --stream relight_tap.jsonl --capture relight_capture.jsonl [--conf rtx.conf] --out out.jsonl
//
// Inputs are one run of an RL-0.4 app with relight.tap.mode = capture + captureRecord (tests/tap_capture):
//   relight_tap.jsonl      the recording tap's event stream: replayed through TranslateTap (RL-1.2 classifier +
//                          RL-1.5 translation: committed draws, legacy material, transforms, camera type, main
//                          camera) exactly as tests/translate/rl_translate_replay does;
//   relight_capture.jsonl  CaptureTap's live record: per draw (frame, draw-in-frame) the RL-1.3 geometry hash
//                          components, bounding box, skinning and asset key, and the RL-1.4 texture hashes of the
//                          bound textures (fed to the classifier before the draw, so the material hash is real).
// Each committed draw becomes a SceneDrawInput (scene_input.hpp) submitted to SceneModel; Present ends the frame
// (SceneModel::endFrame with TranslateTap's main camera).
//
// Output lines:
//   {"ev":"draw","frame":F,"index":I,"committed":b,"instance":N,"ri":N,"blas":N,"match":"identity|transform|
//    nearest|new","path":"preserve|build|refit|instance","created":b,"changed":b,"static":b,
//    "world":[16],"prev":[16],"material":"0x..","topology":"0x.."}
//   {"ev":"frame","frame":F,"draws":N,"preserved":N,"created":N,"instances":N,"ris":N,"blas":N,
//    "gc_expired":N,"gc_destroyed":N,"anti_culled":N,"anti_culling":b,"camera_cut":b,"ids":[...]}
// Floats are %.9g (every float32 round-trips).
//
// The stream-parsing helpers (Json, Parser, valueOf, StateBlock, parseStateBlock, transformSlot) are a copy of
// tests/translate/rl_translate_replay.cpp's (RL-1.5 owns that file).
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>
#include <fuse/relight/scene/translate/translate_tap.hpp>
#include <fuse/relight/tap/d3d9_names.hpp>

#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace fuse::relight;

// ---- minimal JSON reader (the recording tap's output only) --------------------------------------------------
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
    float f32(const char* key, float fallback = 0) const { return static_cast<float>(num(key, fallback)); }
    bool flag(const char* key) const {
        const Json* v = get(key);
        return v && v->kind == Bool && v->b;
    }
    std::string str(const char* key) const {
        const Json* v = get(key);
        return v && v->kind == String ? v->s : std::string();
    }
    float at(std::size_t i) const { return i < a.size() ? static_cast<float>(a[i].n) : 0.0f; }
};

class Parser {
public:
    explicit Parser(const std::string& text) : m_t(text) {}
    bool parse(Json& out) {
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
    // Unnamed values are recorded as "<PREFIX>_<n>".
    const std::size_t us = name.rfind('_');
    if (us != std::string::npos && us + 1 < name.size() && name[us + 1] >= '0' && name[us + 1] <= '9') {
        return static_cast<std::uint32_t>(std::strtoul(name.c_str() + us + 1, nullptr, 10));
    }
    return fallback;
}

tap::Color4 color4(const Json* j) {
    tap::Color4 c;
    if (j && j->kind == Json::Array) {
        c = {j->at(0), j->at(1), j->at(2), j->at(3)};
    }
    return c;
}
tap::Vec3 vec3(const Json* j) {
    tap::Vec3 v;
    if (j && j->kind == Json::Array) {
        v = {j->at(0), j->at(1), j->at(2)};
    }
    return v;
}

/// A recorded state block turned back into the arrays tap::DrawState points at.
struct StateBlock {
    std::uint32_t renderStates[tap::kRenderStateCount] = {};
    std::uint32_t textureStageStates[tap::kTextureStageCount][32] = {};
    std::uint32_t samplerStates[tap::kSamplerSlotCount][tap::kSamplerStateCount] = {};
    tap::ResourceId textures[tap::kSamplerSlotCount] = {};
    std::vector<tap::Light> lights;
    tap::Material material;
    tap::Viewport viewport;
    tap::ResourceId renderTargets[tap::kRenderTargetCount] = {};
    tap::ResourceId depthStencil = tap::kNoResource;
};

std::unique_ptr<StateBlock> parseStateBlock(const Json& st) {
    auto b = std::make_unique<StateBlock>();
    if (const Json* rs = st.get("render_states")) {
        for (const auto& kv : rs->o) {
            const std::uint32_t index = valueOf(tap::names::kRenderStates, kv.first, 0);
            if (index < tap::kRenderStateCount) {
                b->renderStates[index] = static_cast<std::uint32_t>(kv.second.n);
            }
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
    if (const Json* lights = st.get("lights")) {
        for (const Json& l : lights->a) {
            tap::Light light;
            light.index = l.u32("index");
            light.enabled = l.flag("enabled");
            light.type = valueOf(tap::names::kLightTypes, l.str("type"), 0);
            light.diffuse = color4(l.get("diffuse"));
            light.specular = color4(l.get("specular"));
            light.ambient = color4(l.get("ambient"));
            light.position = vec3(l.get("position"));
            light.direction = vec3(l.get("direction"));
            light.range = l.f32("range");
            light.falloff = l.f32("falloff");
            if (const Json* att = l.get("attenuation")) {
                light.attenuation0 = att->at(0);
                light.attenuation1 = att->at(1);
                light.attenuation2 = att->at(2);
            }
            light.theta = l.f32("theta");
            light.phi = l.f32("phi");
            b->lights.push_back(light);
        }
    }
    if (const Json* m = st.get("material"); m && m->kind == Json::Object) {
        b->material.diffuse = color4(m->get("diffuse"));
        b->material.ambient = color4(m->get("ambient"));
        b->material.specular = color4(m->get("specular"));
        b->material.emissive = color4(m->get("emissive"));
        b->material.power = m->f32("power");
    }
    if (const Json* vp = st.get("viewport")) {
        b->viewport.x = vp->u32("x");
        b->viewport.y = vp->u32("y");
        b->viewport.width = vp->u32("width");
        b->viewport.height = vp->u32("height");
        b->viewport.minZ = vp->f32("min_z");
        b->viewport.maxZ = vp->f32("max_z", 1);
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

// ---- the capture record -----------------------------------------------------------------------------------------
struct CapturedGeometry {
    bool captured = false;
    fuse::relight::hash::GeometryHashes hashes;
    fuse::relight::scene::instances::AxisAlignedBoundingBox box;
    std::uint64_t boneHash = 0;
    std::uint32_t numBones = 0;
    std::uint64_t key = 0;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> textures; ///< (texture id, image hash)
};

float bitsFloat(const std::string& hex) {
    const std::uint32_t u = static_cast<std::uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::vector<std::string> splitString(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) {
            return out;
        }
        start = p + 1;
    }
}

bool readCapture(const std::string& path, std::map<std::pair<std::uint64_t, std::uint32_t>, CapturedGeometry>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::size_t lineNo = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        if (line.empty()) {
            continue;
        }
        Json ev;
        if (!Parser(line).parse(ev) || ev.kind != Json::Object) {
            std::fprintf(stderr, "rl_instances_replay: %s:%zu: malformed JSON\n", path.c_str(), lineNo);
            return false;
        }
        if (ev.str("ev") != "draw") {
            continue;
        }
        CapturedGeometry g;
        if (const Json* geo = ev.get("geometry"); geo && geo->str("status") == "captured") {
            g.captured = true;
            const std::vector<std::string> f = splitString(geo->str("f"), ',');
            for (std::size_t i = 0; i < f.size() && i < fuse::relight::hash::kHashComponentCount; ++i) {
                g.hashes.fields[i] = std::strtoull(f[i].c_str(), nullptr, 16);
            }
            g.key = std::strtoull(geo->str("key").c_str(), nullptr, 16);
            const std::vector<std::string> bb = splitString(geo->str("aabb"), ',');
            if (bb.size() == 6) {
                g.box.minPos = {bitsFloat(bb[0]), bitsFloat(bb[1]), bitsFloat(bb[2])};
                g.box.maxPos = {bitsFloat(bb[3]), bitsFloat(bb[4]), bitsFloat(bb[5])};
            }
            const std::vector<std::string> skin = splitString(geo->str("skin"), ':');
            if (skin.size() == 4) {
                g.numBones = static_cast<std::uint32_t>(std::strtoul(skin[0].c_str(), nullptr, 10));
                g.boneHash = std::strtoull(skin[3].c_str(), nullptr, 16);
            }
        }
        if (const Json* tex = ev.get("textures")) {
            for (const Json& t : tex->a) {
                const std::uint64_t h = std::strtoull(t.str("hash").c_str(), nullptr, 16);
                if (h != 0) {
                    g.textures.emplace_back(t.u32("texture"), h);
                }
            }
        }
        out[{static_cast<std::uint64_t>(ev.num("frame")), ev.u32("di")}] = std::move(g);
    }
    return true;
}

// ---- JSON writer ------------------------------------------------------------------------------------------------
std::string num(double v) {
    if (std::isnan(v)) {
        return "\"nan\"";
    }
    if (std::isinf(v)) {
        return v > 0 ? "\"inf\"" : "\"-inf\"";
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}
std::string mat(const fuse::relight::scene::instances::Mat4f& m) {
    std::string s = "[";
    for (std::size_t i = 0; i < 16; ++i) {
        s += (i ? "," : "") + num(static_cast<double>(m[i]));
    }
    return s + "]";
}
std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "\"0x%016llx\"", static_cast<unsigned long long>(v));
    return buf;
}
const char* b(bool v) { return v ? "true" : "false"; }

int usage() {
    std::fprintf(stderr, "usage: rl_instances_replay --stream <relight_tap.jsonl> --capture <relight_capture.jsonl> "
                         "[--conf <rtx.conf>] --out <instances.jsonl>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    namespace inst = fuse::relight::scene::instances;
    std::string streamPath, capturePath, confPath, outPath;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--stream") {
            streamPath = argv[i + 1];
        } else if (k == "--capture") {
            capturePath = argv[i + 1];
        } else if (k == "--conf") {
            confPath = argv[i + 1];
        } else if (k == "--out") {
            outPath = argv[i + 1];
        } else {
            return usage();
        }
    }
    if (streamPath.empty() || capturePath.empty() || outPath.empty()) {
        return usage();
    }
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    inst::registerInstanceOptions();
    options::OptionLayerHandle layer;
    if (!confPath.empty()) {
        bool found = false;
        const options::OptionConfig config = options::OptionConfig::loadFile(confPath, {}, nullptr, &found);
        if (!found) {
            std::fprintf(stderr, "rl_instances_replay: cannot read %s\n", confPath.c_str());
            return 1;
        }
        layer = options::OptionManager::acquireLayer("", {10000u, "rl_instances_replay"}, 1.0f, 0.1f, false, &config);
    }
    options::OptionManager::applyPendingValues(nullptr, false);

    std::map<std::pair<std::uint64_t, std::uint32_t>, CapturedGeometry> captured;
    if (!readCapture(capturePath, captured)) {
        std::fprintf(stderr, "rl_instances_replay: cannot read %s\n", capturePath.c_str());
        return 1;
    }
    std::FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "rl_instances_replay: cannot write %s\n", outPath.c_str());
        return 1;
    }

    std::optional<scene::TranslatedDraw> lastDraw;
    scene::TranslateTap translate(
        nullptr, [&lastDraw](const scene::TranslatedDraw& d) { lastDraw = d; }, nullptr);
    inst::SceneModel model;

    std::ifstream in(streamPath);
    if (!in) {
        std::fprintf(stderr, "rl_instances_replay: cannot read %s\n", streamPath.c_str());
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
    std::size_t lineNo = 0, draws = 0, committed = 0, frames = 0, missing = 0;
    std::uint32_t drawInFrame = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        if (line.empty()) {
            continue;
        }
        Json ev;
        if (!Parser(line).parse(ev) || ev.kind != Json::Object) {
            std::fprintf(stderr, "rl_instances_replay: %s:%zu: malformed JSON\n", streamPath.c_str(), lineNo);
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
            type == "device_create" ? translate.onDeviceCreate(e) : translate.onDeviceReset(e);
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
            d.pool = valueOf(tap::names::kPools, ev.str("pool"), 0);
            d.multiSample = ev.u32("multisample");
            d.isBackBuffer = ev.flag("back_buffer");
            d.isAttachmentOnly = ev.flag("attachment_only");
            d.vkImage = ev.flag("has_image") ? 1 : 0;
            translate.onTextureCreate(d);
        } else if (type == "image_destroy") {
            tap::ImageDestroy d;
            d.texture = ev.u32("texture");
            translate.onImageDestroy(d);
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
                std::fprintf(stderr, "rl_instances_replay: %s:%zu: unknown state block %zu\n", streamPath.c_str(), lineNo,
                             index);
                return 1;
            }
            const StateBlock& blk = *blocks[index];
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
            s.renderStates = blk.renderStates;
            s.textureStageStates = blk.textureStageStates;
            s.samplerStates = blk.samplerStates;
            std::memcpy(s.textures, blk.textures, sizeof s.textures);
            std::memcpy(s.renderTargets, blk.renderTargets, sizeof s.renderTargets);
            s.depthStencil = blk.depthStencil;
            s.viewport = blk.viewport;
            s.lights = blk.lights.empty() ? nullptr : blk.lights.data();
            s.lightCount = static_cast<std::uint32_t>(blk.lights.size());
            s.material = blk.material;
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
            if (const Json* vs = ev.get("vertex_shader"); vs && vs->kind == Json::Object) {
                s.vertexShader.id = vs->u32("id");
            }
            if (const Json* ps = ev.get("pixel_shader"); ps && ps->kind == Json::Object) {
                s.pixelShader.id = ps->u32("id");
            }

            const std::uint64_t frame = static_cast<std::uint64_t>(ev.num("frame"));
            const auto rec = captured.find({frame, drawInFrame});
            if (rec != captured.end()) {
                // The live texture hashes of the bound textures (TextureTracker), before the classifier runs.
                for (const auto& [texture, h] : rec->second.textures) {
                    translate.tracker().setTextureHash(static_cast<tap::ResourceId>(texture), h);
                }
            }
            lastDraw.reset();
            translate.onDraw(call, s);
            ++draws;
            if (!lastDraw) {
                std::fprintf(stderr, "rl_instances_replay: %s:%zu: TranslateTap produced no draw\n", streamPath.c_str(),
                             lineNo);
                return 1;
            }
            std::string line = "{\"ev\":\"draw\",\"frame\":" + std::to_string(frame) +
                               ",\"index\":" + std::to_string(drawInFrame) +
                               ",\"committed\":" + b(lastDraw->classification.committed());
            if (lastDraw->classification.committed()) {
                ++committed;
                inst::SceneDrawInput input = inst::sceneDrawInput(*lastDraw);
                if (rec != captured.end() && rec->second.captured) {
                    input.geometry = rec->second.hashes;
                    input.boundingBox = rec->second.box;
                    input.boneHash = rec->second.boneHash;
                    input.numBones = rec->second.numBones;
                    input.assetHash = rec->second.key;
                } else {
                    ++missing;
                }
                const inst::SceneDrawResult r = model.submitDraw(input);
                line += ",\"instance\":" + std::to_string(r.instanceId) + ",\"ri\":" + std::to_string(r.replacementInstanceId) +
                        ",\"blas\":" + std::to_string(r.blasId) + ",\"match\":\"" + inst::trackerMatchName(r.match) +
                        "\",\"path\":\"" + (r.preserved ? "preserve" : inst::objectCacheStateName(r.cacheState)) +
                        "\",\"created\":" + b(r.created) + ",\"changed\":" + b(r.hasTransformChanged) +
                        ",\"static\":" + b(r.isStatic) + ",\"world\":" + mat(r.objectToWorld) + ",\"prev\":" +
                        mat(r.prevObjectToWorld) + ",\"material\":" + hex64(input.materialHash) +
                        ",\"topology\":" + hex64(input.geometry.hashForRule(fuse::relight::hash::rules::kTopological));
            }
            line += "}\n";
            std::fputs(line.c_str(), out);
            ++drawInFrame;
        } else if (type == "query_begin" || type == "query_end") {
            tap::QueryEvent q;
            q.type = ev.u32("type");
            type == "query_begin" ? translate.onQueryBegin(q) : translate.onQueryEnd(q);
        } else if (type == "present") {
            tap::FrameEvent f;
            f.frame = static_cast<std::uint64_t>(ev.num("frame"));
            f.width = ev.u32("width");
            f.height = ev.u32("height");
            // The main camera as it stands at the end of the frame (before TranslateTap's frame end).
            const scene::CameraState mainCamera = translate.cameras().getMainCamera();
            translate.onPresent(f);
            const inst::SceneFrameStats st = model.endFrame(&mainCamera);
            std::string line = "{\"ev\":\"frame\",\"frame\":" + std::to_string(f.frame) +
                               ",\"draws\":" + std::to_string(st.draws) + ",\"preserved\":" + std::to_string(st.preserved) +
                               ",\"created\":" + std::to_string(st.createdInstances) +
                               ",\"instances\":" + std::to_string(st.activeInstances) +
                               ",\"ris\":" + std::to_string(st.replacementInstances) +
                               ",\"blas\":" + std::to_string(st.blasEntries) +
                               ",\"gc_expired\":" + std::to_string(st.gc.expired) +
                               ",\"gc_destroyed\":" + std::to_string(st.gc.destroyed) +
                               ",\"anti_culled\":" + std::to_string(st.gc.antiCulled) +
                               ",\"anti_culling\":" + b(st.antiCullingSupported) +
                               ",\"camera_cut\":" + b(mainCamera.isCameraCut()) + ",\"ids\":[";
            std::vector<std::uint64_t> alive;
            for (const inst::RtInstance* i : model.instances().getInstanceTable()) {
                alive.push_back(i->getId());
            }
            std::sort(alive.begin(), alive.end());
            for (std::size_t i = 0; i < alive.size(); ++i) {
                line += (i ? "," : "") + std::to_string(alive[i]);
            }
            line += "]}\n";
            std::fputs(line.c_str(), out);
            drawInFrame = 0;
            ++frames;
        }
    }
    std::fclose(out);
    layer = options::OptionLayerHandle();
    options::OptionManager::applyPendingValues(nullptr, false);
    std::printf("rl_instances_replay: %zu draw(s) (%zu committed, %zu without captured geometry), %zu frame(s) from %s\n",
                draws, committed, missing, frames, streamPath.c_str());
    return 0;
}
