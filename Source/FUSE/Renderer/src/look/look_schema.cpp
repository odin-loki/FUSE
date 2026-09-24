#include <fuse/renderer/look/look_schema.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace fuse::renderer::look {

namespace {

// ---------------------------------------------------------------------------------------------
// Minimal JSON DOM (load-time only; the per-frame path never touches it).

struct Json {
    enum class Type : u8 { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* find(std::string_view key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

const char* typeName(Json::Type t) {
    switch (t) {
    case Json::Type::Null:
        return "null";
    case Json::Type::Bool:
        return "bool";
    case Json::Type::Number:
        return "number";
    case Json::Type::String:
        return "string";
    case Json::Type::Array:
        return "array";
    case Json::Type::Object:
        return "object";
    }
    return "?";
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : m_text(text) {}

    bool parse(Json& out, std::string& error) {
        skipWs();
        if (!value(out, 0)) {
            error = m_error;
            return false;
        }
        skipWs();
        if (m_pos != m_text.size()) {
            fail("trailing characters after the document");
            error = m_error;
            return false;
        }
        return true;
    }

private:
    std::string_view m_text;
    size_t m_pos = 0;
    std::string m_error;

    bool fail(const char* msg) {
        if (m_error.empty()) {
            u32 line = 1;
            u32 col = 1;
            for (size_t i = 0; i < m_pos && i < m_text.size(); ++i) {
                if (m_text[i] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            m_error = std::to_string(line) + ":" + std::to_string(col) + ": " + msg;
        }
        return false;
    }

    void skipWs() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        const size_t n = std::strlen(word);
        if (m_text.substr(m_pos, n) != word) {
            return fail("invalid literal");
        }
        m_pos += n;
        return true;
    }

    bool string(std::string& out) {
        ++m_pos; // opening quote
        out.clear();
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) {
                break;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out.push_back(e);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (m_pos + 4u > m_text.size()) {
                    return fail("truncated \\u escape");
                }
                u32 cp = 0;
                for (u32 i = 0; i < 4u; ++i) {
                    const char h = m_text[m_pos++];
                    cp <<= 4u;
                    if (h >= '0' && h <= '9') {
                        cp |= static_cast<u32>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        cp |= static_cast<u32>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        cp |= static_cast<u32>(h - 'A' + 10);
                    } else {
                        return fail("bad \\u escape");
                    }
                }
                // UTF-8 encode (BMP only; surrogate pairs are kept as two code units).
                if (cp < 0x80u) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800u) {
                    out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                } else {
                    out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
                    out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                }
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool number(Json& out) {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+')) {
            ++m_pos;
        }
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
                ++m_pos;
            } else {
                break;
            }
        }
        const std::string token(m_text.substr(start, m_pos - start));
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (token.empty() || end != token.c_str() + token.size() || !std::isfinite(v)) {
            m_pos = start;
            return fail("invalid number");
        }
        out.type = Json::Type::Number;
        out.num = v;
        return true;
    }

    bool value(Json& out, u32 depth) {
        if (depth > 64u) {
            return fail("nesting too deep");
        }
        skipWs();
        if (m_pos >= m_text.size()) {
            return fail("unexpected end of input");
        }
        const char c = m_text[m_pos];
        if (c == '{') {
            ++m_pos;
            out.type = Json::Type::Object;
            skipWs();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            for (;;) {
                skipWs();
                if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                    return fail("expected object key");
                }
                std::string key;
                if (!string(key)) {
                    return false;
                }
                for (const auto& kv : out.obj) {
                    if (kv.first == key) {
                        return fail("duplicate object key");
                    }
                }
                skipWs();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                    return fail("expected ':'");
                }
                ++m_pos;
                Json child;
                if (!value(child, depth + 1u)) {
                    return false;
                }
                out.obj.emplace_back(std::move(key), std::move(child));
                skipWs();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                    ++m_pos;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++m_pos;
            out.type = Json::Type::Array;
            skipWs();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            for (;;) {
                Json child;
                if (!value(child, depth + 1u)) {
                    return false;
                }
                out.arr.push_back(std::move(child));
                skipWs();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                    ++m_pos;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.str);
        }
        if (c == 't') {
            out.type = Json::Type::Bool;
            out.b = true;
            return literal("true");
        }
        if (c == 'f') {
            out.type = Json::Type::Bool;
            out.b = false;
            return literal("false");
        }
        if (c == 'n') {
            out.type = Json::Type::Null;
            return literal("null");
        }
        return number(out);
    }
};

// ---------------------------------------------------------------------------------------------
// Document reader

class Reader {
public:
    explicit Reader(LookParseResult& result) : m_result(result) {}

