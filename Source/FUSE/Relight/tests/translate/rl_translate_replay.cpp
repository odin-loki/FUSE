// FUSE Relight RL-1.5: replays a recording tap stream (JSON Lines, "fuse.relight.tap_events/1",
// Source/FUSE/Relight/tap/include/fuse/relight/tap/recording_tap.hpp) through TranslateTap (RL-1.2's
// classifier + this package's material / fog / transform translation, game lights and camera
// classification) and writes one JSON line per draw and one per frame.
//
//   rl_translate_replay --stream relight_tap.jsonl [--conf rtx.conf] [--texture-hashes file] --out out.jsonl
//
// Every recorded event is turned back into the tap event struct DXVK's dispatcher passed (DeviceEvent,
// TextureDesc, DrawCall + DrawState including lights and material, QueryEvent, FrameEvent). Not carried
// by the recording: shader bytecode, clip planes (zero) and Remix texture hashes (supplied per texture id
// in --texture-hashes, "<id> 0x<hash>" lines). --conf is rtx.conf text applied as an option layer.
//
// Output lines (floats %.9g, non-finite floats as strings "inf" / "-inf" / "nan"):
//   {"ev":"draw","frame":F,"index":I,"status":..,"reason":..,"translated":b,"texture_stage":b,
//    "material":{...},"fog":{...},"texgen":..,"texture_transform":[16],"lights":[...],"camera":"Main",
//    "min_z":..,"max_z":..,"z_write":b,"z_enable":b,"stencil":b,"alpha_swizzle":b}
//   {"ev":"frame","frame":F,"lights":[...],"rejected_lights":N,"fog":{...},"fog_states":[...],
//    "cameras":[{...}],"camera_cut":b}
#include <fuse/relight/scene/translate/translate_tap.hpp>
#include <fuse/relight/tap/d3d9_names.hpp>

#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
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
template <typename It>
std::string floats(It begin, It end) {
    std::string s = "[";
    for (It it = begin; it != end; ++it) {
        s += (it == begin ? "" : ",") + num(static_cast<double>(*it));
    }
    return s + "]";
}
std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "\"0x%016llx\"", static_cast<unsigned long long>(v));
    return buf;
}
const char* b(bool v) { return v ? "true" : "false"; }
std::string c4(const tap::Color4& c) {
    const float v[4] = {c.r, c.g, c.b, c.a};
    return floats(v, v + 4);
}

std::string materialJson(const scene::LegacyMaterialRecord& m) {
    const scene::BlendMode& bm = m.blendMode;
    std::string s = "{";
    s += "\"alpha_test\":" + std::string(b(m.alphaTestEnabled));
    s += ",\"alpha_test_op\":" + std::to_string(m.alphaTestCompareOp);
    s += ",\"alpha_ref\":" + std::to_string(m.alphaTestReferenceValue);
    s += ",\"blend\":" + std::string(b(bm.enableBlending));
    s += ",\"color_src\":" + std::to_string(bm.colorSrcFactor) + ",\"color_dst\":" + std::to_string(bm.colorDstFactor) +
         ",\"color_op\":" + std::to_string(bm.colorBlendOp);
    s += ",\"alpha_src\":" + std::to_string(bm.alphaSrcFactor) + ",\"alpha_dst\":" + std::to_string(bm.alphaDstFactor) +
         ",\"alpha_op\":" + std::to_string(bm.alphaBlendOp);
    s += ",\"write_mask\":" + std::to_string(bm.writeMask);
    s += ",\"diffuse_source\":\"" + std::string(scene::textureArgSourceName(m.diffuseColorSource)) + "\"";
    s += ",\"specular_source\":\"" + std::string(scene::textureArgSourceName(m.specularColorSource)) + "\"";
    s += ",\"tfactor\":" + std::to_string(m.tFactor);
    s += ",\"tex_color_op\":\"" + std::string(scene::textureOperationName(m.textureColorOperation)) + "\"";
    s += ",\"tex_color_arg1\":\"" + std::string(scene::textureArgSourceName(m.textureColorArg1Source)) + "\"";
    s += ",\"tex_color_arg2\":\"" + std::string(scene::textureArgSourceName(m.textureColorArg2Source)) + "\"";
    s += ",\"tex_alpha_op\":\"" + std::string(scene::textureOperationName(m.textureAlphaOperation)) + "\"";
    s += ",\"tex_alpha_arg1\":\"" + std::string(scene::textureArgSourceName(m.textureAlphaArg1Source)) + "\"";
    s += ",\"tex_alpha_arg2\":\"" + std::string(scene::textureArgSourceName(m.textureAlphaArg2Source)) + "\"";
    s += ",\"tf_blend\":" + std::string(b(m.isTextureFactorBlend));
    s += ",\"vc_baked\":" + std::string(b(m.isVertexColorBakedLighting));
    s += ",\"d3d_material\":{\"diffuse\":" + c4(m.d3dMaterial.diffuse) + ",\"ambient\":" + c4(m.d3dMaterial.ambient) +
         ",\"specular\":" + c4(m.d3dMaterial.specular) + ",\"emissive\":" + c4(m.d3dMaterial.emissive) +
         ",\"power\":" + num(m.d3dMaterial.power) + "}";
    s += ",\"texture_slots\":[" + std::to_string(m.colorTextureSlots[0]) + "," + std::to_string(m.colorTextureSlots[1]) + "]";
    s += ",\"hash\":" + hex64(m.hash());
    return s + "}";
}

