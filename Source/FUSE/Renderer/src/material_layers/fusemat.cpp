// Asset plan W0.7: the .fusemat format. See include/fuse/renderer/material_layers/fusemat.hpp.
#include <fuse/renderer/material_layers/fusemat.hpp>

#include <charconv>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace fuse::renderer::material_layers {

namespace {

// --- enum names ------------------------------------------------------------------------------------------------------
constexpr const char* kShadingNames[] = {"default_lit", "subsurface", "foliage", "clear_coat", "cloth", "unlit"};
constexpr const char* kCategoryNames[] = {"generic", "stone",   "soil",    "sand",  "snow",  "ice",
                                          "wood",    "foliage", "metal",   "fabric", "plaster", "brick",
                                          "glass",   "water",   "flesh",   "plastic"};
constexpr const char* kWindNames[] = {"none", "grass", "leaves", "branch", "trunk"};
constexpr const char* kMaskNames[] = {"constant", "vertex_r", "vertex_g", "vertex_b",
                                      "vertex_a", "slope_up", "world_height"};
constexpr const char* kModeNames[] = {"height", "wet"};
static_assert(sizeof(kShadingNames) / sizeof(kShadingNames[0]) == static_cast<usize>(FuseMatShading::Count));
static_assert(sizeof(kCategoryNames) / sizeof(kCategoryNames[0]) == static_cast<usize>(FuseMatCategory::Count));
static_assert(sizeof(kWindNames) / sizeof(kWindNames[0]) == static_cast<usize>(FuseMatWind::Count));
static_assert(sizeof(kMaskNames) / sizeof(kMaskNames[0]) == kMlMaskCount);
static_assert(sizeof(kModeNames) / sizeof(kModeNames[0]) == kMlLayerModeCount);

template <usize N>
bool lookup(const char* const (&names)[N], std::string_view s, u32& out) {
    for (usize i = 0; i < N; ++i) {
        if (s == names[i]) {
            out = static_cast<u32>(i);
            return true;
        }
    }
    return false;
}

// --- minimal JSON DOM (load time only) -----------------------------------------------------------------------------
struct Json {
    enum class Type : u8 { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    std::string text; ///< string value, or the number's source text
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
};

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    bool parse(Json& out, std::string& error) {
        ws();
        if (!value(out, 0)) {
            error = m_error;
            return false;
        }
        ws();
        if (m_pos != m_text.size()) {
            fail("trailing characters");
            error = m_error;
            return false;
        }
        return true;
    }

private:
    void ws() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (c == '\n') {
                    ++m_line;
                    m_col = 1;
                } else {
                    ++m_col;
                }
                ++m_pos;
            } else {
                break;
            }
        }
    }
    bool fail(const char* what) {
        if (m_error.empty()) {
            m_error = std::to_string(m_line) + ":" + std::to_string(m_col) + ": " + what;
        }
        return false;
    }
    void advance(usize n) {
        m_pos += n;
        m_col += static_cast<u32>(n);
    }
    bool literal(const char* word) {
        const usize n = std::strlen(word);
        if (m_text.substr(m_pos, n) != word) {
            return fail("invalid literal");
        }
        advance(n);
        return true;
    }
    bool string(std::string& out) {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
            return fail("expected string");
        }
        advance(1);
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == '"') {
                advance(1);
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                return fail("control character in string");
            }
            if (c == '\\') {
                if (m_pos + 1 >= m_text.size()) {
                    return fail("bad escape");
                }
                const char e = m_text[m_pos + 1];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                default: return fail("unsupported escape"); // \u: ids are ASCII
                }
                advance(2);
                continue;
            }
            out.push_back(c);
            advance(1);
        }
        return fail("unterminated string");
    }
    bool number(Json& out) {
        const usize start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        bool digits = false;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                digits = digits || (c >= '0' && c <= '9');
                ++m_pos;
            } else {
                break;
            }
        }
        m_col += static_cast<u32>(m_pos - start);
        if (!digits) {
            return fail("invalid number");
        }
        out.type = Json::Type::Number;
        out.text = std::string(m_text.substr(start, m_pos - start));
        f64 check = 0.0;
        const auto r = std::from_chars(out.text.data(), out.text.data() + out.text.size(), check);
        if (r.ec != std::errc{} || r.ptr != out.text.data() + out.text.size()) {
            return fail("invalid number");
        }
        return true;
    }
    bool value(Json& out, u32 depth) {
        if (depth > 32u) {
            return fail("nesting too deep");
        }
        if (m_pos >= m_text.size()) {
            return fail("unexpected end");
        }
        const char c = m_text[m_pos];
        if (c == '{') {
            out.type = Json::Type::Object;
            advance(1);
            ws();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                advance(1);
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                for (const auto& kv : out.obj) {
                    if (kv.first == key) {
                        return fail("duplicate key");
                    }
                }
                ws();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                    return fail("expected ':'");
                }
                advance(1);
                ws();
                Json v;
                if (!value(v, depth + 1u)) {
                    return false;
                }
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    advance(1);
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                    advance(1);
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out.type = Json::Type::Array;
            advance(1);
            ws();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                advance(1);
                return true;
            }
            for (;;) {
                ws();
                Json v;
                if (!value(v, depth + 1u)) {
                    return false;
                }
                out.arr.push_back(std::move(v));
                ws();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    advance(1);
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                    advance(1);
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.text);
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

    std::string_view m_text;
    usize m_pos = 0;
    u32 m_line = 1;
    u32 m_col = 1;
    std::string m_error;
};