    bool error(const std::string& path, const std::string& msg) {
        if (m_result.error.empty()) {
            m_result.error = path + ": " + msg;
        }
        return false;
    }
    void warn(const std::string& path, const std::string& msg) { m_result.warnings.push_back(path + ": " + msg); }

    bool expect(const Json& v, Json::Type t, const std::string& path) {
        if (v.type != t) {
            return error(path, std::string("expected ") + typeName(t) + ", got " + typeName(v.type));
        }
        return true;
    }

    bool number(const Json& v, const std::string& path, f32& out) {
        if (!expect(v, Json::Type::Number, path)) {
            return false;
        }
        out = static_cast<f32>(v.num);
        return true;
    }

    bool vec3(const Json& v, const std::string& path, math::Vec3& out) {
        if (!expect(v, Json::Type::Array, path)) {
            return false;
        }
        if (v.arr.size() != 3u) {
            return error(path, "expected [x, y, z]");
        }
        f32 c[3];
        for (u32 i = 0; i < 3u; ++i) {
            if (!number(v.arr[i], path + "[" + std::to_string(i) + "]", c[i])) {
                return false;
            }
        }
        out = {c[0], c[1], c[2]};
        return true;
    }

    bool settings(const Json& v, const std::string& path, LookSettings& out) {
        if (!expect(v, Json::Type::Object, path)) {
            return false;
        }
        for (const auto& [nodeKey, node] : v.obj) {
            const std::string nodePath = path + "." + nodeKey;
            LookEffect effect{};
            if (!look_effect_from_key(nodeKey, effect)) {
                warn(nodePath, "unknown effect node (ignored)");
                continue;
            }
            if (!expect(node, Json::Type::Object, nodePath)) {
                return false;
            }
            for (const auto& [paramKey, value] : node.obj) {
                const std::string paramPath = nodePath + "." + paramKey;
                if (effect == LookEffect::ColorGrade && paramKey == "lut") {
                    if (value.type == Json::Type::Null) {
                        out.lut_set = true;
                        out.lut_path.clear();
                    } else if (value.type == Json::Type::String && !value.str.empty()) {
                        out.lut_set = true;
                        out.lut_path = value.str;
                    } else {
                        return error(paramPath, "expected a .cube path string or null");
                    }
                    continue;
                }
                LookParam param{};
                if (!look_param_find(effect, paramKey, param)) {
                    warn(paramPath, "unknown parameter (ignored)");
                    continue;
                }
                const LookParamInfo& info = look_param_info(param);
                switch (info.type) {
                case LookParamType::Float: {
                    f32 f = 0.f;
                    if (!number(value, paramPath, f)) {
                        return false;
                    }
                    out.values.set(param, f);
                    break;
                }
                case LookParamType::Int: {
                    f32 f = 0.f;
                    if (!number(value, paramPath, f)) {
                        return false;
                    }
                    if (f != std::floor(f)) {
                        return error(paramPath, "expected an integer");
                    }
                    out.values.set(param, f);
                    break;
                }
                case LookParamType::Bool:
                    if (!expect(value, Json::Type::Bool, paramPath)) {
                        return false;
                    }
                    out.values.set(param, value.b ? 1.f : 0.f);
                    break;
                case LookParamType::Color: {
                    math::Vec3 c{};
                    if (!vec3(value, paramPath, c)) {
                        return false;
                    }
                    out.values.setColor(param, c);
                    break;
                }
                case LookParamType::Enum: {
                    if (!expect(value, Json::Type::String, paramPath)) {
                        return false;
                    }
                    u32 e = 0;
                    if (!look_param_enum_from_name(param, value.str, e)) {
                        return error(paramPath, "unknown value '" + value.str + "'");
                    }
                    out.values.set(param, static_cast<f32>(e));
                    break;
                }
                }
                const u32 comps = look_param_components(info.type);
                for (u32 c = 0; c < comps; ++c) {
                    const f32 x = out.values.get(param, c);
                    if (x < info.min || x > info.max) {
                        return error(paramPath, "value out of range [" + std::to_string(info.min) + ", " +
                                                    std::to_string(info.max) + "]");
                    }
                }
                out.mask.set(param);
            }
        }
        return true;
    }

