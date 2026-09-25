// FUSE Relight RL-6.3: per-game profile format (see profile.hpp).
#include <fuse/relight/setup/profile.hpp>

#include "setup_json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fuse::relight::setup {

namespace {

// Remix rtx.*Textures options (RL-1.2 classify_options.hpp). Short names are ours.
const std::vector<TextureCategory> kCategories = {
    {"ui", "rtx.uiTextures"},
    {"worldUi", "rtx.worldSpaceUiTextures"},
    {"worldUiBackground", "rtx.worldSpaceUiBackgroundTextures"},
    {"sky", "rtx.skyBoxTextures"},
    {"ignore", "rtx.ignoreTextures"},
    {"hide", "rtx.hideInstanceTextures"},
    {"lightmap", "rtx.lightmapTextures"},
    {"particle", "rtx.particleTextures"},
    {"particleEmitter", "rtx.particleEmitterTextures"},
    {"beam", "rtx.beamTextures"},
    {"decal", "rtx.decalTextures"},
    {"terrain", "rtx.terrainTextures"},
    {"animatedWater", "rtx.animatedWaterTextures"},
    {"playerModel", "rtx.playerModelTextures"},
    {"playerModelBody", "rtx.playerModelBodyTextures"},
    {"hairCard", "rtx.hairCardTextures"},
    {"ignoreBakedLighting", "rtx.ignoreBakedLightingTextures"},
    {"ignoreAlpha", "rtx.ignoreAlphaOnTextures"},
    {"smoothNormals", "rtx.smoothNormalsTextures"},
    {"opacityMicromapIgnore", "rtx.opacityMicromapIgnoreTextures"},
    {"antiCulling", "rtx.antiCulling.antiCullingTextures"},
    {"motionBlurMaskOut", "rtx.postfx.motionBlurMaskOutTextures"},
    {"raytracedRenderTarget", "rtx.raytracedRenderTargetTextures"},
};

std::vector<std::uint64_t> sortedHashes(const options::HashSet& set) {
    std::vector<std::uint64_t> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

bool fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

bool readStringList(const json::Value* v, const char* what, std::vector<std::string>& out, std::string* error) {
    if (!v) {
        return true;
    }
    if (v->isString()) {
        out.push_back(v->s);
        return true;
    }
    if (!v->isArray()) {
        return fail(error, std::string(what) + ": expected a string or an array of strings");
    }
    for (const json::Value& e : v->a) {
        if (!e.isString()) {
            return fail(error, std::string(what) + ": expected strings");
        }
        out.push_back(e.s);
    }
    return true;
}

std::string optionValueText(const json::Value& v) {
    switch (v.kind) {
    case json::Value::Kind::String: return v.s;
    case json::Value::Kind::Bool: return v.b ? "True" : "False";
    case json::Value::Kind::Number: return json::number(v.n);
    default: return {};
    }
}

} // namespace

bool ProfileSuggestion::operator==(const ProfileSuggestion& o) const {
    return kind == o.kind && category == o.category && hash == o.hash && key == o.key && value == o.value &&
           confidence == o.confidence && applied == o.applied && reason == o.reason;
}

const std::vector<TextureCategory>& textureCategories() { return kCategories; }

const TextureCategory* findTextureCategory(std::string_view nameOrOptionKey) {
    for (const TextureCategory& c : kCategories) {
        if (nameOrOptionKey == c.name || nameOrOptionKey == c.optionKey) {
            return &c;
        }
    }
    return nullptr;
}

std::string formatHash(std::uint64_t hash) { return options::formatHash(hash); }

bool parseHash(std::string_view text, std::uint64_t& out) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        }
        if (d < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(d);
    }
    out = value;
    return true;
}

bool GameProfile::addTexture(std::string_view category, std::uint64_t hash) {
    const TextureCategory* c = findTextureCategory(category);
    if (!c) {
        return false;
    }
    textures[c->name].add(hash);
    return true;
}

std::size_t GameProfile::acceptSuggestions(std::string_view selector) {
    std::uint64_t hash = 0;
    const bool byHash = parseHash(selector, hash) && selector.size() >= 3;
    std::size_t count = 0;
    for (ProfileSuggestion& s : suggestions) {
        bool hit = selector == "all";
        if (s.kind == ProfileSuggestion::Kind::Texture) {
            hit = hit || (byHash && s.hash == hash) || selector == s.category;
        } else {
            hit = hit || selector == s.key;
        }
        if (!hit || s.applied) {
            continue;
        }
        if (s.kind == ProfileSuggestion::Kind::Texture) {
            if (!addTexture(s.category, s.hash)) {
                continue;
            }
        } else {
            options[s.key] = s.value;
        }
        s.applied = true;
        ++count;
    }
    return count;
}

