#include <fuse/project/cook_kinds.hpp>

#include <fuse/project/json_value.hpp>
#include <fuse/project/t3d_material_parse.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>

namespace fuse::project {

namespace {

namespace fs = std::filesystem;
using json::Value;

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeBytes(const std::string& path, const void* data, usize size) {
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return out && out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

bool parseDocument(std::string_view text, Value& out, std::string* error) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return fail(error, "JSON parse error at " + std::to_string(parsed.error().line) + ":" +
                               std::to_string(parsed.error().column) + ": " + parsed.error().message);
    }
    out = std::move(*parsed);
    return true;
}

CookWriteOutcome refused(CookStatus status, std::string note) {
    CookWriteOutcome outcome;
    outcome.ok = false;
    outcome.status = status;
    outcome.note = std::move(note);
    return outcome;
}

// ---- .fusemat.json -------------------------------------------------------------------------------------------

class Fields {
public:
    Fields(const Value& object, std::string path, std::string* error) : m_obj(object), m_path(std::move(path)), m_error(error) {}

    /// Unknown keys are errors (the renderer's strict schema).
    bool only(std::initializer_list<std::string_view> keys) {
        for (const auto& [key, value] : m_obj.object) {
            (void)value;
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                return fail(m_error, at(key) + ": unknown key");
            }
        }
        return true;
    }
    bool number(std::string_view key, f32& out) {
        const Value* v = m_obj.get(key);
        if (v == nullptr) return true;
        if (!v->isNumber() || !std::isfinite(v->number)) return fail(m_error, at(key) + ": expected a number");
        out = static_cast<f32>(v->number);
        return true;
    }
    bool boolean(std::string_view key, bool& out) {
        const Value* v = m_obj.get(key);
        if (v == nullptr) return true;
        if (!v->isBool()) return fail(m_error, at(key) + ": expected true / false");
        out = v->boolean;
        return true;
    }
    bool string(std::string_view key, std::string& out, bool required = false) {
        const Value* v = m_obj.get(key);
        if (v == nullptr) return required ? fail(m_error, at(key) + ": required") : true;
        if (!v->isString()) return fail(m_error, at(key) + ": expected a string");
        out = v->string;
        return true;
    }
    bool rgb(std::string_view key, f32 (&out)[3]) {
        const Value* v = m_obj.get(key);
        if (v == nullptr) return true;
        if (!v->isArray() || v->array.size() != 3u) return fail(m_error, at(key) + ": expected [r, g, b]");
        for (u32 i = 0; i < 3; ++i) {
            if (!v->array[i].isNumber() || !std::isfinite(v->array[i].number)) {
                return fail(m_error, at(key) + "[" + std::to_string(i) + "]: expected a number");
            }
            out[i] = static_cast<f32>(v->array[i].number);
        }
        return true;
    }
    bool enumeration(std::string_view key, std::initializer_list<std::string_view> names, u32& out) {
        const Value* v = m_obj.get(key);
        if (v == nullptr) return true;
        if (v->isString()) {
            u32 index = 0;
            for (std::string_view name : names) {
                if (v->string == name) {
                    out = index;
                    return true;
                }
                ++index;
            }
        }
        return fail(m_error, at(key) + ": unknown value");
    }
    const Value* object(std::string_view key) {
        const Value* v = m_obj.get(key);
        if (v != nullptr && !v->isObject()) {
            (void)fail(m_error, at(key) + ": expected an object");
            m_bad = true;
            return nullptr;
        }
        return v;
    }
    bool bad() const { return m_bad; }
    std::string at(std::string_view key) const { return m_path.empty() ? std::string(key) : m_path + "." + std::string(key); }

private:
    const Value& m_obj;
    std::string m_path;
    std::string* m_error;
    bool m_bad = false;
};

bool parseTextures(const Value& v, const std::string& path, asset::CookedMaterialTextures& out, std::string* error) {
    Fields f(v, path, error);
    return f.only({"albedo", "normal"}) && f.string("albedo", out.albedo) && f.string("normal", out.normal);
}

