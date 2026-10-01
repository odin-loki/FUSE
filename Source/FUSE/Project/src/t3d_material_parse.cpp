// UNI-U7-MAT-1: T3D Material definitions -> `.fusemat` + strict `.fusetex` cooks (see the header).

#include <fuse/project/t3d_material_parse.hpp>

#include <fuse/io/vfs.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace fuse::project {

namespace {

namespace fs = std::filesystem;

std::string toLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string trim(std::string_view text) {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool readMaterialSourceFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// TorqueScript with comments blanked out (string literals kept; offsets preserved).
std::string stripComments(std::string_view text) {
    std::string out(text);
    usize i = 0;
    while (i < out.size()) {
        const char c = out[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < out.size() && out[i] != quote) {
                i += (out[i] == '\\' && i + 1 < out.size()) ? 2u : 1u;
            }
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') {
                out[i++] = ' ';
            }
            continue;
        }
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            while (i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/')) {
                if (out[i] != '\n') {
                    out[i] = ' ';
                }
                ++i;
            }
            if (i + 1 < out.size()) {
                out[i] = ' ';
                out[i + 1] = ' ';
                i += 2;
            }
            continue;
        }
        ++i;
    }
    return out;
}

bool parseBool(const std::string& value) {
    const std::string v = toLower(trim(value));
    return v == "1" || v == "true" || v == "yes";
}

