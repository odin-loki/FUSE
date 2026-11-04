#include <fuse/project/manifest.hpp>

#include <cstdio>
#include <iterator>

namespace fuse::project {

hybrid::DimensionFlags toDimensionFlags(const DimensionSettings& settings) {
    hybrid::DimensionFlags flags;
    flags.enable3D = settings.enable3D;
    flags.enable2D = settings.enable2D;
    flags.enableUI = settings.enableUI;
    return flags;
}

std::string escapeJsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2u);
    for (const char ch : text) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20u) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
            } else {
                out += ch; // UTF-8 bytes pass through unchanged
            }
            break;
        }
    }
    return out;
}

namespace {

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

void appendUtf8(std::string& out, u32 codePoint) {
    if (codePoint < 0x80u) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800u) {
        out += static_cast<char>(0xC0u | (codePoint >> 6));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else if (codePoint < 0x10000u) {
        out += static_cast<char>(0xE0u | (codePoint >> 12));
        out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (codePoint >> 18));
        out += static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    }
}

bool readHex4(std::string_view text, usize at, u32& out) {
    if (at + 4u > text.size()) {
        return false;
    }
    u32 value = 0;
    for (usize i = 0; i < 4u; ++i) {
        const int digit = hexValue(text[at + i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<u32>(digit);
    }
    out = value;
    return true;
}

} // namespace

std::string unescapeJsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch != '\\' || i + 1u >= text.size()) {
            out += ch;
            continue;
        }
        const char esc = text[++i];
        switch (esc) {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            u32 codePoint = 0;
            if (!readHex4(text, i + 1u, codePoint)) {
                out += "\\u";
                break;
            }
            i += 4u;
            // Surrogate pair (\\uD83D\\uDE00) -> one code point.
            if (codePoint >= 0xD800u && codePoint <= 0xDBFFu && i + 6u < text.size() &&
                text[i + 1u] == '\\' && text[i + 2u] == 'u') {
                u32 low = 0;
                if (readHex4(text, i + 3u, low) && low >= 0xDC00u && low <= 0xDFFFu) {
                    codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) + (low - 0xDC00u);
                    i += 6u;
                }
            }
            appendUtf8(out, codePoint);
            break;
        }
        default:
            out += '\\';
            out += esc;
            break;
        }
    }
    return out;
}