    bool namedList(const Json& v, const std::string& path, std::vector<LookWeatherState>& out) {
        if (!expect(v, Json::Type::Array, path)) {
            return false;
        }
        for (size_t i = 0; i < v.arr.size(); ++i) {
            const std::string p = path + "[" + std::to_string(i) + "]";
            const Json& e = v.arr[i];
            if (!expect(e, Json::Type::Object, p)) {
                return false;
            }
            LookWeatherState state;
            const Json* name = e.find("name");
            if (name == nullptr || name->type != Json::Type::String || name->str.empty()) {
                return error(p, "missing \"name\"");
            }
            state.name = name->str;
            for (const auto& other : out) {
                if (other.name == state.name) {
                    return error(p, "duplicate name '" + state.name + "'");
                }
            }
            if (const Json* s = e.find("settings")) {
                if (!settings(*s, p + ".settings", state.settings)) {
                    return false;
                }
            }
            for (const auto& kv : e.obj) {
                if (kv.first != "name" && kv.first != "settings") {
                    warn(p + "." + kv.first, "unknown key (ignored)");
                }
            }
            out.push_back(std::move(state));
        }
        return true;
    }

    bool document(const Json& root, LookDocument& doc) {
        if (!expect(root, Json::Type::Object, "$")) {
            return false;
        }
        const Json* version = root.find("schemaVersion");
        if (version == nullptr) {
            return error("$.schemaVersion", "required");
        }
        if (version->type != Json::Type::Number || version->num != std::floor(version->num) || version->num < 1.0) {
            return error("$.schemaVersion", "expected a positive integer");
        }
        if (version->num > static_cast<double>(kLookSchemaVersion)) {
            return error("$.schemaVersion", "unsupported version " + std::to_string(static_cast<long long>(version->num)) +
                                                " (this build reads up to " + std::to_string(kLookSchemaVersion) + ")");
        }
        doc.schema_version = static_cast<u32>(version->num);
        const Json* kind = root.find("kind");
        if (kind == nullptr || kind->type != Json::Type::String || kind->str != kLookSchemaKind) {
            return error("$.kind", std::string("expected \"") + kLookSchemaKind + "\"");
        }
        for (const auto& [key, value] : root.obj) {
            const std::string path = "$." + key;
            if (key == "schemaVersion" || key == "kind") {
                continue;
            }
            if (key == "name" || key == "description") {
                if (!expect(value, Json::Type::String, path)) {
                    return false;
                }
                (key == "name" ? doc.name : doc.description) = value.str;
            } else if (key == "graph") {
                if (!expect(value, Json::Type::Array, path)) {
                    return false;
                }
                doc.has_graph = true;
                doc.graph.clear();
                for (size_t i = 0; i < value.arr.size(); ++i) {
                    const std::string p = path + "[" + std::to_string(i) + "]";
                    if (!expect(value.arr[i], Json::Type::String, p)) {
                        return false;
                    }
                    LookEffect effect{};
                    if (!look_effect_from_key(value.arr[i].str, effect)) {
                        return error(p, "unknown effect node '" + value.arr[i].str + "'");
                    }
                    if (!doc.graph.push(effect)) {
                        return error(p, "too many graph nodes");
                    }
                }
                const LookGraphValidation gv = doc.graph.validate();
                if (!gv.ok()) {
                    return error(path + "[" + std::to_string(gv.node_index) + "]",
                                 std::string("invalid effect order: ") + look_graph_error_name(gv.error));
                }
            } else if (key == "base") {
                if (!settings(value, path, doc.base)) {
                    return false;
                }
            } else if (key == "timeOfDay") {
                if (!expect(value, Json::Type::Object, path)) {
                    return false;
                }
                if (const Json* interp = value.find("interpolation")) {
                    if (interp->type != Json::Type::String ||
                        (interp->str != "linear" && interp->str != "smooth")) {
                        return error(path + ".interpolation", "expected \"linear\" or \"smooth\"");
                    }
                    doc.time_interpolation =
                        interp->str == "smooth" ? LookTimeInterpolation::Smooth : LookTimeInterpolation::Linear;
                }
                const Json* keys = value.find("keys");
                if (keys != nullptr) {
                    if (!expect(*keys, Json::Type::Array, path + ".keys")) {
                        return false;
                    }
                    for (size_t i = 0; i < keys->arr.size(); ++i) {
                        const std::string p = path + ".keys[" + std::to_string(i) + "]";
                        const Json& k = keys->arr[i];
                        if (!expect(k, Json::Type::Object, p)) {
                            return false;
                        }
                        LookTimeKey key;
                        const Json* hour = k.find("hour");
                        if (hour == nullptr) {
                            return error(p + ".hour", "required");
                        }
                        if (!number(*hour, p + ".hour", key.hour)) {
                            return false;
                        }
                        if (!(key.hour >= 0.f && key.hour < 24.f)) {
                            return error(p + ".hour", "must be in [0, 24)");
                        }
                        for (const auto& other : doc.time_keys) {
                            if (other.hour == key.hour) {
                                return error(p + ".hour", "duplicate key hour");
                            }
                        }
                        if (const Json* s = k.find("settings")) {
                            if (!settings(*s, p + ".settings", key.settings)) {
                                return false;
                            }
                        }
                        doc.time_keys.push_back(std::move(key));
                    }
                }
                for (const auto& kv : value.obj) {
                    if (kv.first != "interpolation" && kv.first != "keys") {
                        warn(path + "." + kv.first, "unknown key (ignored)");
                    }
                }
            } else if (key == "weather") {
                if (!namedList(value, path, doc.weather)) {
                    return false;
                }
            } else if (key == "overrides") {
                if (!namedList(value, path, doc.overrides)) {
                    return false;
                }
            } else if (key == "volumes") {
                if (!expect(value, Json::Type::Array, path)) {
                    return false;
                }
                for (size_t i = 0; i < value.arr.size(); ++i) {
                    if (!volume(value.arr[i], path + "[" + std::to_string(i) + "]", doc)) {
                        return false;
                    }
                }
            } else {
                warn(path, "unknown key (ignored)");
            }
        }
        return true;
    }