std::optional<GameProfile> parseProfile(std::string_view text, std::string* error) {
    std::string jsonError;
    const std::optional<json::Value> doc = json::parse(text, &jsonError);
    if (!doc) {
        fail(error, "JSON: " + jsonError);
        return std::nullopt;
    }
    if (!doc->isObject()) {
        fail(error, "a profile is a JSON object");
        return std::nullopt;
    }
    const std::string schema = doc->str("schema");
    if (schema != kProfileSchema) {
        fail(error, "schema '" + schema + "' is not " + kProfileSchema);
        return std::nullopt;
    }
    GameProfile p;
    p.name = doc->str("name");
    p.description = doc->str("description");

    if (const json::Value* m = doc->get("match")) {
        if (!m->isObject()) {
            fail(error, "match: expected an object");
            return std::nullopt;
        }
        std::vector<std::string> hashes;
        if (!readStringList(m->get("exe"), "match.exe", p.match.exeNames, error) ||
            !readStringList(m->get("xxh3"), "match.xxh3", hashes, error)) {
            return std::nullopt;
        }
        for (const std::string& h : hashes) {
            std::uint64_t value = 0;
            if (!parseHash(h, value)) {
                fail(error, "match.xxh3: bad hash '" + h + "'");
                return std::nullopt;
            }
            p.match.exeHashes.push_back(value);
        }
        p.match.requireHash = m->flag("require_hash");
    }

    if (const json::Value* o = doc->get("options")) {
        if (!o->isObject()) {
            fail(error, "options: expected an object");
            return std::nullopt;
        }
        for (const auto& [key, value] : o->o) {
            if (value.isObject() || value.isArray() || value.kind == json::Value::Kind::Null) {
                fail(error, "options." + key + ": expected a string, number or bool");
                return std::nullopt;
            }
            p.options[key] = optionValueText(value);
        }
    }

    if (const json::Value* t = doc->get("textures")) {
        if (!t->isObject()) {
            fail(error, "textures: expected an object");
            return std::nullopt;
        }
        for (const auto& [key, list] : t->o) {
            const TextureCategory* c = findTextureCategory(key);
            if (!c) {
                fail(error, "textures: unknown category '" + key + "'");
                return std::nullopt;
            }
            std::vector<std::string> entries;
            if (!readStringList(&list, ("textures." + key).c_str(), entries, error)) {
                return std::nullopt;
            }
            options::HashSetLayer& set = p.textures[c->name];
            for (const std::string& e : entries) {
                std::string_view v(e);
                const bool negative = !v.empty() && v.front() == '-';
                if (negative) {
                    v.remove_prefix(1);
                }
                std::uint64_t h = 0;
                if (!parseHash(v, h)) {
                    fail(error, "textures." + key + ": bad hash '" + e + "'");
                    return std::nullopt;
                }
                if (negative) {
                    set.remove(h);
                } else {
                    set.add(h);
                }
            }
        }
    }

    if (const json::Value* s = doc->get("suggestions")) {
        if (!s->isArray()) {
            fail(error, "suggestions: expected an array");
            return std::nullopt;
        }
        for (const json::Value& e : s->a) {
            if (!e.isObject()) {
                fail(error, "suggestions: expected objects");
                return std::nullopt;
            }
            ProfileSuggestion sg;
            const std::string kind = e.str("kind", "texture");
            if (kind == "texture") {
                sg.kind = ProfileSuggestion::Kind::Texture;
                const TextureCategory* c = findTextureCategory(e.str("category"));
                if (!c || !parseHash(e.str("hash"), sg.hash)) {
                    fail(error, "suggestions: a texture suggestion needs a known category and a hash");
                    return std::nullopt;
                }
                sg.category = c->name;
            } else if (kind == "option") {
                sg.kind = ProfileSuggestion::Kind::Option;
                sg.key = e.str("key");
                const json::Value* value = e.get("value");
                sg.value = value ? optionValueText(*value) : std::string();
                if (sg.key.empty()) {
                    fail(error, "suggestions: an option suggestion needs a key");
                    return std::nullopt;
                }
            } else {
                fail(error, "suggestions: unknown kind '" + kind + "'");
                return std::nullopt;
            }
            sg.confidence = e.num("confidence");
            sg.applied = e.flag("applied");
            sg.reason = e.str("reason");
            p.suggestions.push_back(std::move(sg));
        }
    }

    if (!readStringList(doc->get("notes"), "notes", p.notes, error)) {
        return std::nullopt;
    }
    return p;
}