constexpr std::initializer_list<std::string_view> kShading = {"default_lit", "subsurface", "foliage",
                                                              "clear_coat",  "cloth",      "unlit"};
constexpr std::initializer_list<std::string_view> kCategory = {"generic", "stone", "soil",    "sand",  "snow",  "ice",
                                                               "wood",    "foliage", "metal", "fabric", "plaster",
                                                               "brick",   "glass", "water",   "flesh", "plastic"};
constexpr std::initializer_list<std::string_view> kWind = {"none", "grass", "leaves", "branch", "trunk"};
constexpr std::initializer_list<std::string_view> kMask = {"constant", "vertex_r", "vertex_g", "vertex_b",
                                                           "vertex_a", "slope_up", "world_height"};
constexpr std::initializer_list<std::string_view> kLayerMode = {"height", "wet"};

bool parseLayer(const Value& v, const std::string& path, asset::CookedMaterialLayer& layer, std::string* error) {
    if (!v.isObject()) return fail(error, path + ": expected an object");
    Fields f(v, path, error);
    if (!f.only({"name", "mode", "mask", "mask_bias", "mask_scale", "coverage", "contrast", "albedo", "roughness",
                 "metallic", "uv_scale", "normal_strength", "textures"}) ||
        !f.string("name", layer.name) || !f.enumeration("mode", kLayerMode, layer.mode) ||
        !f.enumeration("mask", kMask, layer.mask) || !f.number("mask_bias", layer.mask_bias) ||
        !f.number("mask_scale", layer.mask_scale) || !f.number("coverage", layer.coverage) ||
        !f.number("contrast", layer.contrast) || !f.rgb("albedo", layer.albedo) ||
        !f.number("roughness", layer.roughness) || !f.number("metallic", layer.metallic) ||
        !f.number("uv_scale", layer.uv_scale) || !f.number("normal_strength", layer.normal_strength)) {
        return false;
    }
    const Value* tex = f.object("textures");
    return !f.bad() && (tex == nullptr || parseTextures(*tex, f.at("textures"), layer.textures, error));
}

// ---- canonical JSON (.fuselook) ------------------------------------------------------------------------------

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20u) {
                static const char* kHex = "0123456789abcdef";
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 15u];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

void writeCanonical(std::string& out, const Value& v) {
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: {
        char buffer[64];
        const double n = v.number == 0.0 ? 0.0 : v.number; // -0 -> 0
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), n);
        out.append(buffer, result.ptr);
        break;
    }
    case Value::Type::String: writeString(out, v.string); break;
    case Value::Type::Array:
        out += '[';
        for (usize i = 0; i < v.array.size(); ++i) {
            if (i != 0) out += ',';
            writeCanonical(out, v.array[i]);
        }
        out += ']';
        break;
    case Value::Type::Object: {
        std::vector<const std::pair<std::string, Value>*> members;
        members.reserve(v.object.size());
        for (const auto& member : v.object) members.push_back(&member);
        std::sort(members.begin(), members.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
        out += '{';
        for (usize i = 0; i < members.size(); ++i) {
            if (i != 0) out += ',';
            writeString(out, members[i]->first);
            out += ':';
            writeCanonical(out, members[i]->second);
        }
        out += '}';
        break;
    }
    }
}

bool isNumberArray(const Value* v, usize n) {
    if (v == nullptr || !v->isArray() || v->array.size() != n) return false;
    for (const Value& e : v->array) {
        if (!e.isNumber()) return false;
    }
    return true;
}

/// settings = { "<node>": { "<param>": number | bool | [r, g, b] | "enum" | null } }
bool validateSettings(const Value* v, const std::string& path, std::string* error) {
    if (v == nullptr) return true;
    if (!v->isObject()) return fail(error, path + ": expected an object of nodes");
    for (const auto& [node, params] : v->object) {
        if (!params.isObject()) return fail(error, path + "." + node + ": expected an object of parameters");
        for (const auto& [param, value] : params.object) {
            const bool ok = value.isNumber() || value.isBool() || value.isString() ||
                            value.type == Value::Type::Null || isNumberArray(&value, 3);
            if (!ok) return fail(error, path + "." + node + "." + param + ": expected number, bool, [r, g, b], name or null");
        }
    }
    return true;
}