// --- JSON -> FuseMat -----------------------------------------------------------------------------------------------
class Reader {
public:
    explicit Reader(FuseMatResult& r) : m_result(r) {}

    void error(const std::string& path, std::string message) { m_result.errors.push_back({path, std::move(message)}); }

    /// Object members, with unknown keys reported.
    bool object(const Json& j, const std::string& path, std::initializer_list<const char*> known) {
        if (j.type != Json::Type::Object) {
            error(path, "expected an object");
            return false;
        }
        for (const auto& kv : j.obj) {
            bool ok = false;
            for (const char* k : known) {
                ok = ok || kv.first == k;
            }
            if (!ok) {
                error(join(path, kv.first), "unknown key");
            }
        }
        return true;
    }
    static const Json* find(const Json& j, const char* key) {
        for (const auto& kv : j.obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
    static std::string join(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }

    void f32Value(const Json& j, const char* key, const std::string& path, f32& out) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        if (v->type != Json::Type::Number) {
            error(join(path, key), "expected a number");
            return;
        }
        f32 x = 0.f;
        const auto r = std::from_chars(v->text.data(), v->text.data() + v->text.size(), x);
        if (r.ec != std::errc{} || !std::isfinite(x)) {
            error(join(path, key), "number out of range");
            return;
        }
        out = x;
    }
    void u32Value(const Json& j, const char* key, const std::string& path, u32& out) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        u32 x = 0;
        const auto r = v->type == Json::Type::Number
                           ? std::from_chars(v->text.data(), v->text.data() + v->text.size(), x)
                           : std::from_chars_result{nullptr, std::errc::invalid_argument};
        if (r.ec != std::errc{} || r.ptr != v->text.data() + v->text.size()) {
            error(join(path, key), "expected an unsigned integer");
            return;
        }
        out = x;
    }
    void boolValue(const Json& j, const char* key, const std::string& path, bool& out) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        if (v->type != Json::Type::Bool) {
            error(join(path, key), "expected true or false");
            return;
        }
        out = v->b;
    }
    void stringValue(const Json& j, const char* key, const std::string& path, std::string& out) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        if (v->type != Json::Type::String) {
            error(join(path, key), "expected a string");
            return;
        }
        out = v->text;
    }
    template <usize N>
    void enumValue(const Json& j, const char* key, const std::string& path, const char* const (&names)[N], u32& out) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        if (v->type != Json::Type::String) {
            error(join(path, key), "expected a string");
            return;
        }
        if (!lookup(names, v->text, out)) {
            error(join(path, key), "unknown value \"" + v->text + "\"");
        }
    }
    void floats(const Json& j, const char* key, const std::string& path, f32* out, u32 count) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        if (v->type != Json::Type::Array || v->arr.size() != count) {
            error(join(path, key), "expected an array of " + std::to_string(count) + " numbers");
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            const Json& e = v->arr[i];
            f32 x = 0.f;
            const auto r = e.type == Json::Type::Number ? std::from_chars(e.text.data(), e.text.data() + e.text.size(), x)
                                                        : std::from_chars_result{nullptr, std::errc::invalid_argument};
            if (r.ec != std::errc{} || !std::isfinite(x)) {
                error(join(path, key) + "[" + std::to_string(i) + "]", "expected a number");
                continue;
            }
            out[i] = x;
        }
    }
    void textures(const Json& j, const char* key, const std::string& path, FuseMatTextureSet& out) {
        const Json* v = find(j, key);
        if (v == nullptr) {
            return;
        }
        const std::string p = join(path, key);
        if (!object(*v, p, {"albedo", "normal"})) {
            return;
        }
        stringValue(*v, "albedo", p, out.albedo);
        stringValue(*v, "normal", p, out.normal);
    }

private:
    FuseMatResult& m_result;
};