std::string fogJson(const scene::FogRecord& f) {
    return "{\"mode\":" + std::to_string(f.mode) + ",\"color\":" + floats(f.color.begin(), f.color.end()) +
           ",\"scale\":" + num(f.scale) + ",\"end\":" + num(f.end) + ",\"density\":" + num(f.density) +
           ",\"hash\":" + hex64(f.mode == 0 ? 0 : f.hash()) + "}";
}

std::string lightJson(const scene::LightRecord& l) {
    const char* type = l.type == hash::LightType::Distant ? "distant" : "sphere";
    std::string s = "{\"index\":" + std::to_string(l.d3dIndex) + ",\"d3d_type\":" + std::to_string(l.d3dType) +
                    ",\"type\":\"" + type + "\",\"hash\":" + hex64(l.hash) +
                    ",\"radiance\":" + floats(l.radiance.begin(), l.radiance.end()) +
                    ",\"intensity\":" + num(l.intensity);
    if (l.type == hash::LightType::Distant) {
        s += ",\"direction\":" + floats(l.direction.begin(), l.direction.end()) + ",\"half_angle\":" + num(l.halfAngle);
    } else {
        s += ",\"position\":" + floats(l.position.begin(), l.position.end()) + ",\"radius\":" + num(l.radius);
        s += ",\"shaping\":{\"enabled\":" + std::string(b(l.shaping.enabled)) +
             ",\"direction\":" + floats(l.shaping.direction.begin(), l.shaping.direction.end()) +
             ",\"cos_cone\":" + num(l.shaping.cosConeAngle) + ",\"softness\":" + num(l.shaping.coneSoftness) +
             ",\"focus\":" + num(l.shaping.focusExponent) + "}";
    }
    return s + "}";
}

std::string lightsJson(const std::vector<scene::LightRecord>& lights) {
    std::string s = "[";
    for (std::size_t i = 0; i < lights.size(); ++i) {
        s += (i ? "," : "") + lightJson(lights[i]);
    }
    return s + "]";
}

std::string cameraJson(const scene::CameraState& c) {
    const std::array<float, 3> p = c.position();
    const std::array<float, 3> d = c.direction();
    return std::string("{\"type\":\"") + scene::cameraTypeName(c.type) + "\",\"fov\":" + num(c.fov) +
           ",\"aspect\":" + num(c.aspectRatio) + ",\"near\":" + num(c.nearPlane) + ",\"far\":" + num(c.farPlane) +
           ",\"lhs\":" + b(c.isLHS) + ",\"reverse_z\":" + b(c.isReverseZ) + ",\"shear_x\":" + num(c.shearX) +
           ",\"shear_y\":" + num(c.shearY) + ",\"position\":" + floats(p.begin(), p.end()) +
           ",\"direction\":" + floats(d.begin(), d.end()) + ",\"jitter_px\":[" + num(c.jitter.pixelX) + "," +
           num(c.jitter.pixelY) + "],\"jitter\":" + b(c.jitterDetected) + "}";
}