bool validateNamedList(const Value* v, const std::string& path, std::string* error) {
    if (v == nullptr) return true;
    if (!v->isArray()) return fail(error, path + ": expected an array");
    for (usize i = 0; i < v->array.size(); ++i) {
        const Value& e = v->array[i];
        const std::string at = path + "[" + std::to_string(i) + "]";
        if (!e.isObject()) return fail(error, at + ": expected an object");
        const Value* name = e.get("name");
        if (name == nullptr || !name->isString() || name->string.empty()) return fail(error, at + ".name: required string");
        if (!validateSettings(e.get("settings"), at + ".settings", error)) return false;
    }
    return true;
}

bool validateLook(const Value& doc, std::string* error) {
    if (!doc.isObject()) return fail(error, "root: expected an object");
    const Value* version = doc.get("schemaVersion");
    if (version == nullptr || !version->isNumber() || version->number != std::floor(version->number)) {
        return fail(error, "schemaVersion: required integer");
    }
    if (version->number < 1.0 || version->number > 1.0) {
        return fail(error, "schemaVersion: unsupported (this build reads .fuselook schema 1)");
    }
    const Value* kind = doc.get("kind");
    if (kind == nullptr || !kind->isString() || kind->string != "fuse.look") return fail(error, "kind: must be \"fuse.look\"");
    const Value* name = doc.get("name");
    if (name == nullptr || !name->isString() || name->string.empty()) return fail(error, "name: required string");
    if (const Value* d = doc.get("description"); d != nullptr && !d->isString()) return fail(error, "description: expected a string");
    if (const Value* g = doc.get("graph"); g != nullptr) {
        if (!g->isArray()) return fail(error, "graph: expected an array of node names");
        for (usize i = 0; i < g->array.size(); ++i) {
            if (!g->array[i].isString()) return fail(error, "graph[" + std::to_string(i) + "]: expected a node name");
        }
    }
    if (!validateSettings(doc.get("base"), "base", error)) return false;
    if (const Value* tod = doc.get("timeOfDay"); tod != nullptr) {
        if (!tod->isObject()) return fail(error, "timeOfDay: expected an object");
        if (const Value* interp = tod->get("interpolation");
            interp != nullptr && (!interp->isString() || (interp->string != "linear" && interp->string != "smooth"))) {
            return fail(error, "timeOfDay.interpolation: \"linear\" or \"smooth\"");
        }
        const Value* keys = tod->get("keys");
        if (keys != nullptr) {
            if (!keys->isArray()) return fail(error, "timeOfDay.keys: expected an array");
            for (usize i = 0; i < keys->array.size(); ++i) {
                const Value& k = keys->array[i];
                const std::string at = "timeOfDay.keys[" + std::to_string(i) + "]";
                const Value* hour = k.isObject() ? k.get("hour") : nullptr;
                if (hour == nullptr || !hour->isNumber() || hour->number < 0.0 || hour->number >= 24.0) {
                    return fail(error, at + ".hour: number in [0, 24)");
                }
                if (!validateSettings(k.get("settings"), at + ".settings", error)) return false;
            }
        }
    }
    if (!validateNamedList(doc.get("weather"), "weather", error) ||
        !validateNamedList(doc.get("overrides"), "overrides", error) ||
        !validateNamedList(doc.get("volumes"), "volumes", error)) {
        return false;
    }
    if (const Value* volumes = doc.get("volumes"); volumes != nullptr) {
        for (usize i = 0; i < volumes->array.size(); ++i) {
            const Value& vol = volumes->array[i];
            const std::string at = "volumes[" + std::to_string(i) + "]";
            const Value* shape = vol.get("shape");
            const std::string s = shape != nullptr && shape->isString() ? shape->string : "sphere";
            if (s != "sphere" && s != "box") return fail(error, at + ".shape: \"sphere\" or \"box\"");
            if (const Value* c = vol.get("center"); c != nullptr && !isNumberArray(c, 3)) return fail(error, at + ".center: [x, y, z]");
            if (s == "box") {
                if (const Value* h = vol.get("halfExtents"); h != nullptr && !isNumberArray(h, 3)) {
                    return fail(error, at + ".halfExtents: [x, y, z]");
                }
            }
            if (const Value* w = vol.get("weight"); w != nullptr && (!w->isNumber() || w->number < 0.0 || w->number > 1.0)) {
                return fail(error, at + ".weight: number in [0, 1]");
            }
            for (const char* key : {"radius", "falloff", "priority"}) {
                if (const Value* n = vol.get(key); n != nullptr && !n->isNumber()) return fail(error, at + "." + key + ": expected a number");
            }
        }
    }
    return true;
}

} // namespace