std::string writeProfile(const GameProfile& p) {
    std::string out = "{\n";
    out += "  \"schema\": " + json::quote(kProfileSchema) + ",\n";
    out += "  \"name\": " + json::quote(p.name);
    if (!p.description.empty()) {
        out += ",\n  \"description\": " + json::quote(p.description);
    }
    // match
    out += ",\n  \"match\": {";
    {
        std::vector<std::string> names = p.match.exeNames;
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        out += "\"exe\": [";
        for (std::size_t i = 0; i < names.size(); ++i) {
            out += (i ? ", " : "") + json::quote(names[i]);
        }
        out += "], \"xxh3\": [";
        std::vector<std::uint64_t> hashes = p.match.exeHashes;
        std::sort(hashes.begin(), hashes.end());
        hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            out += (i ? ", " : "") + json::quote(formatHash(hashes[i]));
        }
        out += "], \"require_hash\": ";
        out += p.match.requireHash ? "true" : "false";
        out += "}";
    }
    // options
    out += ",\n  \"options\": {";
    {
        bool first = true;
        for (const auto& [key, value] : p.options) {
            out += first ? "\n" : ",\n";
            out += "    " + json::quote(key) + ": " + json::quote(value);
            first = false;
        }
        out += first ? "}" : "\n  }";
    }
    // textures, in category-table order
    out += ",\n  \"textures\": {";
    {
        bool first = true;
        for (const TextureCategory& c : kCategories) {
            const auto it = p.textures.find(c.name);
            if (it == p.textures.end() || it->second.empty()) {
                continue;
            }
            out += first ? "\n" : ",\n";
            out += "    " + json::quote(c.name) + ": [";
            bool firstHash = true;
            for (std::uint64_t h : sortedHashes(it->second.positives())) {
                out += (firstHash ? "" : ", ") + json::quote(formatHash(h));
                firstHash = false;
            }
            for (std::uint64_t h : sortedHashes(it->second.negatives())) {
                out += (firstHash ? "" : ", ") + json::quote("-" + formatHash(h));
                firstHash = false;
            }
            out += "]";
            first = false;
        }
        out += first ? "}" : "\n  }";
    }
    // suggestions, in their order (the assistant sorts them)
    out += ",\n  \"suggestions\": [";
    for (std::size_t i = 0; i < p.suggestions.size(); ++i) {
        const ProfileSuggestion& s = p.suggestions[i];
        out += i ? ",\n    {" : "\n    {";
        if (s.kind == ProfileSuggestion::Kind::Texture) {
            out += "\"kind\": \"texture\", \"category\": " + json::quote(s.category) +
                   ", \"hash\": " + json::quote(formatHash(s.hash));
        } else {
            out += "\"kind\": \"option\", \"key\": " + json::quote(s.key) + ", \"value\": " + json::quote(s.value);
        }
        // Two decimals: confidences are heuristic scores, and the text must round-trip exactly.
        const double rounded = std::round(s.confidence * 100.0) / 100.0;
        out += ", \"confidence\": " + json::number(rounded);
        out += ", \"applied\": ";
        out += s.applied ? "true" : "false";
        out += ", \"reason\": " + json::quote(s.reason) + "}";
    }
    out += p.suggestions.empty() ? "]" : "\n  ]";
    // notes
    out += ",\n  \"notes\": [";
    for (std::size_t i = 0; i < p.notes.size(); ++i) {
        out += (i ? ",\n    " : "\n    ") + json::quote(p.notes[i]);
    }
    out += p.notes.empty() ? "]" : "\n  ]";
    out += "\n}\n";
    return out;
}

std::optional<GameProfile> loadProfileFile(const std::string& path, std::string* error) {
    std::string text;
    if (!readTextFile(path, text)) {
        fail(error, "cannot read " + path);
        return std::nullopt;
    }
    std::string parseError;
    std::optional<GameProfile> p = parseProfile(text, &parseError);
    if (!p) {
        fail(error, path + ": " + parseError);
        return std::nullopt;
    }
    p->sourcePath = path;
    return p;
}

bool saveProfileFile(const GameProfile& profile, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << writeProfile(profile);
    return static_cast<bool>(out);
}

options::OptionConfig profileToConfig(const GameProfile& p) {
    options::OptionConfig config;
    for (const auto& [key, value] : p.options) {
        config.set(key, value);
    }
    for (const TextureCategory& c : kCategories) {
        const auto it = p.textures.find(c.name);
        if (it != p.textures.end() && !it->second.empty()) {
            config.set(c.optionKey, it->second.toString());
        }
    }
    return config;
}

std::string exportRtxConf(const GameProfile& profile) { return profileToConfig(profile).serialize(); }

GameProfile importRtxConf(std::string_view confText, const options::ConfigParseOptions& parseOptions,
                          std::vector<options::ConfigDiagnostic>* diagnostics) {
    const options::OptionConfig config = options::OptionConfig::parse(confText, parseOptions, diagnostics);
    GameProfile p;
    for (const auto& [key, value] : config.entries()) {
        if (const TextureCategory* c = findTextureCategory(key); c && std::string_view(key) == c->optionKey) {
            std::vector<std::string> parts;
            std::string current;
            for (const char ch : value) {
                if (ch == ',') {
                    parts.push_back(current);
                    current.clear();
                } else {
                    current += ch;
                }
            }
            parts.push_back(current);
            options::HashSetLayer set;
            std::vector<std::string> errors;
            if (set.parseFromStrings(parts, &errors) == 0) {
                p.textures[c->name] = set;
                continue;
            }
            // Not a clean hash list: keep the raw text as an option so nothing is lost.
        }
        p.options[key] = value;
    }
    return p;
}

} // namespace fuse::relight::setup