void readDocument(const Json& root, FuseMat& m, FuseMatResult& result) {
    Reader r(result);
    if (!r.object(root, "", {"fusemat", "name", "shading_model", "category", "wind", "base", "uv_scale", "triplanar",
                             "stochastic", "macro", "detail", "layers", "procedural"})) {
        return;
    }
    if (Reader::find(root, "fusemat") == nullptr) {
        r.error("fusemat", "missing (format version)");
    }
    r.u32Value(root, "fusemat", "", m.version);
    if (Reader::find(root, "name") == nullptr) {
        r.error("name", "missing");
    }
    r.stringValue(root, "name", "", m.name);
    u32 e = static_cast<u32>(m.shading);
    r.enumValue(root, "shading_model", "", kShadingNames, e);
    m.shading = static_cast<FuseMatShading>(e);
    e = static_cast<u32>(m.category);
    r.enumValue(root, "category", "", kCategoryNames, e);
    m.category = static_cast<FuseMatCategory>(e);
    e = static_cast<u32>(m.wind);
    r.enumValue(root, "wind", "", kWindNames, e);
    m.wind = static_cast<FuseMatWind>(e);
    if (const Json* base = Reader::find(root, "base")) {
        if (r.object(*base, "base", {"albedo", "roughness", "metallic", "normal_strength", "textures"})) {
            r.floats(*base, "albedo", "base", m.albedo, 3u);
            r.f32Value(*base, "roughness", "base", m.roughness);
            r.f32Value(*base, "metallic", "base", m.metallic);
            r.f32Value(*base, "normal_strength", "base", m.normalStrength);
            r.textures(*base, "textures", "base", m.textures);
        }
    }
    r.f32Value(root, "uv_scale", "", m.uvScale);
    if (const Json* t = Reader::find(root, "triplanar")) {
        if (r.object(*t, "triplanar", {"enabled", "sharpness"})) {
            r.boolValue(*t, "enabled", "triplanar", m.triplanar);
            r.f32Value(*t, "sharpness", "triplanar", m.triplanarSharpness);
        }
    }
    if (const Json* s = Reader::find(root, "stochastic")) {
        if (r.object(*s, "stochastic", {"enabled", "lattice"})) {
            r.boolValue(*s, "enabled", "stochastic", m.stochastic);
            r.f32Value(*s, "lattice", "stochastic", m.stochasticLattice);
        }
    }
    if (const Json* s = Reader::find(root, "macro")) {
        if (r.object(*s, "macro", {"scale", "strength"})) {
            r.f32Value(*s, "scale", "macro", m.macroScale);
            r.f32Value(*s, "strength", "macro", m.macroStrength);
        }
    }
    if (const Json* d = Reader::find(root, "detail")) {
        if (r.object(*d, "detail", {"albedo", "normal", "scale", "strength", "fade"})) {
            r.stringValue(*d, "albedo", "detail", m.detail.albedo);
            r.stringValue(*d, "normal", "detail", m.detail.normal);
            r.f32Value(*d, "scale", "detail", m.detailScale);
            r.f32Value(*d, "strength", "detail", m.detailStrength);
            r.floats(*d, "fade", "detail", m.detailFade, 2u);
        }
    }
    if (const Json* ls = Reader::find(root, "layers")) {
        if (ls->type != Json::Type::Array) {
            r.error("layers", "expected an array");
        } else {
            for (usize i = 0; i < ls->arr.size(); ++i) {
                const std::string p = "layers[" + std::to_string(i) + "]";
                const Json& lj = ls->arr[i];
                FuseMatLayer l{};
                if (!r.object(lj, p, {"name", "mode", "mask", "mask_bias", "mask_scale", "coverage", "contrast", "albedo",
                                      "roughness", "metallic", "uv_scale", "normal_strength", "textures"})) {
                    continue;
                }
                r.stringValue(lj, "name", p, l.name);
                u32 mode = l.mode;
                r.enumValue(lj, "mode", p, kModeNames, mode);
                l.mode = static_cast<MlLayerMode>(mode);
                u32 mask = l.mask;
                r.enumValue(lj, "mask", p, kMaskNames, mask);
                l.mask = static_cast<MlMask>(mask);
                r.f32Value(lj, "mask_bias", p, l.maskBias);
                r.f32Value(lj, "mask_scale", p, l.maskScale);
                r.f32Value(lj, "coverage", p, l.coverage);
                r.f32Value(lj, "contrast", p, l.contrast);
                r.floats(lj, "albedo", p, l.albedo, 3u);
                r.f32Value(lj, "roughness", p, l.roughness);
                r.f32Value(lj, "metallic", p, l.metallic);
                r.f32Value(lj, "uv_scale", p, l.uvScale);
                r.f32Value(lj, "normal_strength", p, l.normalStrength);
                r.textures(lj, "textures", p, l.textures);
                m.layers.push_back(std::move(l));
            }
        }
    }
    if (const Json* pr = Reader::find(root, "procedural")) {
        if (r.object(*pr, "procedural", {"function", "params"})) {
            r.u32Value(*pr, "function", "procedural", m.proceduralFunction);
            if (const Json* ps = Reader::find(*pr, "params")) {
                if (ps->type != Json::Type::Array) {
                    r.error("procedural.params", "expected an array");
                } else {
                    m.proceduralParams.assign(ps->arr.size(), 0.f);
                    for (usize i = 0; i < ps->arr.size(); ++i) {
                        const Json& e2 = ps->arr[i];
                        f32 x = 0.f;
                        const auto res = e2.type == Json::Type::Number
                                             ? std::from_chars(e2.text.data(), e2.text.data() + e2.text.size(), x)
                                             : std::from_chars_result{nullptr, std::errc::invalid_argument};
                        if (res.ec != std::errc{} || !std::isfinite(x)) {
                            r.error("procedural.params[" + std::to_string(i) + "]", "expected a number");
                        }
                        m.proceduralParams[i] = x;
                    }
                }
            }
        }
    }
}