// ---- Material ---------------------------------------------------------------------------------------------------

bool parseFusematJson(std::string_view text, asset::CookedMaterial& out, std::string* error) {
    out = asset::CookedMaterial{};
    Value doc;
    if (!parseDocument(text, doc, error)) return false;
    if (!doc.isObject()) return fail(error, "root: expected an object");
    asset::CookedMaterial m;
    Fields f(doc, "", error);
    if (!f.only({"fusemat", "name", "shading_model", "category", "wind", "base", "uv_scale", "triplanar", "stochastic",
                 "macro", "detail", "layers", "procedural"})) {
        return false;
    }
    const Value* version = doc.get("fusemat");
    if (version == nullptr || !version->isNumber() || version->number != 1.0) return fail(error, "fusemat: must be 1");
    if (!f.string("name", m.name, true)) return false;
    if (m.name.empty()) return fail(error, "name: required non-empty string");
    if (!f.enumeration("shading_model", kShading, m.shading) || !f.enumeration("category", kCategory, m.category) ||
        !f.enumeration("wind", kWind, m.wind) || !f.number("uv_scale", m.uv_scale)) {
        return false;
    }
    if (const Value* base = f.object("base"); base != nullptr) {
        Fields b(*base, "base", error);
        if (!b.only({"albedo", "roughness", "metallic", "normal_strength", "textures"}) || !b.rgb("albedo", m.albedo) ||
            !b.number("roughness", m.roughness) || !b.number("metallic", m.metallic) ||
            !b.number("normal_strength", m.normal_strength)) {
            return false;
        }
        const Value* tex = b.object("textures");
        if (b.bad() || (tex != nullptr && !parseTextures(*tex, "base.textures", m.textures, error))) return false;
    }
    if (const Value* tri = f.object("triplanar"); tri != nullptr) {
        Fields t(*tri, "triplanar", error);
        if (!t.only({"enabled", "sharpness"}) || !t.boolean("enabled", m.triplanar) ||
            !t.number("sharpness", m.triplanar_sharpness)) {
            return false;
        }
    }
    if (const Value* st = f.object("stochastic"); st != nullptr) {
        Fields s(*st, "stochastic", error);
        if (!s.only({"enabled", "lattice"}) || !s.boolean("enabled", m.stochastic) ||
            !s.number("lattice", m.stochastic_lattice)) {
            return false;
        }
    }
    if (const Value* macro = f.object("macro"); macro != nullptr) {
        Fields s(*macro, "macro", error);
        if (!s.only({"scale", "strength"}) || !s.number("scale", m.macro_scale) || !s.number("strength", m.macro_strength)) {
            return false;
        }
    }
    if (const Value* detail = f.object("detail"); detail != nullptr) {
        Fields d(*detail, "detail", error);
        if (!d.only({"albedo", "normal", "scale", "strength", "fade"}) || !d.string("albedo", m.detail.albedo) ||
            !d.string("normal", m.detail.normal) || !d.number("scale", m.detail_scale) ||
            !d.number("strength", m.detail_strength)) {
            return false;
        }
        if (const Value* fade = detail->get("fade"); fade != nullptr) {
            if (!isNumberArray(fade, 2)) return fail(error, "detail.fade: expected [start, end]");
            m.detail_fade[0] = static_cast<f32>(fade->array[0].number);
            m.detail_fade[1] = static_cast<f32>(fade->array[1].number);
        }
    }
    if (const Value* layers = doc.get("layers"); layers != nullptr) {
        if (!layers->isArray()) return fail(error, "layers: expected an array");
        if (layers->array.size() > asset::kCookedMaterialMaxLayers) return fail(error, "layers: at most 3");
        for (usize i = 0; i < layers->array.size(); ++i) {
            asset::CookedMaterialLayer layer;
            if (!parseLayer(layers->array[i], "layers[" + std::to_string(i) + "]", layer, error)) return false;
            m.layers.push_back(std::move(layer));
        }
    }
    if (const Value* proc = f.object("procedural"); proc != nullptr) {
        Fields p(*proc, "procedural", error);
        if (!p.only({"function", "params"})) return false;
        if (const Value* fn = proc->get("function"); fn != nullptr) {
            if (!fn->isNumber() || fn->number < 0.0 || fn->number != std::floor(fn->number) || fn->number > 4294967295.0) {
                return fail(error, "procedural.function: expected a u32 id");
            }
            m.procedural_function = static_cast<u32>(fn->number);
        }
        if (const Value* params = proc->get("params"); params != nullptr) {
            if (!params->isArray() || params->array.size() > asset::kCookedMaterialMaxProceduralParams) {
                return fail(error, "procedural.params: up to 8 numbers");
            }
            for (usize i = 0; i < params->array.size(); ++i) {
                if (!params->array[i].isNumber()) return fail(error, "procedural.params[" + std::to_string(i) + "]: expected a number");
                m.procedural_params.push_back(static_cast<f32>(params->array[i].number));
            }
        }
    }
    if (f.bad()) return false;
    if (!asset::validate_cooked_material(m, error)) return false;
    out = std::move(m);
    return true;
}