int usage() {
    std::fprintf(stderr, "usage: rl_translate_replay --stream <relight_tap.jsonl> [--conf <rtx.conf>] "
                         "[--texture-hashes <file>] --out <translated.jsonl>\n");
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
            std::fprintf(stderr, "rl_translate_replay: cannot read %s\n", confPath.c_str());
            return 1;
        }
        layer = options::OptionManager::acquireLayer("", {10000u, "rl_translate_replay"}, 1.0f, 0.1f, false, &config);
    }
    options::OptionManager::applyPendingValues(nullptr, false);

    std::FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "rl_translate_replay: cannot write %s\n", outPath.c_str());
        return 1;
    }
    scene::TranslateTap translate(
        nullptr,
        [out](const scene::TranslatedDraw& d) {
            const scene::DrawClassification& r = d.classification;
            std::string s = "{\"ev\":\"draw\",\"frame\":" + std::to_string(d.frame) +
                            ",\"index\":" + std::to_string(d.indexInFrame) + ",\"status\":\"" +
                            scene::geometryStatusName(r.status) + "\",\"reason\":\"" + scene::classifyReasonName(r.reason) +
                            "\",\"categories\":\"" + r.categories.toString() + "\",\"translated\":" + b(d.translated) +
                            ",\"texture_stage\":" + b(d.textureStageApplied);
            if (d.translated) {
                s += ",\"material\":" + materialJson(d.material) + ",\"fog\":" + fogJson(d.fog);
                s += ",\"texgen\":\"" + std::string(scene::texGenModeName(d.transforms.texgenMode)) + "\"";
                s += ",\"texture_transform\":" + floats(d.transforms.textureTransform.begin(), d.transforms.textureTransform.end());
                s += ",\"object_to_view\":" + floats(d.transforms.objectToView.begin(), d.transforms.objectToView.end());
                s += ",\"clip_plane\":" + std::string(b(d.transforms.enableClipPlane));
                s += ",\"lights\":" + lightsJson(d.addedLights);
                s += ",\"min_z\":" + num(d.minZ) + ",\"max_z\":" + num(d.maxZ) + ",\"z_write\":" + b(d.zWriteEnable) +
                     ",\"z_enable\":" + b(d.zEnable) + ",\"stencil\":" + b(d.stencilEnabled);
            }
            s += ",\"camera\":\"" + std::string(scene::cameraTypeName(d.cameraType)) + "\"";
            s += ",\"alpha_swizzle\":" + std::string(b(d.alphaSwizzle)) + "}\n";
            std::fputs(s.c_str(), out);
        },
        [out](const scene::TranslatedFrame& f) {
            std::string s = "{\"ev\":\"frame\",\"frame\":" + std::to_string(f.frame) + ",\"lights\":" + lightsJson(f.lights) +
                            ",\"rejected_lights\":" + std::to_string(f.rejectedLights) + ",\"fog\":" + fogJson(f.fog) +
                            ",\"fog_states\":[";
            for (std::size_t i = 0; i < f.fogStates.size(); ++i) {
                s += (i ? "," : "") + fogJson(f.fogStates[i]);
            }
            s += "],\"cameras\":[";
            for (std::size_t i = 0; i < f.cameras.size(); ++i) {
                s += (i ? "," : "") + cameraJson(f.cameras[i]);
            }
            s += "],\"camera_cut\":" + std::string(b(f.cameraCut)) + "}\n";
            std::fputs(s.c_str(), out);
        });

    if (!hashesPath.empty()) {
        std::ifstream in(hashesPath);
        unsigned long id = 0;
        std::string h;
        while (in >> id >> h) {
            translate.tracker().setTextureHash(static_cast<tap::ResourceId>(id), std::strtoull(h.c_str(), nullptr, 16));
        }
    }

    std::ifstream in(streamPath);
    if (!in) {
        std::fprintf(stderr, "rl_translate_replay: cannot read %s\n", streamPath.c_str());
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
    std::size_t lineNo = 0, draws = 0, frames = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        if (line.empty()) {
            continue;
        }
        Json ev;
        if (!Parser(line).parse(ev) || ev.kind != Json::Object) {
            std::fprintf(stderr, "rl_translate_replay: %s:%zu: malformed JSON\n", streamPath.c_str(), lineNo);
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
                std::fprintf(stderr, "rl_translate_replay: %s:%zu: unknown state block %zu\n", streamPath.c_str(),
                             lineNo, index);
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
            // Shaders: identity only (the recording keeps a digest of the bytecode).
            if (const Json* vs = ev.get("vertex_shader"); vs && vs->kind == Json::Object) {
                s.vertexShader.id = vs->u32("id");
            }
            if (const Json* ps = ev.get("pixel_shader"); ps && ps->kind == Json::Object) {
                s.pixelShader.id = ps->u32("id");
            }
            translate.onDraw(call, s);
            ++draws;
        } else if (type == "query_begin" || type == "query_end") {
            tap::QueryEvent q;
            q.type = ev.u32("type");
            type == "query_begin" ? translate.onQueryBegin(q) : translate.onQueryEnd(q);
        } else if (type == "present") {
            tap::FrameEvent f;
            f.frame = static_cast<std::uint64_t>(ev.num("frame"));
            f.width = ev.u32("width");
            f.height = ev.u32("height");
            translate.onPresent(f);
            ++frames;
        }
    }
    std::fclose(out);
    // Leave no option dirty at exit (see rl_classify_replay).
    layer = options::OptionLayerHandle();
    options::OptionManager::applyPendingValues(nullptr, false);
    std::printf("rl_translate_replay: %zu draw(s), %zu frame(s) translated from %s\n", draws, frames, streamPath.c_str());
    return 0;
}