// --- validation ----------------------------------------------------------------------------------------------------
void checkRange(FuseMatResult& r, const std::string& path, f32 v, f32 lo, f32 hi) {
    if (!(v >= lo && v <= hi)) {
        r.errors.push_back({path, "out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]"});
    }
}
void checkPositive(FuseMatResult& r, const std::string& path, f32 v, f32 hi) {
    if (!(v > 0.f && v <= hi)) {
        r.errors.push_back({path, "must be in (0, " + std::to_string(hi) + "]"});
    }
}
void checkAlbedo(FuseMatResult& r, const std::string& path, const f32* albedo, f32 metallic, bool textured) {
    for (u32 c = 0; c < 3u; ++c) {
        const std::string p = path + "[" + std::to_string(c) + "]";
        if (textured) {
            checkRange(r, p, albedo[c], 0.f, 1.f); // a tint of the texture (the texture gates calibrate it)
        } else if (metallic >= 0.5f) {
            checkRange(r, p, albedo[c], 0.45f, 1.f); // §1.6 metal albedo 180-255 sRGB
        } else {
            checkRange(r, p, albedo[c], 0.02f, 0.9f); // §1.6 dielectric 30-240 sRGB
        }
    }
}
void checkId(FuseMatResult& r, const std::string& path, const std::string& id) {
    if (id.size() > 255u) {
        r.errors.push_back({path, "texture id longer than 255 characters"});
    }
}

// --- binary --------------------------------------------------------------------------------------------------------
u64 fnv1a64(const u8* data, usize size) {
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

class Writer {
public:
    std::vector<u8> bytes;
    void u32v(u32 v) {
        for (u32 i = 0; i < 4u; ++i) {
            bytes.push_back(static_cast<u8>(v >> (8u * i)));
        }
    }
    void u64v(u64 v) {
        for (u32 i = 0; i < 8u; ++i) {
            bytes.push_back(static_cast<u8>(v >> (8u * i)));
        }
    }
    void f32v(f32 v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, 4u);
        u32v(bits);
    }
    void str(const std::string& s) {
        const usize n = s.size() > 0xFFFFu ? 0xFFFFu : s.size();
        bytes.push_back(static_cast<u8>(n));
        bytes.push_back(static_cast<u8>(n >> 8));
        bytes.insert(bytes.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }
};

class Cursor {
public:
    Cursor(const u8* d, usize n) : m_data(d), m_size(n) {}
    bool ok() const { return m_ok; }
    bool done() const { return m_pos == m_size; }
    u32 u32v() {
        if (!need(4u)) {
            return 0;
        }
        u32 v = 0;
        for (u32 i = 0; i < 4u; ++i) {
            v |= static_cast<u32>(m_data[m_pos + i]) << (8u * i);
        }
        m_pos += 4u;
        return v;
    }
    f32 f32v() {
        const u32 bits = u32v();
        f32 v = 0.f;
        std::memcpy(&v, &bits, 4u);
        return v;
    }
    std::string str() {
        if (!need(2u)) {
            return {};
        }
        const usize n = static_cast<usize>(m_data[m_pos]) | (static_cast<usize>(m_data[m_pos + 1]) << 8);
        m_pos += 2u;
        if (!need(n)) {
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), n);
        m_pos += n;
        return s;
    }

private:
    bool need(usize n) {
        if (!m_ok || m_size - m_pos < n) {
            m_ok = false;
            return false;
        }
        return true;
    }
    const u8* m_data;
    usize m_size;
    usize m_pos = 0;
    bool m_ok = true;
};

void writeSet(Writer& w, const FuseMatTextureSet& s) {
    w.str(s.albedo);
    w.str(s.normal);
}
FuseMatTextureSet readSet(Cursor& c) {
    FuseMatTextureSet s;
    s.albedo = c.str();
    s.normal = c.str();
    return s;
}