CookWriteOutcome cookMaterialFile(const std::string& source_path, const std::string& output_path) {
    asset::CookedMaterial material;
    std::string error;
    std::string text;
    if (!readText(source_path, text)) return refused(CookStatus::SourceMissing, "cannot read " + source_path);
    const bool binary = text.size() >= 4u && text.compare(0, 4, "FMAT") == 0;
    if (binary) {
        if (!asset::read_cooked_material(reinterpret_cast<const u8*>(text.data()), text.size(), material, &error)) {
            return refused(CookStatus::MalformedSource, "fusemat binary refused: " + error);
        }
    } else if (!parseFusematJson(text, material, &error)) {
        return refused(CookStatus::MalformedSource, "fusemat json refused: " + error);
    }
    const std::vector<u8> bytes = encodeCookedMaterial(material);
    if (bytes.empty()) return refused(CookStatus::MalformedSource, "fusemat encode refused");
    if (!writeBytes(output_path, bytes.data(), bytes.size())) return refused(CookStatus::OutputError, "cannot write " + output_path);
    CookWriteOutcome outcome;
    outcome.ok = true;
    outcome.status = CookStatus::Ok;
    outcome.byte_count = static_cast<u32>(bytes.size());
    outcome.note = std::string("fusemat '") + material.name + "' from " + (binary ? "binary" : "json") + ", " +
                   std::to_string(material.layers.size()) + " layer(s)";
    return outcome;
}

// ---- Look -------------------------------------------------------------------------------------------------------

bool canonicalizeLookJson(std::string_view text, std::string& out, std::string* error) {
    Value doc;
    if (!parseDocument(text, doc, error) || !validateLook(doc, error)) return false;
    out.clear();
    writeCanonical(out, doc);
    out += '\n';
    return true;
}