namespace {

constexpr std::string_view kKindNames[] = {"game", "app", "hybrid"};
constexpr std::string_view kFeatureNames[] = {"AppPresent", "GameFrame", "Renderer3D", "Renderer2D", "Physics",
                                              "Audio",      "Script",    "Jobs",       "Viewport"};
constexpr std::string_view kRedrawNames[] = {"onDemand", "continuous"};
constexpr std::string_view kUiProfileNames[] = {"hud", "app", "editor"};
constexpr std::string_view kTextProfileNames[] = {"minimal", "full", "cooked"};
static_assert(std::size(kFeatureNames) == static_cast<usize>(HostFeature::Count));

template <class E, usize N> bool parseName(const std::string_view (&names)[N], std::string_view text, E& out) {
    for (usize i = 0; i < N; ++i) {
        if (names[i] == text) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

std::string quoted(std::string_view text) {
    return "\"" + escapeJsonString(text) + "\"";
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// Project-relative path: not absolute, no drive, no ".." segment.
bool isProjectRelative(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find(':') != std::string_view::npos) {
        return false;
    }
    usize start = 0;
    while (start <= path.size()) {
        usize end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        if (path.substr(start, end - start) == "..") {
            return false;
        }
        start = end + 1u;
    }
    return true;
}

std::string checkDocPath(std::string_view field, std::string_view path, std::string_view extension) {
    if (!isProjectRelative(path)) {
        return std::string(field) + ": \"" + std::string(path) + "\" must be a path inside the project";
    }
    if (!endsWith(path, extension)) {
        return std::string(field) + ": \"" + std::string(path) + "\" must end with " + std::string(extension);
    }
    return {};
}

bool validWindowId(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    for (const char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                        ch == '-' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

constexpr HostFeatureSet kGameSideFeatures = hostFeatureBit(HostFeature::GameFrame) |
                                             hostFeatureBit(HostFeature::Renderer3D) |
                                             hostFeatureBit(HostFeature::Renderer2D) |
                                             hostFeatureBit(HostFeature::Physics);

} // namespace

std::string_view toString(ProjectKind kind) {
    return kKindNames[static_cast<usize>(kind)];
}
std::string_view toString(HostFeature feature) {
    return feature < HostFeature::Count ? kFeatureNames[static_cast<usize>(feature)] : std::string_view("?");
}
std::string_view toString(RedrawMode mode) {
    return kRedrawNames[static_cast<usize>(mode)];
}
std::string_view toString(UiProfile profile) {
    return kUiProfileNames[static_cast<usize>(profile)];
}
std::string_view toString(TextProfile profile) {
    return kTextProfileNames[static_cast<usize>(profile)];
}
bool parseProjectKind(std::string_view text, ProjectKind& out) {
    return parseName(kKindNames, text, out);
}
bool parseHostFeature(std::string_view text, HostFeature& out) {
    return parseName(kFeatureNames, text, out);
}
bool parseRedrawMode(std::string_view text, RedrawMode& out) {
    return parseName(kRedrawNames, text, out);
}
bool parseUiProfile(std::string_view text, UiProfile& out) {
    return parseName(kUiProfileNames, text, out);
}
bool parseTextProfile(std::string_view text, TextProfile& out) {
    return parseName(kTextProfileNames, text, out);
}

std::string hostFeatureList(HostFeatureSet features) {
    std::string out;
    for (u32 i = 0; i < static_cast<u32>(HostFeature::Count); ++i) {
        if ((features & (1u << i)) != 0u) {
            if (!out.empty()) {
                out += ',';
            }
            out += kFeatureNames[i];
        }
    }
    return out;
}

bool needsSchema2(const ProjectManifest& manifest) {
    const UiSettings defaults;
    return manifest.kind != ProjectKind::Game || !manifest.windows.empty() || manifest.requiredFeatures != 0u ||
           !manifest.ui.root.empty() || !manifest.ui.styles.empty() || manifest.ui.theme != defaults.theme ||
           manifest.ui.redraw != defaults.redraw || manifest.ui.profile != defaults.profile ||
           manifest.ui.text != defaults.text;
}

bool migrateToLatest(ProjectManifest& manifest) {
    if (manifest.schemaVersion == kProjectSchemaVersion) {
        return false;
    }
    manifest.schemaVersion = kProjectSchemaVersion;
    return true;
}

HostFeatureSet impliedHostFeatures(const ProjectManifest& manifest) {
    switch (manifest.kind) {
    case ProjectKind::App:
        return hostFeatureBit(HostFeature::AppPresent);
    case ProjectKind::Hybrid:
        return hostFeatureBit(HostFeature::AppPresent) | hostFeatureBit(HostFeature::Viewport);
    case ProjectKind::Game:
        break;
    }
    HostFeatureSet set = hostFeatureBit(HostFeature::GameFrame) | hostFeatureBit(HostFeature::Jobs);
    if (manifest.dimensions.enable3D) {
        set |= hostFeatureBit(HostFeature::Renderer3D) | hostFeatureBit(HostFeature::Physics);
    }
    if (manifest.dimensions.enable2D) {
        set |= hostFeatureBit(HostFeature::Renderer2D);
    }
    return set;
}

HostFeatureSet effectiveHostFeatures(const ProjectManifest& manifest) {
    return impliedHostFeatures(manifest) | manifest.requiredFeatures;
}

InitPlan planInit(const ProjectManifest& manifest) {
    InitPlan plan;
    const HostFeatureSet all = effectiveHostFeatures(manifest);
    if (manifest.kind == ProjectKind::Game) {
        plan.startup = all;
        return plan;
    }
    plan.appShell = true;
    if (manifest.kind == ProjectKind::Hybrid) {
        // the game frame, worlds, renderer and physics wait for the first viewport widget
        HostFeatureSet game = hostFeatureBit(HostFeature::GameFrame) | (all & kGameSideFeatures);
        if (manifest.dimensions.enable3D) {
            game |= hostFeatureBit(HostFeature::Renderer3D) | hostFeatureBit(HostFeature::Physics);
        }
        if (manifest.dimensions.enable2D) {
            game |= hostFeatureBit(HostFeature::Renderer2D);
        }
        plan.deferred = game;
        plan.startup = all & ~kGameSideFeatures;
        return plan;
    }
    plan.startup = all;
    return plan;
}

std::string validateManifest(const ProjectManifest& manifest) {
    if (manifest.name.empty()) {
        return "name: must not be empty";
    }
    for (u32 i = static_cast<u32>(HostFeature::Count); i < 32u; ++i) {
        if ((manifest.requiredFeatures & (1u << i)) != 0u) {
            return "requiredFeatures: unknown feature bit " + std::to_string(i);
        }
    }
    const bool appLike = manifest.kind != ProjectKind::Game;
    if (!appLike) {
        if ((manifest.requiredFeatures &
             (hostFeatureBit(HostFeature::AppPresent) | hostFeatureBit(HostFeature::Viewport))) != 0u) {
            return "requiredFeatures: AppPresent / Viewport need kind \"app\" or \"hybrid\"";
        }
        if (!manifest.windows.empty()) {
            return "windows: only app and hybrid projects declare windows";
        }
    }
    if (manifest.kind == ProjectKind::App) {
        if (manifest.dimensions.enable3D || manifest.dimensions.enable2D || !manifest.defaultWorld3D.empty() ||
            !manifest.defaultWorld2D.empty()) {
            return "kind: an \"app\" project has no 3D / 2D worlds (use kind \"hybrid\")";
        }
        if ((manifest.requiredFeatures & (kGameSideFeatures | hostFeatureBit(HostFeature::Viewport))) != 0u) {
            return "requiredFeatures: " + hostFeatureList(manifest.requiredFeatures & (kGameSideFeatures |
                                                                                     hostFeatureBit(HostFeature::Viewport))) +
                   " need kind \"hybrid\"";
        }
    }
    if (appLike) {
        if (!manifest.dimensions.enableUI) {
            return "dimensions.enableUI: app and hybrid projects need the UI layer";
        }
        bool everyWindowHasRoot = !manifest.windows.empty();
        for (const WindowSettings& window : manifest.windows) {
            everyWindowHasRoot = everyWindowHasRoot && !window.root.empty();
        }
        if (manifest.ui.root.empty() && !everyWindowHasRoot) {
            return "ui.root: app and hybrid projects need a main .fuseui (ui.root or a root per window)";
        }
    }
    if (!manifest.ui.root.empty()) {
        if (std::string e = checkDocPath("ui.root", manifest.ui.root, ".fuseui"); !e.empty()) {
            return e;
        }
    }
    for (usize i = 0; i < manifest.ui.styles.size(); ++i) {
        const std::string field = "ui.styles[" + std::to_string(i) + "]";
        if (std::string e = checkDocPath(field, manifest.ui.styles[i], ".fusestyle"); !e.empty()) {
            return e;
        }
    }
    if (manifest.ui.theme.empty()) {
        return "ui.theme: must not be empty";
    }
    for (usize i = 0; i < manifest.windows.size(); ++i) {
        const WindowSettings& window = manifest.windows[i];
        const std::string field = "windows[" + std::to_string(i) + "]";
        if (!validWindowId(window.id)) {
            return field + ".id: \"" + window.id + "\" must be non-empty [A-Za-z0-9_.-]";
        }
        for (usize j = 0; j < i; ++j) {
            if (manifest.windows[j].id == window.id) {
                return field + ".id: duplicate window id \"" + window.id + "\"";
            }
        }
        if (window.width == 0u || window.height == 0u || window.width > 16384u || window.height > 16384u) {
            return field + ": width / height must be 1..16384";
        }
        if (!window.root.empty()) {
            if (std::string e = checkDocPath(field + ".root", window.root, ".fuseui"); !e.empty()) {
                return e;
            }
        }
    }
    return {};
}

ProjectManifest appTemplateManifest(std::string_view name) {
    ProjectManifest manifest;
    manifest.schemaVersion = kProjectSchemaVersion;
    manifest.name = name.empty() ? std::string("Untitled") : std::string(name);
    manifest.kind = ProjectKind::App;
    manifest.dimensions = {false, false, true};
    manifest.ui.root = "ui/main.fuseui";
    WindowSettings window;
    window.title = manifest.name;
    manifest.windows.push_back(window);
    return manifest;
}

std::string_view appTemplateMainUi() {
    return R"({
  "fuseui": 1,
  "style": ".main { gap: 10px; padding: 24px; align-items: start; } .title { font-size: 24px; }",
  "root": {
    "class": "panel", "id": "main", "classes": ["main"],
    "children": [
      { "class": "label", "id": "title", "classes": ["title"], "text": "Hello from FUSE UI" },
      { "class": "label", "id": "hint", "text": "Edit ui/main.fuseui to build your app." },
      { "class": "button", "id": "action", "text": "Click me", "on": { "clicked": "action" } }
    ]
  }
}
)";
}

std::string writeManifestJson(const ProjectManifest& manifest) {
    const auto boolText = [](bool value) { return value ? "true" : "false"; };
    const bool schema2 = manifest.schemaVersion != 1u || needsSchema2(manifest);
    std::string json;
    json.reserve(1024);
    json += "{\n";
    json += "  \"schemaVersion\": " + std::to_string(schema2 ? kProjectSchemaVersion : 1u) + ",\n";
    json += "  \"name\": \"" + escapeJsonString(manifest.name) + "\",\n";
    if (schema2) {
        json += "  \"kind\": " + quoted(toString(manifest.kind)) + ",\n";
    }
    json += "  \"dimensions\": {\n";
    json += std::string("    \"enable3D\": ") + boolText(manifest.dimensions.enable3D) + ",\n";
    json += std::string("    \"enable2D\": ") + boolText(manifest.dimensions.enable2D) + ",\n";
    json += std::string("    \"enableUI\": ") + boolText(manifest.dimensions.enableUI) + "\n";
    json += "  },\n";
    json += "  \"modules\": {\n";
    json += std::string("    \"ai\": ") + boolText(manifest.modules.ai) + ",\n";
    json += std::string("    \"cinematics\": ") + boolText(manifest.modules.cinematics) + ",\n";
    json += std::string("    \"fx\": ") + boolText(manifest.modules.fx) + ",\n";
    json += std::string("    \"mechanics\": ") + boolText(manifest.modules.mechanics) + ",\n";
    json += std::string("    \"adventure\": ") + boolText(manifest.modules.adventure) + "\n";
    json += "  },\n";
    json += "  \"defaultWorld3D\": \"" + escapeJsonString(manifest.defaultWorld3D) + "\",\n";
    json += "  \"defaultWorld2D\": \"" + escapeJsonString(manifest.defaultWorld2D) + "\",\n";
    if (!schema2) {
        json += "  \"workerCap\": " + std::to_string(manifest.workerCap) + "\n";
        json += "}\n";
        return json;
    }
    json += "  \"workerCap\": " + std::to_string(manifest.workerCap) + ",\n";
    json += "  \"requiredFeatures\": [";
    bool first = true;
    for (u32 i = 0; i < static_cast<u32>(HostFeature::Count); ++i) {
        if ((manifest.requiredFeatures & (1u << i)) != 0u) {
            json += first ? "" : ", ";
            json += quoted(kFeatureNames[i]);
            first = false;
        }
    }
    json += "],\n";
    json += "  \"ui\": {\n";
    json += "    \"root\": " + quoted(manifest.ui.root) + ",\n";
    json += "    \"styles\": [";
    for (usize i = 0; i < manifest.ui.styles.size(); ++i) {
        json += (i ? ", " : "") + quoted(manifest.ui.styles[i]);
    }
    json += "],\n";
    json += "    \"theme\": " + quoted(manifest.ui.theme) + ",\n";
    json += "    \"redraw\": " + quoted(toString(manifest.ui.redraw)) + ",\n";
    json += "    \"profile\": " + quoted(toString(manifest.ui.profile)) + ",\n";
    json += "    \"text\": " + quoted(toString(manifest.ui.text)) + "\n";
    json += "  },\n";
    json += "  \"windows\": [";
    for (usize i = 0; i < manifest.windows.size(); ++i) {
        const WindowSettings& window = manifest.windows[i];
        json += i ? ",\n    {" : "\n    {";
        json += "\"id\": " + quoted(window.id) + ", \"title\": " + quoted(window.title) +
                ", \"width\": " + std::to_string(window.width) + ", \"height\": " + std::to_string(window.height) +
                ", \"root\": " + quoted(window.root) + ", \"redraw\": " + quoted(toString(window.redraw)) + "}";
    }
    json += manifest.windows.empty() ? "]\n" : "\n  ]\n";
    json += "}\n";
    return json;
}

} // namespace fuse::project