// --- JSON writer ---------------------------------------------------------------------------------------------------
std::string num(f32 v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}
std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        case '\r': o += "\\r"; break;
        default: o.push_back(c); break;
        }
    }
    return o + "\"";
}
std::string vec(const f32* v, u32 n) {
    std::string o = "[";
    for (u32 i = 0; i < n; ++i) {
        o += (i ? ", " : "") + num(v[i]);
    }
    return o + "]";
}
std::string texSet(const FuseMatTextureSet& s) {
    return "{\"albedo\": " + quoted(s.albedo) + ", \"normal\": " + quoted(s.normal) + "}";
}

} // namespace

std::string FuseMatResult::describe() const {
    std::string o;
    for (const FuseMatError& e : errors) {
        o += (e.path.empty() ? std::string("(document)") : e.path) + ": " + e.message + "\n";
    }
    return o;
}

const char* fusemat_shading_name(FuseMatShading s) {
    const u32 i = static_cast<u32>(s);
    return i < static_cast<u32>(FuseMatShading::Count) ? kShadingNames[i] : "?";
}
const char* fusemat_category_name(FuseMatCategory c) {
    const u32 i = static_cast<u32>(c);
    return i < static_cast<u32>(FuseMatCategory::Count) ? kCategoryNames[i] : "?";
}
const char* fusemat_wind_name(FuseMatWind w) {
    const u32 i = static_cast<u32>(w);
    return i < static_cast<u32>(FuseMatWind::Count) ? kWindNames[i] : "?";
}
const char* fusemat_mask_name(MlMask m) { return m < kMlMaskCount ? kMaskNames[m] : "?"; }
const char* fusemat_layer_mode_name(MlLayerMode m) { return m < kMlLayerModeCount ? kModeNames[m] : "?"; }

FuseMatResult validate_fusemat(const FuseMat& m) {
    FuseMatResult r;
    if (m.version != kFuseMatVersion) {
        r.errors.push_back({"fusemat", "unsupported version " + std::to_string(m.version)});
    }
    if (m.name.empty() || m.name.size() > 255u) {
        r.errors.push_back({"name", "must be 1-255 characters"});
    }
    if (static_cast<u32>(m.shading) >= static_cast<u32>(FuseMatShading::Count)) {
        r.errors.push_back({"shading_model", "unknown"});
    }
    if (static_cast<u32>(m.category) >= static_cast<u32>(FuseMatCategory::Count)) {
        r.errors.push_back({"category", "unknown"});
    }
    if (static_cast<u32>(m.wind) >= static_cast<u32>(FuseMatWind::Count)) {
        r.errors.push_back({"wind", "unknown"});
    }
    checkRange(r, "base.metallic", m.metallic, 0.f, 1.f);
    checkRange(r, "base.roughness", m.roughness, 0.f, 1.f);
    checkAlbedo(r, "base.albedo", m.albedo, m.metallic, !m.textures.albedo.empty());
    checkRange(r, "base.normal_strength", m.normalStrength, 0.f, 4.f);
    checkId(r, "base.textures.albedo", m.textures.albedo);
    checkId(r, "base.textures.normal", m.textures.normal);
    checkPositive(r, "uv_scale", m.uvScale, 1024.f);
    checkRange(r, "triplanar.sharpness", m.triplanarSharpness, 1.f, 16.f);
    checkPositive(r, "stochastic.lattice", m.stochasticLattice, 16.f);
    checkPositive(r, "macro.scale", m.macroScale, 1024.f);
    checkRange(r, "macro.strength", m.macroStrength, 0.f, 1.f);
    checkId(r, "detail.albedo", m.detail.albedo);
    checkId(r, "detail.normal", m.detail.normal);
    checkRange(r, "detail.scale", m.detailScale, 1.f, 32.f);
    checkRange(r, "detail.strength", m.detailStrength, 0.f, 2.f);
    if (!(m.detailFade[0] >= 0.f && m.detailFade[1] > m.detailFade[0])) {
        r.errors.push_back({"detail.fade", "needs 0 <= start < end"});
    }
    if (m.layers.size() > kMlMaxLayers) {
        r.errors.push_back({"layers", "at most " + std::to_string(kMlMaxLayers) + " layers"});
    }
    for (usize i = 0; i < m.layers.size(); ++i) {
        const FuseMatLayer& l = m.layers[i];
        const std::string p = "layers[" + std::to_string(i) + "]";
        if (l.mode >= kMlLayerModeCount) {
            r.errors.push_back({p + ".mode", "unknown"});
        }
        if (l.mask >= kMlMaskCount) {
            r.errors.push_back({p + ".mask", "unknown"});
        }
        checkRange(r, p + ".coverage", l.coverage, 0.f, 1.f);
        checkRange(r, p + ".contrast", l.contrast, 1.f, 64.f);
        checkRange(r, p + ".roughness", l.roughness, 0.f, 1.f);
        checkRange(r, p + ".metallic", l.metallic, 0.f, 1.f);
        checkPositive(r, p + ".uv_scale", l.uvScale, 1024.f);
        checkRange(r, p + ".normal_strength", l.normalStrength, 0.f, 4.f);
        checkRange(r, p + ".mask_scale", l.maskScale, -1024.f, 1024.f);
        checkRange(r, p + ".mask_bias", l.maskBias, -1.0e6f, 1.0e6f);
        if (l.mode == kMlLayerWet) {
            for (u32 c = 0; c < 3u; ++c) {
                const f32 a = l.albedo[c];
                if (!(a > 0.f && a <= 1.f)) {
                    r.errors.push_back({p + ".albedo[" + std::to_string(c) + "]", "wet darkening factor must be in (0, 1]"});
                }
            }
        } else {
            checkAlbedo(r, p + ".albedo", l.albedo, l.metallic, !l.textures.albedo.empty());
        }
        checkId(r, p + ".textures.albedo", l.textures.albedo);
        checkId(r, p + ".textures.normal", l.textures.normal);
    }
    if (m.proceduralParams.size() > kFuseMatMaxProceduralParams) {
        r.errors.push_back({"procedural.params", "at most 8 parameters"});
    }
    r.ok = r.errors.empty();
    return r;
}