CookWriteOutcome cookLookFile(const std::string& source_path, const std::string& output_path) {
    std::string text;
    if (!readText(source_path, text)) return refused(CookStatus::SourceMissing, "cannot read " + source_path);
    std::string canonical;
    std::string error;
    if (!canonicalizeLookJson(text, canonical, &error)) return refused(CookStatus::MalformedSource, "fuselook refused: " + error);
    if (!writeBytes(output_path, canonical.data(), canonical.size())) return refused(CookStatus::OutputError, "cannot write " + output_path);
    CookWriteOutcome outcome;
    outcome.ok = true;
    outcome.status = CookStatus::Ok;
    outcome.byte_count = static_cast<u32>(canonical.size());
    outcome.note = "canonical fuselook";
    return outcome;
}

bool cookedLookLoads(const std::string& path) {
    std::string text;
    std::string canonical;
    return readText(path, text) && canonicalizeLookJson(text, canonical, nullptr) && canonical == text;
}

// ---- Anim -------------------------------------------------------------------------------------------------------

std::string animEventsSidecarPath(const std::string& source_path) {
    return source_path + ".events.json";
}

bool readAnimEventsJson(std::string_view text, std::vector<fuse::cook::AnimCookEvent>& out, std::string* error) {
    out.clear();
    Value doc;
    if (!parseDocument(text, doc, error)) return false;
    const Value* events = doc.isObject() ? doc.get("events") : nullptr;
    if (events == nullptr || !events->isArray()) return fail(error, "events: required array");
    for (usize i = 0; i < events->array.size(); ++i) {
        const Value& e = events->array[i];
        const std::string at = "events[" + std::to_string(i) + "]";
        const Value* name = e.isObject() ? e.get("name") : nullptr;
        const Value* time = e.isObject() ? e.get("time") : nullptr;
        if (name == nullptr || !name->isString() || name->string.empty()) return fail(error, at + ".name: required string");
        if (time == nullptr || !time->isNumber() || !std::isfinite(time->number)) return fail(error, at + ".time: required number");
        out.push_back({name->string, static_cast<f32>(time->number)});
    }
    return true;
}

CookWriteOutcome cookAnimFile(const AnimImportDesc& desc) {
    fuse::cook::AnimCookOptions options;
    options.sample_rate = desc.sample_rate;
    options.clip_index = desc.clip_index;
    options.clip_name = desc.clip_name;
    options.looping = desc.looping;
    options.root_motion = desc.root_motion;
    options.root_joint = desc.root_joint;
    const std::string sidecar = desc.events_path.empty() ? animEventsSidecarPath(desc.input_path) : desc.events_path;
    std::error_code ec;
    if (!desc.events_path.empty() || fs::is_regular_file(sidecar, ec)) {
        std::string text;
        std::string error;
        if (!readText(sidecar, text)) return refused(CookStatus::SourceMissing, "cannot read events " + sidecar);
        if (!readAnimEventsJson(text, options.events, &error)) {
            return refused(CookStatus::MalformedSource, "events sidecar refused: " + error);
        }
    }
    const fuse::cook::CookStubWriteResult written =
        fuse::cook::cook_anim_file(desc.input_path, desc.output_path, options);
    CookWriteOutcome outcome;
    outcome.ok = written.ok;
    outcome.byte_count = written.byteCount;
    outcome.note = written.note;
    if (written.ok) {
        outcome.status = CookStatus::Ok;
        return outcome;
    }
    switch (written.failure) {
    case fuse::cook::CookFailure::ImporterUnavailable: outcome.status = CookStatus::ImporterUnavailable; break;
    case fuse::cook::CookFailure::MalformedSource: outcome.status = CookStatus::MalformedSource; break;
    case fuse::cook::CookFailure::WriteFailed: outcome.status = CookStatus::OutputError; break;
    default: outcome.status = CookStatus::InvalidInput; break;
    }
    return outcome;
}

} // namespace fuse::project