    bool volume(const Json& v, const std::string& p, LookDocument& doc) {
        if (!expect(v, Json::Type::Object, p)) {
            return false;
        }
        LookVolumeDesc vol;
        for (const auto& [key, value] : v.obj) {
            const std::string path = p + "." + key;
            if (key == "name") {
                if (!expect(value, Json::Type::String, path)) {
                    return false;
                }
                vol.name = value.str;
            } else if (key == "shape") {
                if (value.type != Json::Type::String || (value.str != "sphere" && value.str != "box")) {
                    return error(path, "expected \"sphere\" or \"box\"");
                }
                vol.shape = value.str == "box" ? LookVolumeShape::Box : LookVolumeShape::Sphere;
            } else if (key == "center") {
                if (!vec3(value, path, vol.center)) {
                    return false;
                }
            } else if (key == "radius") {
                if (!number(value, path, vol.radius)) {
                    return false;
                }
                if (!(vol.radius >= 0.f)) {
                    return error(path, "must be >= 0");
                }
            } else if (key == "halfExtents") {
                if (!vec3(value, path, vol.half_extents)) {
                    return false;
                }
                if (!(vol.half_extents.x >= 0.f && vol.half_extents.y >= 0.f && vol.half_extents.z >= 0.f)) {
                    return error(path, "must be >= 0");
                }
            } else if (key == "falloff") {
                if (!number(value, path, vol.falloff)) {
                    return false;
                }
                if (!(vol.falloff >= 0.f)) {
                    return error(path, "must be >= 0");
                }
            } else if (key == "priority") {
                f32 pr = 0.f;
                if (!number(value, path, pr)) {
                    return false;
                }
                if (pr != std::floor(pr)) {
                    return error(path, "expected an integer");
                }
                vol.priority = static_cast<s32>(pr);
            } else if (key == "weight") {
                if (!number(value, path, vol.weight)) {
                    return false;
                }
                if (!(vol.weight >= 0.f && vol.weight <= 1.f)) {
                    return error(path, "must be in [0, 1]");
                }
            } else if (key == "settings") {
                if (!settings(value, path, vol.settings)) {
                    return false;
                }
            } else {
                warn(path, "unknown key (ignored)");
            }
        }
        doc.volumes.push_back(std::move(vol));
        return true;
    }

private:
    LookParseResult& m_result;
};

// ---------------------------------------------------------------------------------------------
// Writer

std::string num(f32 v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20u) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out += "\"";
    return out;
}