FuseMatResult parse_fusemat_json(std::string_view text, FuseMat& out) {
    FuseMatResult r;
    out = FuseMat{};
    Json root;
    std::string syntax;
    Parser p(text);
    if (!p.parse(root, syntax)) {
        r.errors.push_back({"", "syntax: " + syntax});
        return r;
    }
    readDocument(root, out, r);
    if (!r.errors.empty()) {
        return r;
    }
    return validate_fusemat(out);
}

std::string write_fusemat_json(const FuseMat& m) {
    std::string o = "{\n";
    o += "  \"fusemat\": " + std::to_string(m.version) + ",\n";
    o += "  \"name\": " + quoted(m.name) + ",\n";
    o += "  \"shading_model\": " + quoted(fusemat_shading_name(m.shading)) + ",\n";
    o += "  \"category\": " + quoted(fusemat_category_name(m.category)) + ",\n";
    o += "  \"wind\": " + quoted(fusemat_wind_name(m.wind)) + ",\n";
    o += "  \"base\": {\"albedo\": " + vec(m.albedo, 3u) + ", \"roughness\": " + num(m.roughness) +
         ", \"metallic\": " + num(m.metallic) + ", \"normal_strength\": " + num(m.normalStrength) +
         ", \"textures\": " + texSet(m.textures) + "},\n";
    o += "  \"uv_scale\": " + num(m.uvScale) + ",\n";
    o += "  \"triplanar\": {\"enabled\": " + std::string(m.triplanar ? "true" : "false") +
         ", \"sharpness\": " + num(m.triplanarSharpness) + "},\n";
    o += "  \"stochastic\": {\"enabled\": " + std::string(m.stochastic ? "true" : "false") +
         ", \"lattice\": " + num(m.stochasticLattice) + "},\n";
    o += "  \"macro\": {\"scale\": " + num(m.macroScale) + ", \"strength\": " + num(m.macroStrength) + "},\n";
    o += "  \"detail\": {\"albedo\": " + quoted(m.detail.albedo) + ", \"normal\": " + quoted(m.detail.normal) +
         ", \"scale\": " + num(m.detailScale) + ", \"strength\": " + num(m.detailStrength) +
         ", \"fade\": " + vec(m.detailFade, 2u) + "},\n";
    o += "  \"layers\": [";
    for (usize i = 0; i < m.layers.size(); ++i) {
        const FuseMatLayer& l = m.layers[i];
        o += std::string(i ? "," : "") + "\n    {\"name\": " + quoted(l.name) + ", \"mode\": " +
             quoted(fusemat_layer_mode_name(l.mode)) + ", \"mask\": " + quoted(fusemat_mask_name(l.mask)) +
             ", \"mask_bias\": " + num(l.maskBias) + ", \"mask_scale\": " + num(l.maskScale) +
             ", \"coverage\": " + num(l.coverage) + ", \"contrast\": " + num(l.contrast) + ", \"albedo\": " +
             vec(l.albedo, 3u) + ", \"roughness\": " + num(l.roughness) + ", \"metallic\": " + num(l.metallic) +
             ", \"uv_scale\": " + num(l.uvScale) + ", \"normal_strength\": " + num(l.normalStrength) +
             ", \"textures\": " + texSet(l.textures) + "}";
    }
    o += m.layers.empty() ? "],\n" : "\n  ],\n";
    o += "  \"procedural\": {\"function\": " + std::to_string(m.proceduralFunction) + ", \"params\": " +
         vec(m.proceduralParams.data(), static_cast<u32>(m.proceduralParams.size())) + "}\n";
    o += "}\n";
    return o;
}