bool parseFloat(const std::string& value, f32& out) {
    std::istringstream stream(value);
    f32 v = 0.f;
    if (!(stream >> v) || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

void parseColor(const std::string& value, f32 out[4]) {
    std::istringstream stream(value);
    f32 v[4] = {out[0], out[1], out[2], out[3]};
    for (f32& c : v) {
        f32 x = 0.f;
        if (!(stream >> x) || !std::isfinite(x)) {
            break;
        }
        c = x;
    }
    for (u32 i = 0; i < 4u; ++i) {
        out[i] = v[i];
    }
}

std::string stripAssetPrefix(std::string value) {
    constexpr std::string_view kPrefix = "@asset=";
    if (value.size() >= kPrefix.size() && iequals(std::string_view(value).substr(0, kPrefix.size()), kPrefix)) {
        value.erase(0, kPrefix.size());
    }
    return value;
}

/// Applies one `key = value` (key lower-cased, stage index already stripped).
void applyField(T3DMaterialDef& m, const std::string& key, const std::string& value) {
    if (key == "mapto") {
        m.mapTo = value;
    } else if (key == "diffusemap") {
        m.diffuseMap = value;
    } else if (key == "diffusemapasset") {
        m.diffuseMapAsset = stripAssetPrefix(value);
    } else if (key == "normalmap") {
        m.normalMap = value;
    } else if (key == "normalmapasset") {
        m.normalMapAsset = stripAssetPrefix(value);
    } else if (key == "diffusecolor") {
        parseColor(value, m.diffuseColor);
    } else if (key == "roughness") {
        m.hasRoughness = parseFloat(value, m.roughness) || m.hasRoughness;
    } else if (key == "metalness") {
        m.hasMetalness = parseFloat(value, m.metalness) || m.hasMetalness;
    } else if (key == "specularpower") {
        m.hasSpecularPower = parseFloat(value, m.specularPower) || m.hasSpecularPower;
    } else if (key == "emissive") {
        m.emissive = parseBool(value);
    } else if (key == "translucent") {
        m.translucent = parseBool(value);
    } else if (key == "alphatest") {
        m.alphaTest = parseBool(value);
    } else if (key == "alpharef") {
        f32 ref = 0.f;
        if (parseFloat(value, ref)) {
            // T3D stores alphaRef as 0..255.
            m.alphaRef = ref > 1.f ? ref / 255.f : ref;
        }
    } else if (key == "doublesided") {
        m.doubleSided = parseBool(value);
    }
}

/// `key[idx]` -> ("key", idx); a bare key is stage 0.
bool splitStageKey(const std::string& raw, std::string& key, u32& stage) {
    const usize bracket = raw.find('[');
    stage = 0u;
    if (bracket == std::string::npos) {
        key = toLower(trim(raw));
        return !key.empty();
    }
    key = toLower(trim(std::string_view(raw).substr(0, bracket)));
    const usize close = raw.find(']', bracket);
    if (close == std::string::npos) {
        return false;
    }
    const std::string index = trim(std::string_view(raw).substr(bracket + 1, close - bracket - 1));
    if (index.empty() || !std::all_of(index.begin(), index.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        return false;
    }
    stage = static_cast<u32>(std::stoul(index));
    return !key.empty();
}

/// Statements of a `{ ... }` body (nested blocks skipped).
void parseBody(std::string_view body, T3DMaterialDef& m) {
    usize i = 0;
    while (i < body.size()) {
        while (i < body.size() && (std::isspace(static_cast<unsigned char>(body[i])) || body[i] == ';')) {
            ++i;
        }
        if (i >= body.size()) {
            break;
        }
        if (body[i] == '{') { // nested block: skip it
            int depth = 0;
            while (i < body.size()) {
                if (body[i] == '{') {
                    ++depth;
                } else if (body[i] == '}' && --depth == 0) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        const usize eq = body.find('=', i);
        const usize semi = body.find(';', i);
        if (eq == std::string_view::npos || (semi != std::string_view::npos && semi < eq)) {
            i = semi == std::string_view::npos ? body.size() : semi + 1;
            continue;
        }
        const std::string rawKey = trim(body.substr(i, eq - i));
        usize v = eq + 1;
        while (v < body.size() && std::isspace(static_cast<unsigned char>(body[v]))) {
            ++v;
        }
        std::string value;
        if (v < body.size() && (body[v] == '"' || body[v] == '\'')) {
            const char quote = body[v];
            ++v;
            while (v < body.size() && body[v] != quote) {
                if (body[v] == '\\' && v + 1 < body.size()) {
                    value.push_back(body[v + 1]);
                    v += 2;
                    continue;
                }
                value.push_back(body[v++]);
            }
            ++v;
            const usize end = body.find(';', v);
            i = end == std::string_view::npos ? body.size() : end + 1;
        } else {
            const usize end = body.find(';', v);
            value = trim(body.substr(v, (end == std::string_view::npos ? body.size() : end) - v));
            i = end == std::string_view::npos ? body.size() : end + 1;
        }
        std::string key;
        u32 stage = 0;
        if (!splitStageKey(rawKey, key, stage) || stage != 0u) {
            continue;
        }
        applyField(m, key, value);
    }
}

/// Parses `name="value"` attributes of a TAML element opening tag (the text between the tag name and '>').
std::vector<std::pair<std::string, std::string>> parseAttributes(std::string_view tag) {
    std::vector<std::pair<std::string, std::string>> out;
    usize i = 0;
    while (i < tag.size()) {
        while (i < tag.size() && !isIdentChar(tag[i])) {
            ++i;
        }
        const usize nameBegin = i;
        while (i < tag.size() && (isIdentChar(tag[i]) || tag[i] == '.')) {
            ++i;
        }
        const std::string name(tag.substr(nameBegin, i - nameBegin));
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }
        if (i >= tag.size() || tag[i] != '=') {
            continue;
        }
        ++i;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) {
            continue;
        }
        const char quote = tag[i++];
        const usize valueBegin = i;
        while (i < tag.size() && tag[i] != quote) {
            ++i;
        }
        out.emplace_back(name, std::string(tag.substr(valueBegin, i - valueBegin)));
        ++i;
    }
    return out;
}

/// Opening tag `<Name ...>` starting at or after `from` whose element name is exactly `name`.
/// Returns the attribute text and the position after '>' (npos when not found).
usize findElement(std::string_view text, std::string_view name, usize from, std::string_view& attributes) {
    usize pos = from;
    while ((pos = text.find('<', pos)) != std::string_view::npos) {
        const usize nameBegin = pos + 1;
        usize nameEnd = nameBegin;
        while (nameEnd < text.size() && (isIdentChar(text[nameEnd]) || text[nameEnd] == '.')) {
            ++nameEnd;
        }
        if (iequals(text.substr(nameBegin, nameEnd - nameBegin), name)) {
            const usize close = text.find('>', nameEnd);
            if (close == std::string_view::npos) {
                return std::string_view::npos;
            }
            usize attrEnd = close;
            if (attrEnd > nameEnd && text[attrEnd - 1] == '/') {
                --attrEnd;
            }
            attributes = text.substr(nameEnd, attrEnd - nameEnd);
            return close + 1;
        }
        pos = nameBegin;
    }
    return std::string_view::npos;
}

bool looksLikeAssetReference(const std::string& ref) {
    if (ref.rfind("@asset=", 0) == 0) {
        return true;
    }
    const usize colon = ref.find(':');
    if (colon == std::string::npos || colon == 0u) {
        return false;
    }
    // "C:/..." drive letters and "game:/..." mount schemes are paths.
    if (colon + 1 < ref.size() && (ref[colon + 1] == '/' || ref[colon + 1] == '\\')) {
        return false;
    }
    return ref.find('/') == std::string::npos && ref.find('\\') == std::string::npos;
}

const char* const kImageExtensions[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".ktx2"};

bool isRegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

/// `base` as given when it names a file, else with each known image extension.
std::string tryImage(const fs::path& base) {
    if (base.has_extension() && isRegularFile(base)) {
        return base.lexically_normal().generic_string();
    }
    for (const char* ext : kImageExtensions) {
        fs::path candidate = base;
        candidate += ext;
        if (isRegularFile(candidate)) {
            return candidate.lexically_normal().generic_string();
        }
    }
    return {};
}

/// ImageAsset lookup over `.asset.taml` files (AssetName, case-insensitive -> taml paths).
class ImageAssetIndex {
public:
    void build(const std::vector<std::string>& roots) {
        m_byName.clear();
        for (const std::string& root : roots) {
            std::error_code ec;
            if (root.empty() || !fs::is_directory(root, ec)) {
                continue;
            }
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const std::string file = it->path().filename().string();
                constexpr std::string_view kSuffix = ".asset.taml";
                if (file.size() <= kSuffix.size() ||
                    !iequals(std::string_view(file).substr(file.size() - kSuffix.size()), kSuffix)) {
                    continue;
                }
                m_byName[toLower(file.substr(0, file.size() - kSuffix.size()))].push_back(
                    it->path().lexically_normal().generic_string());
            }
        }
    }

    /// Image file of asset `Module:Name` ("" when unknown or not an ImageAsset).
    std::string resolve(const std::string& reference) const {
        const std::string ref = stripAssetPrefix(reference);
        const usize colon = ref.find(':');
        const std::string module = colon == std::string::npos ? std::string() : toLower(ref.substr(0, colon));
        const std::string name = toLower(colon == std::string::npos ? ref : ref.substr(colon + 1));
        const auto it = m_byName.find(name);
        if (it == m_byName.end()) {
            return {};
        }
        // Prefer a taml under a folder named like the module.
        std::vector<std::string> ordered = it->second;
        std::stable_sort(ordered.begin(), ordered.end(), [&](const std::string& a, const std::string& b) {
            const bool am = !module.empty() && toLower(a).find("/" + module + "/") != std::string::npos;
            const bool bm = !module.empty() && toLower(b).find("/" + module + "/") != std::string::npos;
            return am && !bm;
        });
        for (const std::string& taml : ordered) {
            std::string text;
            if (!readMaterialSourceFile(taml, text)) {
                continue;
            }
            std::string_view attributes;
            if (findElement(text, "ImageAsset", 0, attributes) == std::string_view::npos) {
                continue;
            }
            for (const auto& [key, value] : parseAttributes(attributes)) {
                if (!iequals(key, "imageFile")) {
                    continue;
                }
                std::string file = value;
                constexpr std::string_view kFilePrefix = "@assetFile=";
                if (file.size() >= kFilePrefix.size() && iequals(std::string_view(file).substr(0, kFilePrefix.size()), kFilePrefix)) {
                    file.erase(0, kFilePrefix.size());
                }
                const std::string found = tryImage(fs::path(taml).parent_path() / file);
                if (!found.empty()) {
                    return found;
                }
            }
        }
        return {};
    }

private:
    std::unordered_map<std::string, std::vector<std::string>> m_byName;
};

std::string resolveMap(const std::string& mapRef, bool assetReference, const std::string& definingFile,
                       const T3DMaterialCookOptions& options, const ImageAssetIndex& assets) {
    const std::string ref = trim(mapRef);
    if (ref.empty()) {
        return {};
    }
    if (assetReference || looksLikeAssetReference(ref)) {
        return assets.resolve(ref);
    }
    std::string relative = ref;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const bool sameFolder = relative.rfind("./", 0) == 0 || relative.rfind("~/", 0) == 0;
    if (sameFolder) {
        relative.erase(0, 2);
    }
    const fs::path asGiven(relative);
    if (asGiven.is_absolute()) {
        const std::string found = tryImage(asGiven);
        if (!found.empty()) {
            return found;
        }
    }
    if (!definingFile.empty()) {
        const std::string found = tryImage(fs::path(definingFile).parent_path() / relative);
        if (!found.empty()) {
            return found;
        }
    }
    if (!sameFolder && !options.gameRoot.empty()) {
        const std::string found = tryImage(fs::path(options.gameRoot) / relative);
        if (!found.empty()) {
            return found;
        }
    }
    if (options.useVfs) {
        fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
        const std::string virtualPaths[] = {remapLegacyAssetPath(relative), "/game/" + relative};
        for (const std::string& virtualPath : virtualPaths) {
            std::string physical;
            if (vfs.resolve(virtualPath, physical)) {
                const std::string found = tryImage(fs::path(physical));
                if (!found.empty()) {
                    return found;
                }
            }
            for (const char* ext : kImageExtensions) {
                if (vfs.resolve(virtualPath + ext, physical) && isRegularFile(physical)) {
                    return fs::path(physical).lexically_normal().generic_string();
                }
            }
        }
    }
    return {};
}

// --- .fusemat writer (layout of asset::read_cooked_material) --------------------------------------------------

void putU32(std::vector<u8>& out, u32 v) {
    for (u32 i = 0; i < 4u; ++i) {
        out.push_back(static_cast<u8>((v >> (8u * i)) & 0xFFu));
    }
}

void putF32(std::vector<u8>& out, f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(out, bits);
}

void putStr(std::vector<u8>& out, const std::string& s) {
    const usize n = std::min<usize>(s.size(), 0xFFFFu);
    out.push_back(static_cast<u8>(n & 0xFFu));
    out.push_back(static_cast<u8>((n >> 8) & 0xFFu));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

void putSet(std::vector<u8>& out, const asset::CookedMaterialTextures& t) {
    putStr(out, t.albedo);
    putStr(out, t.normal);
}

u64 fnv1a64(const u8* data, usize size) {
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

f32 clampf(f32 v, f32 lo, f32 hi) {
    return std::min(std::max(v, lo), hi);
}

/// sRGB-authored T3D colour -> linear.
f32 srgbToLinear(f32 c) {
    c = clampf(c, 0.f, 1.f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool ensureDirectory(const fs::path& dir) {
    if (dir.empty()) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec;
}

} // namespace

T3DMaterialParseResult parseT3DMaterialScript(std::string_view text, const std::string& sourcePath) {
    T3DMaterialParseResult result;
    const std::string clean = stripComments(text);
    const std::string_view s(clean);
    std::unordered_map<std::string, usize> byName; // lower-case name -> index in result.materials
    usize pos = 0;
    while (pos < s.size()) {
        // Next "singleton" / "new" keyword at a word boundary.
        usize kw = std::string_view::npos;
        usize kwLen = 0;
        for (usize i = pos; i < s.size(); ++i) {
            if (i > 0 && isIdentChar(s[i - 1])) {
                continue;
            }
            if (s.size() - i >= 9u && iequals(s.substr(i, 9), "singleton") && (i + 9 >= s.size() || !isIdentChar(s[i + 9]))) {
                kw = i;
                kwLen = 9;
                break;
            }
            if (s.size() - i >= 3u && iequals(s.substr(i, 3), "new") && (i + 3 >= s.size() || !isIdentChar(s[i + 3]))) {
                kw = i;
                kwLen = 3;
                break;
            }
        }
        if (kw == std::string_view::npos) {
            break;
        }
        usize i = kw + kwLen;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        const usize classBegin = i;
        while (i < s.size() && isIdentChar(s[i])) {
            ++i;
        }
        const std::string_view className = s.substr(classBegin, i - classBegin);
        pos = i;
        if (!iequals(className, "Material")) {
            continue;
        }
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        if (i >= s.size() || s[i] != '(') {
            continue;
        }
        ++result.blocksSeen;
        const usize close = s.find(')', i);
        const usize open = close == std::string_view::npos ? close : s.find('{', close);
        if (close == std::string_view::npos || open == std::string_view::npos) {
            break;
        }
        // Everything between ')' and '{' must be whitespace.
        if (trim(s.substr(close + 1, open - close - 1)).size() != 0u) {
            continue;
        }
        int depth = 0;
        usize end = open;
        for (; end < s.size(); ++end) {
            if (s[end] == '"' || s[end] == '\'') {
                const char quote = s[end++];
                while (end < s.size() && s[end] != quote) {
                    end += (s[end] == '\\') ? 2u : 1u;
                }
                continue;
            }
            if (s[end] == '{') {
                ++depth;
            } else if (s[end] == '}' && --depth == 0) {
                break;
            }
        }
        if (end >= s.size()) {
            break; // unterminated block
        }
        pos = end + 1;

        T3DMaterialDef def;
        const std::string header = trim(s.substr(i + 1, close - i - 1));
        const usize colon = header.find(':');
        def.name = trim(colon == std::string::npos ? std::string_view(header) : std::string_view(header).substr(0, colon));
        if (colon != std::string::npos) {
            def.parent = trim(std::string_view(header).substr(colon + 1));
            const auto parent = byName.find(toLower(def.parent));
            if (parent != byName.end()) {
                const std::string name = def.name;
                const std::string parentName = def.parent;
                def = result.materials[parent->second];
                def.name = name;
                def.parent = parentName;
                def.mapTo.clear();
            }
        }
        if (def.name.empty()) {
            continue; // anonymous materials cannot be referenced
        }
        def.sourcePath = sourcePath;
        parseBody(s.substr(open + 1, end - open - 1), def);
        byName[toLower(def.name)] = result.materials.size();
        result.materials.push_back(std::move(def));
    }
    result.note = "parsed " + std::to_string(result.materials.size()) + " material definition(s) from " +
                  (sourcePath.empty() ? std::string("script text") : sourcePath);
    return result;
}

T3DMaterialParseResult parseT3DMaterialAssetTaml(std::string_view text, const std::string& sourcePath) {
    T3DMaterialParseResult result;
    std::string assetName;
    std::string_view attributes;
    if (findElement(text, "MaterialAsset", 0, attributes) != std::string_view::npos) {
        for (const auto& [key, value] : parseAttributes(attributes)) {
            if (iequals(key, "materialDefinitionName") || (assetName.empty() && iequals(key, "AssetName"))) {
                assetName = value;
            }
        }
    }
    usize pos = 0;
    while ((pos = findElement(text, "Material", pos, attributes)) != std::string_view::npos) {
        ++result.blocksSeen;
        T3DMaterialDef def;
        def.sourcePath = sourcePath;
        for (const auto& [key, value] : parseAttributes(attributes)) {
            if (iequals(key, "Name")) {
                def.name = value;
            } else {
                std::string field;
                u32 stage = 0;
                if (splitStageKey(key, field, stage) && stage == 0u) {
                    applyField(def, field, value);
                }
            }
        }
        // Stage 0 = the first stage element before the Material closes.
        const usize materialEnd = text.find("</Material>", pos);
        std::string_view stage;
        const usize stagePos = findElement(text, "Stages_beginarray", pos, stage);
        if (stagePos != std::string_view::npos && (materialEnd == std::string_view::npos || stagePos < materialEnd)) {
            for (const auto& [key, value] : parseAttributes(stage)) {
                std::string field;
                u32 index = 0;
                if (splitStageKey(key, field, index) && index == 0u) {
                    applyField(def, field, value);
                }
            }
        }
        if (def.name.empty()) {
            def.name = assetName;
        }
        if (!def.name.empty()) {
            result.materials.push_back(std::move(def));
        }
        if (materialEnd == std::string_view::npos) {
            break;
        }
        pos = materialEnd + 1;
    }
    result.note = "parsed " + std::to_string(result.materials.size()) + " MaterialAsset definition(s)";
    return result;
}

T3DMaterialParseResult parseT3DMaterialFile(const std::string& path) {
    std::string text;
    if (!readMaterialSourceFile(path, text)) {
        T3DMaterialParseResult result;
        result.note = "unable to read " + path;
        return result;
    }
    const std::string lower = toLower(path);
    if (lower.size() >= 5u && lower.compare(lower.size() - 5u, 5u, ".taml") == 0) {
        return parseT3DMaterialAssetTaml(text, path);
    }
    return parseT3DMaterialScript(text, path);
}

std::string resolveT3DMaterialMapPath(const std::string& mapRef, bool assetReference, const std::string& definingFile,
                                      const T3DMaterialCookOptions& options) {
    ImageAssetIndex assets;
    if (assetReference || looksLikeAssetReference(trim(mapRef))) {
        assets.build(options.assetSearchRoots);
    }
    return resolveMap(mapRef, assetReference, definingFile, options, assets);
}

asset::CookedMaterial t3dMaterialToCooked(const T3DMaterialDef& material, const std::string& albedoCookId,
                                          const std::string& normalCookId) {
    asset::CookedMaterial out;
    out.name = material.name.substr(0, 255);
    if (out.name.empty()) {
        out.name = "material";
    }
    // FuseMatShading: 0 default_lit, 5 unlit (T3D emissive materials are drawn unlit).
    out.shading = material.emissive ? 5u : 0u;
    out.metallic = material.hasMetalness ? clampf(material.metalness, 0.f, 1.f) : 0.f;
    if (material.hasRoughness) {
        out.roughness = clampf(material.roughness, 0.f, 1.f);
    } else if (material.hasSpecularPower) {
        // Blinn-Phong exponent -> GGX roughness (alpha^2 = 2 / (n + 2), roughness = sqrt(alpha)).
        out.roughness = clampf(std::sqrt(std::sqrt(2.f / (std::max(material.specularPower, 0.f) + 2.f))), 0.f, 1.f);
    } else {
        out.roughness = 1.f;
    }
    const bool textured = !albedoCookId.empty();
    for (u32 c = 0; c < 3u; ++c) {
        const f32 linear = srgbToLinear(material.diffuseColor[c]);
        if (textured) {
            out.albedo[c] = clampf(linear, 0.f, 1.f); // tint over the texture
        } else if (out.metallic >= 0.5f) {
            out.albedo[c] = clampf(linear, 0.45f, 1.f);
        } else {
            out.albedo[c] = clampf(linear, 0.02f, 0.9f);
        }
    }
    out.textures.albedo = albedoCookId.substr(0, 255);
    out.textures.normal = normalCookId.substr(0, 255);
    return out;
}

std::vector<u8> encodeCookedMaterial(const asset::CookedMaterial& m) {
    std::vector<u8> payload;
    putStr(payload, m.name);
    putU32(payload, m.shading);
    putU32(payload, m.category);
    putU32(payload, m.wind);
    for (const f32 a : m.albedo) {
        putF32(payload, a);
    }
    putF32(payload, m.roughness);
    putF32(payload, m.metallic);
    putF32(payload, m.normal_strength);
    putSet(payload, m.textures);
    putF32(payload, m.uv_scale);
    putU32(payload, m.triplanar ? 1u : 0u);
    putF32(payload, m.triplanar_sharpness);
    putU32(payload, m.stochastic ? 1u : 0u);
    putF32(payload, m.stochastic_lattice);
    putF32(payload, m.macro_scale);
    putF32(payload, m.macro_strength);
    putSet(payload, m.detail);
    putF32(payload, m.detail_scale);
    putF32(payload, m.detail_strength);
    putF32(payload, m.detail_fade[0]);
    putF32(payload, m.detail_fade[1]);
    putU32(payload, static_cast<u32>(m.layers.size()));
    for (const asset::CookedMaterialLayer& l : m.layers) {
        putStr(payload, l.name);
        putU32(payload, l.mode);
        putU32(payload, l.mask);
        putF32(payload, l.mask_bias);
        putF32(payload, l.mask_scale);
        putF32(payload, l.coverage);
        putF32(payload, l.contrast);
        for (const f32 a : l.albedo) {
            putF32(payload, a);
        }
        putF32(payload, l.roughness);
        putF32(payload, l.metallic);
        putF32(payload, l.uv_scale);
        putF32(payload, l.normal_strength);
        putSet(payload, l.textures);
    }
    putU32(payload, m.procedural_function);
    putU32(payload, static_cast<u32>(m.procedural_params.size()));
    for (const f32 p : m.procedural_params) {
        putF32(payload, p);
    }

    std::vector<u8> out;
    out.reserve(payload.size() + 20u);
    putU32(out, asset::kCookedMaterialMagic);
    putU32(out, asset::kCookedMaterialVersion);
    putU32(out, static_cast<u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    const u64 trailer = fnv1a64(out.data(), out.size());
    for (u32 i = 0; i < 8u; ++i) {
        out.push_back(static_cast<u8>((trailer >> (8u * i)) & 0xFFu));
    }
    return out;
}

bool writeCookedMaterialFile(const asset::CookedMaterial& material, const std::string& path, std::string* error) {
    if (!asset::validate_cooked_material(material, error)) {
        return false;
    }
    const std::vector<u8> bytes = encodeCookedMaterial(material);
    asset::CookedMaterial check;
    if (!asset::read_cooked_material(bytes.data(), bytes.size(), check, error) || !(check == material)) {
        if (error != nullptr && error->empty()) {
            *error = "encoded .fusemat does not read back identically";
        }
        return false;
    }
    if (!ensureDirectory(fs::path(path).parent_path())) {
        if (error != nullptr) {
            *error = "unable to create directory for " + path;
        }
        return false;
    }
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error != nullptr) {
                *error = "unable to write " + path;
            }
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            if (error != nullptr) {
                *error = "short write " + path;
            }
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
        if (ec) {
            fs::remove(temp, ec);
            if (error != nullptr) {
                *error = "unable to replace " + path;
            }
            return false;
        }
    }
    return true;
}

T3DMaterialCookResult cookT3DMaterials(const std::vector<T3DMaterialDef>& materials, const T3DMaterialCookOptions& options) {
    T3DMaterialCookResult result;
    result.materialCount = static_cast<u32>(materials.size());
    const fs::path outDir = options.outputDir.empty() ? fs::path(".") : fs::path(options.outputDir);
    const fs::path texDir = outDir / "textures";

    ImageAssetIndex assets;
    assets.build(options.assetSearchRoots);

    AssetCooker cooker;
    cooker.set_import_validation(ImportValidation::Strict);
    // (source, normal?) -> cook id ("" = the strict cook refused it).
    std::unordered_map<std::string, std::string> cooked;

    auto cookSlot = [&](const std::string& reference, bool assetReference, bool normal, const std::string& definingFile,
                        std::string& sourceOut, std::string& note) -> std::string {
        if (trim(reference).empty()) {
            return {};
        }
        ++result.mapsReferenced;
        const std::string source = resolveMap(reference, assetReference, definingFile, options, assets);
        if (source.empty()) {
            ++result.mapsUnresolved;
            note += (normal ? "normal" : "albedo") + std::string(" map '") + reference + "' unresolved; ";
            return {};
        }
        ++result.mapsResolved;
        sourceOut = source;
        const std::string key = (normal ? "n|" : "a|") + source;
        const auto hit = cooked.find(key);
        if (hit != cooked.end()) {
            if (!hit->second.empty()) {
                ++result.textureReuses;
            }
            return hit->second;
        }
        const std::string cookId = fs::path(source).stem().string() + (normal ? "_n" : "");
        TextureImportDesc desc;
        desc.input_path = source;
        desc.output_path = (texDir / (cookId + ".fusetex")).generic_string();
        desc.generate_mipmaps = true;
        desc.is_normal_map = normal;
        desc.color_space = normal ? TextureImportDesc::ColorSpace::Linear : TextureImportDesc::ColorSpace::sRGB;
        desc.compression = normal ? TextureImportDesc::Compression::BC5 : TextureImportDesc::Compression::BC7;
        std::string id;
        if (!ensureDirectory(texDir)) {
            note += "cannot create " + texDir.generic_string() + "; ";
            ++result.textureCookFailures;
        } else {
            const CookRecord record = cooker.cook_texture(desc);
            if (record.ok) {
                id = cookId;
                ++result.texturesCooked;
            } else {
                ++result.textureCookFailures;
                note += "strict cook refused " + source + " (" + record.note + "); ";
            }
        }
        cooked.emplace(key, id);
        return id;
    };

    for (const T3DMaterialDef& material : materials) {
        T3DCookedMaterialEntry entry;
        entry.name = material.name;
        entry.mapTo = material.mapTo;
        const bool albedoAsset = !material.diffuseMapAsset.empty();
        entry.albedoCookId = cookSlot(albedoAsset ? material.diffuseMapAsset : material.diffuseMap, albedoAsset, false,
                                      material.sourcePath, entry.albedoSource, entry.note);
        const bool normalAsset = !material.normalMapAsset.empty();
        entry.normalCookId = cookSlot(normalAsset ? material.normalMapAsset : material.normalMap, normalAsset, true,
                                      material.sourcePath, entry.normalSource, entry.note);
        entry.fusematPath = (outDir / (material.name + ".fusemat")).generic_string();
        std::string error;
        entry.written = writeCookedMaterialFile(t3dMaterialToCooked(material, entry.albedoCookId, entry.normalCookId),
                                                entry.fusematPath, &error);
        if (entry.written) {
            ++result.fusematWritten;
        } else {
            entry.note += "fusemat not written: " + error;
        }
        result.entries.push_back(std::move(entry));
    }
    result.note = "cooked " + std::to_string(result.fusematWritten) + "/" + std::to_string(result.materialCount) +
                  " material(s), " + std::to_string(result.texturesCooked) + " texture(s) (" +
                  std::to_string(result.mapsResolved) + "/" + std::to_string(result.mapsReferenced) + " maps resolved, " +
                  std::to_string(result.textureCookFailures) + " refused)";
    fuse::log::info("cookT3DMaterials: %s", result.note.c_str());
    return result;
}

T3DMaterialCookResult cookT3DMaterialFile(const std::string& path, const T3DMaterialCookOptions& options) {
    const T3DMaterialParseResult parsed = parseT3DMaterialFile(path);
    T3DMaterialCookResult result = cookT3DMaterials(parsed.materials, options);
    if (parsed.materials.empty()) {
        result.note = parsed.note + "; " + result.note;
    }
    return result;
}

} // namespace fuse::project