std::string vec(const math::Vec3& v) {
    return "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + "]";
}

void writeSettings(std::string& s, const LookSettings& st, const std::string& indent) {
    s += "{";
    bool firstNode = true;
    for (u32 e = 0; e < kLookEffectCount; ++e) {
        const LookEffect effect = static_cast<LookEffect>(e);
        std::string body;
        bool firstParam = true;
        for (u32 i = 0; i < kLookParamCount; ++i) {
            const LookParamInfo& info = look_param_info(i);
            if (info.effect != effect || !st.mask.test(info.id)) {
                continue;
            }
            body += firstParam ? "" : ", ";
            firstParam = false;
            body += quote(info.key) + ": ";
            switch (info.type) {
            case LookParamType::Bool:
                body += st.values.get(info.id) >= 0.5f ? "true" : "false";
                break;
            case LookParamType::Color:
                body += vec(st.values.color(info.id));
                break;
            case LookParamType::Enum: {
                const char* name = look_param_enum_name(info.id, static_cast<u32>(st.values.get(info.id)));
                body += quote(name != nullptr ? name : "");
                break;
            }
            default:
                body += num(st.values.get(info.id));
                break;
            }
        }
        if (effect == LookEffect::ColorGrade && st.lut_set) {
            body += firstParam ? "" : ", ";
            firstParam = false;
            body += "\"lut\": " + (st.lut_path.empty() ? std::string("null") : quote(st.lut_path));
        }
        if (firstParam) {
            continue;
        }
        s += firstNode ? "\n" : ",\n";
        firstNode = false;
        s += indent + "  " + quote(look_effect_key(effect)) + ": { " + body + " }";
    }
    s += firstNode ? "}" : "\n" + indent + "}";
}

} // namespace

bool LookSettings::operator==(const LookSettings& o) const {
    if (lut_set != o.lut_set || lut_path != o.lut_path) {
        return false;
    }
    for (u32 w = 0; w < LookParamMask::kWords; ++w) {
        if (mask.bits[w] != o.mask.bits[w]) {
            return false;
        }
    }
    for (u32 i = 0; i < kLookParamCount; ++i) {
        const LookParamInfo& info = look_param_info(i);
        if (!mask.test(info.id)) {
            continue;
        }
        for (u32 c = 0; c < look_param_components(info.type); ++c) {
            if (values.get(info.id, c) != o.values.get(info.id, c)) {
                return false;
            }
        }
    }
    return true;
}

bool LookDocument::operator==(const LookDocument& o) const {
    if (schema_version != o.schema_version || name != o.name || description != o.description ||
        has_graph != o.has_graph || !(graph == o.graph) || !(base == o.base) ||
        time_interpolation != o.time_interpolation || time_keys.size() != o.time_keys.size() ||
        weather.size() != o.weather.size() || volumes.size() != o.volumes.size() ||
        overrides.size() != o.overrides.size()) {
        return false;
    }
    for (size_t i = 0; i < time_keys.size(); ++i) {
        if (time_keys[i].hour != o.time_keys[i].hour || !(time_keys[i].settings == o.time_keys[i].settings)) {
            return false;
        }
    }
    for (size_t i = 0; i < weather.size(); ++i) {
        if (weather[i].name != o.weather[i].name || !(weather[i].settings == o.weather[i].settings)) {
            return false;
        }
    }
    for (size_t i = 0; i < overrides.size(); ++i) {
        if (overrides[i].name != o.overrides[i].name || !(overrides[i].settings == o.overrides[i].settings)) {
            return false;
        }
    }
    for (size_t i = 0; i < volumes.size(); ++i) {
        const LookVolumeDesc& a = volumes[i];
        const LookVolumeDesc& b = o.volumes[i];
        if (a.name != b.name || a.shape != b.shape || a.center.x != b.center.x || a.center.y != b.center.y ||
            a.center.z != b.center.z ||
            (a.shape == LookVolumeShape::Sphere && a.radius != b.radius) ||
            (a.shape == LookVolumeShape::Box &&
             (a.half_extents.x != b.half_extents.x || a.half_extents.y != b.half_extents.y ||
              a.half_extents.z != b.half_extents.z)) ||
            a.falloff != b.falloff ||
            a.priority != b.priority || a.weight != b.weight || !(a.settings == b.settings)) {
            return false;
        }
    }
    return true;
}