std::vector<u8> write_fusemat_binary(const FuseMat& m) {
    Writer p; // payload
    p.str(m.name);
    p.u32v(static_cast<u32>(m.shading));
    p.u32v(static_cast<u32>(m.category));
    p.u32v(static_cast<u32>(m.wind));
    for (const f32 a : m.albedo) {
        p.f32v(a);
    }
    p.f32v(m.roughness);
    p.f32v(m.metallic);
    p.f32v(m.normalStrength);
    writeSet(p, m.textures);
    p.f32v(m.uvScale);
    p.u32v(m.triplanar ? 1u : 0u);
    p.f32v(m.triplanarSharpness);
    p.u32v(m.stochastic ? 1u : 0u);
    p.f32v(m.stochasticLattice);
    p.f32v(m.macroScale);
    p.f32v(m.macroStrength);
    writeSet(p, m.detail);
    p.f32v(m.detailScale);
    p.f32v(m.detailStrength);
    p.f32v(m.detailFade[0]);
    p.f32v(m.detailFade[1]);
    p.u32v(static_cast<u32>(m.layers.size()));
    for (const FuseMatLayer& l : m.layers) {
        p.str(l.name);
        p.u32v(l.mode);
        p.u32v(l.mask);
        p.f32v(l.maskBias);
        p.f32v(l.maskScale);
        p.f32v(l.coverage);
        p.f32v(l.contrast);
        for (const f32 a : l.albedo) {
            p.f32v(a);
        }
        p.f32v(l.roughness);
        p.f32v(l.metallic);
        p.f32v(l.uvScale);
        p.f32v(l.normalStrength);
        writeSet(p, l.textures);
    }
    p.u32v(m.proceduralFunction);
    p.u32v(static_cast<u32>(m.proceduralParams.size()));
    for (const f32 v : m.proceduralParams) {
        p.f32v(v);
    }
    Writer out;
    out.u32v(kFuseMatMagic);
    out.u32v(m.version);
    out.u32v(static_cast<u32>(p.bytes.size()));
    out.bytes.insert(out.bytes.end(), p.bytes.begin(), p.bytes.end());
    out.u64v(fnv1a64(out.bytes.data(), out.bytes.size()));
    return std::move(out.bytes);
}

FuseMatResult read_fusemat_binary(const u8* data, usize size, FuseMat& out) {
    FuseMatResult r;
    out = FuseMat{};
    if (data == nullptr || size < 20u) {
        r.errors.push_back({"", "truncated file"});
        return r;
    }
    Cursor head(data, 12u);
    const u32 magic = head.u32v();
    const u32 version = head.u32v();
    const u32 payload = head.u32v();
    if (magic != kFuseMatMagic) {
        r.errors.push_back({"", "bad magic (not a .fusemat)"});
        return r;
    }
    if (version != kFuseMatVersion) {
        r.errors.push_back({"fusemat", "unsupported version " + std::to_string(version)});
        return r;
    }
    if (static_cast<u64>(payload) + 20u != size) {
        r.errors.push_back({"", "size mismatch (truncated or trailing bytes)"});
        return r;
    }
    u64 trailer = 0;
    for (u32 i = 0; i < 8u; ++i) {
        trailer |= static_cast<u64>(data[size - 8u + i]) << (8u * i);
    }
    if (trailer != fnv1a64(data, size - 8u)) {
        r.errors.push_back({"", "checksum mismatch (FNV-1a trailer)"});
        return r;
    }
    Cursor c(data + 12u, payload);
    out.version = version;
    out.name = c.str();
    out.shading = static_cast<FuseMatShading>(c.u32v());
    out.category = static_cast<FuseMatCategory>(c.u32v());
    out.wind = static_cast<FuseMatWind>(c.u32v());
    for (f32& a : out.albedo) {
        a = c.f32v();
    }
    out.roughness = c.f32v();
    out.metallic = c.f32v();
    out.normalStrength = c.f32v();
    out.textures = readSet(c);
    out.uvScale = c.f32v();
    out.triplanar = c.u32v() != 0u;
    out.triplanarSharpness = c.f32v();
    out.stochastic = c.u32v() != 0u;
    out.stochasticLattice = c.f32v();
    out.macroScale = c.f32v();
    out.macroStrength = c.f32v();
    out.detail = readSet(c);
    out.detailScale = c.f32v();
    out.detailStrength = c.f32v();
    out.detailFade[0] = c.f32v();
    out.detailFade[1] = c.f32v();
    const u32 layers = c.u32v();
    if (!c.ok() || layers > kMlMaxLayers) {
        r.errors.push_back({"layers", "bad layer count"});
        return r;
    }
    for (u32 i = 0; i < layers; ++i) {
        FuseMatLayer l{};
        l.name = c.str();
        l.mode = static_cast<MlLayerMode>(c.u32v());
        l.mask = static_cast<MlMask>(c.u32v());
        l.maskBias = c.f32v();
        l.maskScale = c.f32v();
        l.coverage = c.f32v();
        l.contrast = c.f32v();
        for (f32& a : l.albedo) {
            a = c.f32v();
        }
        l.roughness = c.f32v();
        l.metallic = c.f32v();
        l.uvScale = c.f32v();
        l.normalStrength = c.f32v();
        l.textures = readSet(c);
        out.layers.push_back(std::move(l));
    }
    out.proceduralFunction = c.u32v();
    const u32 params = c.u32v();
    if (!c.ok() || params > kFuseMatMaxProceduralParams) {
        r.errors.push_back({"procedural.params", "bad parameter count"});
        return r;
    }
    for (u32 i = 0; i < params; ++i) {
        out.proceduralParams.push_back(c.f32v());
    }
    if (!c.ok() || !c.done()) {
        r.errors.push_back({"", "payload size does not match its fields"});
        return r;
    }
    return validate_fusemat(out);
}

FuseMatResult resolve_fusemat(const FuseMat& m, const FuseMatTextureResolver& resolve, MlMaterial& out) {
    FuseMatResult r = validate_fusemat(m);
    if (!r.ok) {
        return r;
    }
    auto tex = [&](const std::string& id, const std::string& path) -> u32 {
        if (id.empty()) {
            return kMlNoTexture;
        }
        const u32 index = resolve ? resolve(id) : kMlNoTexture;
        if (index == kMlNoTexture) {
            r.errors.push_back({path, "unknown texture \"" + id + "\""});
        }
        return index;
    };
    MlMaterial g{};
    for (u32 c = 0; c < 3u; ++c) {
        g.albedo[c] = m.albedo[c];
    }
    g.roughness = m.roughness;
    g.metallic = m.metallic;
    g.uvScale = m.uvScale;
    g.normalStrength = m.normalStrength;
    g.albedoTex = tex(m.textures.albedo, "base.textures.albedo");
    g.normalTex = tex(m.textures.normal, "base.textures.normal");
    g.flags = (m.triplanar ? static_cast<u32>(kMlFlagTriplanar) : 0u) |
              (m.stochastic ? static_cast<u32>(kMlFlagStochastic) : 0u) |
              (m.macroStrength > 0.f ? static_cast<u32>(kMlFlagMacro) : 0u);
    const bool detail = !m.detail.albedo.empty() || !m.detail.normal.empty();
    if (detail && m.detailStrength > 0.f) {
        g.flags |= kMlFlagDetail;
    }
    g.detailAlbedoTex = tex(m.detail.albedo, "detail.albedo");
    g.detailNormalTex = tex(m.detail.normal, "detail.normal");
    g.detailScale = m.detailScale;
    g.detailStrength = m.detailStrength;
    g.detailFadeStart = m.detailFade[0];
    g.detailFadeEnd = m.detailFade[1];
    g.triplanarSharpness = m.triplanarSharpness;
    g.stochasticLattice = m.stochasticLattice;
    g.macroScale = m.macroScale;
    g.macroStrength = m.macroStrength;
    g.shadingModel = static_cast<u32>(m.shading);
    g.category = static_cast<u32>(m.category);
    g.procedural = m.proceduralFunction;
    g.layerCount = static_cast<u32>(m.layers.size());
    for (usize i = 0; i < m.layers.size(); ++i) {
        const FuseMatLayer& l = m.layers[i];
        MlLayer& d = g.layers[i];
        const std::string p = "layers[" + std::to_string(i) + "].textures.";
        for (u32 c = 0; c < 3u; ++c) {
            d.albedo[c] = l.albedo[c];
        }
        d.roughness = l.roughness;
        d.metallic = l.metallic;
        d.uvScale = l.uvScale;
        d.contrast = l.contrast;
        d.coverage = l.coverage;
        d.albedoTex = tex(l.textures.albedo, p + "albedo");
        d.normalTex = tex(l.textures.normal, p + "normal");
        d.mask = l.mask;
        d.mode = l.mode;
        d.maskBias = l.maskBias;
        d.maskScale = l.maskScale;
        d.normalStrength = l.normalStrength;
    }
    r.ok = r.errors.empty();
    if (r.ok) {
        out = g;
    }
    return r;
}

} // namespace fuse::renderer::material_layers