bool look_parse(std::string_view text, LookDocument& out, LookParseResult& result) {
    result = {};
    Json root;
    std::string syntaxError;
    if (!JsonParser(text).parse(root, syntaxError)) {
        result.error = syntaxError;
        return false;
    }
    LookDocument doc;
    Reader reader(result);
    if (!reader.document(root, doc)) {
        return false;
    }
    out = std::move(doc);
    result.ok = true;
    return true;
}

bool look_load_file(const char* path, LookDocument& out, LookParseResult& result) {
    result = {};
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = std::string("cannot open ") + (path != nullptr ? path : "(null)");
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return look_parse(ss.str(), out, result);
}

std::string look_write(const LookDocument& doc) {
    std::string s = "{\n";
    s += "  \"schemaVersion\": " + std::to_string(kLookSchemaVersion) + ",\n";
    s += "  \"kind\": " + quote(kLookSchemaKind) + ",\n";
    s += "  \"name\": " + quote(doc.name) + ",\n";
    s += "  \"description\": " + quote(doc.description);
    if (doc.has_graph) {
        s += ",\n  \"graph\": [";
        for (u32 i = 0; i < doc.graph.size(); ++i) {
            s += (i == 0u ? "" : ", ") + quote(look_effect_key(doc.graph.at(i)));
        }
        s += "]";
    }
    s += ",\n  \"base\": ";
    writeSettings(s, doc.base, "  ");
    if (!doc.time_keys.empty()) {
        s += ",\n  \"timeOfDay\": {\n    \"interpolation\": ";
        s += doc.time_interpolation == LookTimeInterpolation::Smooth ? "\"smooth\"" : "\"linear\"";
        s += ",\n    \"keys\": [";
        for (size_t i = 0; i < doc.time_keys.size(); ++i) {
            s += i == 0u ? "\n" : ",\n";
            s += "      { \"hour\": " + num(doc.time_keys[i].hour) + ", \"settings\": ";
            writeSettings(s, doc.time_keys[i].settings, "      ");
            s += " }";
        }
        s += "\n    ]\n  }";
    }
    const auto writeNamed = [&s](const char* key, const std::vector<LookWeatherState>& list) {
        if (list.empty()) {
            return;
        }
        s += ",\n  " + quote(key) + ": [";
        for (size_t i = 0; i < list.size(); ++i) {
            s += i == 0u ? "\n" : ",\n";
            s += "    { \"name\": " + quote(list[i].name) + ", \"settings\": ";
            writeSettings(s, list[i].settings, "    ");
            s += " }";
        }
        s += "\n  ]";
    };
    writeNamed("weather", doc.weather);
    if (!doc.volumes.empty()) {
        s += ",\n  \"volumes\": [";
        for (size_t i = 0; i < doc.volumes.size(); ++i) {
            const LookVolumeDesc& v = doc.volumes[i];
            s += i == 0u ? "\n" : ",\n";
            s += "    { \"name\": " + quote(v.name) + ", \"shape\": ";
            s += v.shape == LookVolumeShape::Box ? "\"box\"" : "\"sphere\"";
            s += ", \"center\": " + vec(v.center);
            if (v.shape == LookVolumeShape::Box) {
                s += ", \"halfExtents\": " + vec(v.half_extents);
            } else {
                s += ", \"radius\": " + num(v.radius);
            }
            s += ", \"falloff\": " + num(v.falloff) + ", \"priority\": " + std::to_string(v.priority) +
                 ", \"weight\": " + num(v.weight) + ",\n      \"settings\": ";
            writeSettings(s, v.settings, "      ");
            s += " }";
        }
        s += "\n  ]";
    }
    writeNamed("overrides", doc.overrides);
    s += "\n}\n";
    return s;
}

} // namespace fuse::renderer::look
