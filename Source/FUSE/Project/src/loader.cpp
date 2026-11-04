#include <fuse/project/loader.hpp>

#include <fuse/project/json_reader.hpp>
#include <fuse/project/json_value.hpp>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <system_error>
#include <utility>

namespace fuse::project {

namespace {

std::string readFileToString(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// Schema 2 reader over the JSON document: every key is typed and checked; unknown keys are errors except "x-*"
/// extension keys (ignored, not written back).
class Schema2Reader {
public:
    explicit Schema2Reader(ProjectManifest& out) : m_out(out) {}
    const std::string& error() const { return m_error; }

    bool read(const json::Value& root) {
        if (!root.isObject()) {
            return fail("$", "expected an object");
        }
        for (const auto& [key, value] : root.object) {
            const std::string path = "$." + key;
            bool ok = true;
            if (key == "schemaVersion") {
                ok = value.isNumber();
            } else if (key == "name") {
                ok = string(path, value, m_out.name);
            } else if (key == "kind") {
                ok = enumValue(path, value, m_out.kind, parseProjectKind, "game, app, hybrid");
            } else if (key == "dimensions") {
                ok = members(path, value, {"enable3D", "enable2D", "enableUI"},
                             {&m_out.dimensions.enable3D, &m_out.dimensions.enable2D, &m_out.dimensions.enableUI});
            } else if (key == "modules") {
                ok = members(path, value, {"ai", "cinematics", "fx", "mechanics", "adventure"},
                             {&m_out.modules.ai, &m_out.modules.cinematics, &m_out.modules.fx,
                              &m_out.modules.mechanics, &m_out.modules.adventure});
            } else if (key == "defaultWorld3D") {
                ok = string(path, value, m_out.defaultWorld3D);
            } else if (key == "defaultWorld2D") {
                ok = string(path, value, m_out.defaultWorld2D);
            } else if (key == "workerCap") {
                ok = count(path, value, m_out.workerCap, 1024u);
            } else if (key == "requiredFeatures") {
                ok = features(path, value);
            } else if (key == "ui") {
                ok = ui(path, value);
            } else if (key == "windows") {
                ok = windows(path, value);
            } else if (key.rfind("x-", 0) != 0) {
                return fail(path, "unknown key");
            }
            if (!ok) {
                return false;
            }
        }
        if (root.get("name") == nullptr) {
            return fail("$.name", "required");
        }
        return true;
    }

private:
    ProjectManifest& m_out;
    std::string m_error;

    bool fail(const std::string& path, std::string_view message) {
        if (m_error.empty()) {
            m_error = path + ": " + std::string(message);
        }
        return false;
    }

    bool string(const std::string& path, const json::Value& value, std::string& out) {
        if (!value.isString()) {
            return fail(path, "expected a string");
        }
        out = value.string;
        return true;
    }

    bool count(const std::string& path, const json::Value& value, u32& out, u32 max) {
        if (!value.isNumber() || value.number < 0.0 || value.number > double(max) ||
            value.number != static_cast<double>(static_cast<u32>(value.number))) {
            return fail(path, "expected an integer 0.." + std::to_string(max));
        }
        out = static_cast<u32>(value.number);
        return true;
    }

    template <class E>
    bool enumValue(const std::string& path, const json::Value& value, E& out, bool (*parse)(std::string_view, E&),
                   std::string_view allowed) {
        if (!value.isString() || !parse(value.string, out)) {
            return fail(path, "expected one of " + std::string(allowed));
        }
        return true;
    }

    bool members(const std::string& path, const json::Value& value, std::initializer_list<std::string_view> keys,
                 std::initializer_list<bool*> targets) {
        if (!value.isObject()) {
            return fail(path, "expected an object");
        }
        for (const auto& [key, member] : value.object) {
            usize index = 0;
            while (index < keys.size() && keys.begin()[index] != key) {
                ++index;
            }
            if (index == keys.size()) {
                if (key.rfind("x-", 0) == 0) {
                    continue;
                }
                return fail(path + "." + key, "unknown key");
            }
            if (!member.isBool()) {
                return fail(path + "." + key, "expected true or false");
            }
            *targets.begin()[index] = member.boolean;
        }
        return true;
    }

    bool features(const std::string& path, const json::Value& value) {
        if (!value.isArray()) {
            return fail(path, "expected an array of feature names");
        }
        for (usize i = 0; i < value.array.size(); ++i) {
            HostFeature feature{};
            const json::Value& item = value.array[i];
            if (!item.isString() || !parseHostFeature(item.string, feature)) {
                return fail(path + "[" + std::to_string(i) + "]",
                            "unknown host feature (AppPresent, GameFrame, Renderer3D, Renderer2D, Physics, Audio, "
                            "Script, Jobs, Viewport)");
            }
            m_out.requiredFeatures |= hostFeatureBit(feature);
        }
        return true;
    }

    bool ui(const std::string& path, const json::Value& value) {
        if (!value.isObject()) {
            return fail(path, "expected an object");
        }
        UiSettings& out = m_out.ui;
        for (const auto& [key, member] : value.object) {
            const std::string at = path + "." + key;
            bool ok = true;
            if (key == "root") {
                ok = string(at, member, out.root);
            } else if (key == "styles") {
                if (!member.isArray()) {
                    return fail(at, "expected an array of paths");
                }
                for (usize i = 0; i < member.array.size() && ok; ++i) {
                    out.styles.emplace_back();
                    ok = string(at + "[" + std::to_string(i) + "]", member.array[i], out.styles.back());
                }
            } else if (key == "theme") {
                ok = string(at, member, out.theme);
            } else if (key == "redraw") {
                ok = enumValue(at, member, out.redraw, parseRedrawMode, "onDemand, continuous");
            } else if (key == "profile") {
                ok = enumValue(at, member, out.profile, parseUiProfile, "hud, app, editor");
            } else if (key == "text") {
                ok = enumValue(at, member, out.text, parseTextProfile, "minimal, full, cooked");
            } else if (key.rfind("x-", 0) != 0) {
                return fail(at, "unknown key");
            }
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    bool windows(const std::string& path, const json::Value& value) {
        if (!value.isArray()) {
            return fail(path, "expected an array of windows");
        }
        for (usize i = 0; i < value.array.size(); ++i) {
            const std::string at = path + "[" + std::to_string(i) + "]";
            const json::Value& item = value.array[i];
            if (!item.isObject()) {
                return fail(at, "expected an object");
            }
            WindowSettings window;
            for (const auto& [key, member] : item.object) {
                const std::string field = at + "." + key;
                bool ok = true;
                if (key == "id") {
                    ok = string(field, member, window.id);
                } else if (key == "title") {
                    ok = string(field, member, window.title);
                } else if (key == "width") {
                    ok = count(field, member, window.width, 16384u);
                } else if (key == "height") {
                    ok = count(field, member, window.height, 16384u);
                } else if (key == "root") {
                    ok = string(field, member, window.root);
                } else if (key == "redraw") {
                    ok = enumValue(field, member, window.redraw, parseRedrawMode, "onDemand, continuous");
                } else if (key.rfind("x-", 0) != 0) {
                    return fail(field, "unknown key");
                }
                if (!ok) {
                    return false;
                }
            }
            m_out.windows.push_back(std::move(window));
        }
        return true;
    }
};

LoadResult parseSchema2(std::string_view jsonText, LoadResult result) {
    auto document = json::parse(jsonText);
    if (!document) {
        result.status = LoadStatus::ParseError;
        result.error = "line " + std::to_string(document.error().line) + ", column " +
                       std::to_string(document.error().column) + ": " + document.error().message;
        return result;
    }
    Schema2Reader reader(result.manifest);
    if (!reader.read(*document)) {
        result.status = LoadStatus::Invalid;
        result.error = reader.error();
        return result;
    }
    if (std::string invalid = validateManifest(result.manifest); !invalid.empty()) {
        result.status = LoadStatus::Invalid;
        result.error = "$." + invalid;
        return result;
    }
    result.status = LoadStatus::Ok;
    return result;
}

} // namespace

LoadResult parseManifest(std::string_view jsonText, const std::string& projectRoot) {
    LoadResult result;
    result.manifest.projectRoot = projectRoot;

    json::Reader reader(jsonText);
    if (!reader.readU32("schemaVersion", result.manifest.schemaVersion)) {
        result.status = LoadStatus::ParseError;
        result.error = reader.error();
        return result;
    }

    if (result.manifest.schemaVersion == 2u) {
        return parseSchema2(jsonText, std::move(result));
    }
    if (result.manifest.schemaVersion != 1u) {
        result.status = LoadStatus::UnsupportedSchema;
        result.error = "unsupported schemaVersion " + std::to_string(result.manifest.schemaVersion) +
                       " (expected 1 or 2)";
        return result;
    }
    // Schema 1: read exactly as before; the schema 2 fields keep their defaults (kind game, no app UI).

    if (!reader.readString("name", result.manifest.name)) {
        result.status = LoadStatus::ParseError;
        result.error = "missing required field: name";
        return result;
    }
    result.manifest.name = unescapeJsonString(result.manifest.name);

    reader.readBool("dimensions", "enable3D", result.manifest.dimensions.enable3D);
    reader.readBool("dimensions", "enable2D", result.manifest.dimensions.enable2D);
    reader.readBool("dimensions", "enableUI", result.manifest.dimensions.enableUI);

    reader.readBool("modules", "ai", result.manifest.modules.ai);
    reader.readBool("modules", "cinematics", result.manifest.modules.cinematics);
    reader.readBool("modules", "fx", result.manifest.modules.fx);
    reader.readBool("modules", "mechanics", result.manifest.modules.mechanics);
    reader.readBool("modules", "adventure", result.manifest.modules.adventure);

    reader.readString("defaultWorld3D", result.manifest.defaultWorld3D);
    reader.readString("defaultWorld2D", result.manifest.defaultWorld2D);
    result.manifest.defaultWorld3D = unescapeJsonString(result.manifest.defaultWorld3D);
    result.manifest.defaultWorld2D = unescapeJsonString(result.manifest.defaultWorld2D);
    reader.readU32("workerCap", result.manifest.workerCap);

    result.status = LoadStatus::Ok;
    return result;
}

LoadResult loadFromFile(const std::string& projectJsonPath) {
    LoadResult result;
    const std::string jsonText = readFileToString(projectJsonPath);
    if (jsonText.empty()) {
        result.status = LoadStatus::FileNotFound;
        result.error = "unable to read project.json at: " + projectJsonPath;
        return result;
    }

    std::string projectRoot = projectJsonPath;
    const std::size_t slash = projectRoot.find_last_of("/\\");
    if (slash != std::string::npos) {
        projectRoot = projectRoot.substr(0, slash);
    } else {
        projectRoot.clear();
    }

    return parseManifest(jsonText, projectRoot);
}

LoadResult loadFromDirectory(const std::string& projectDirectory) {
    std::string jsonPath = projectDirectory;
    if (!jsonPath.empty() && jsonPath.back() != '/' && jsonPath.back() != '\\') {
        jsonPath.push_back('/');
    }
    jsonPath += "project.json";
    return loadFromFile(jsonPath);
}

SaveResult saveToFile(const ProjectManifest& manifest, const std::string& projectJsonPath) {
    SaveResult result;
    result.path = projectJsonPath;
    if (projectJsonPath.empty()) {
        result.error = "empty project.json path";
        return result;
    }

    const std::filesystem::path target(projectJsonPath);
    std::error_code ec;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            result.error = "unable to create project directory: " + target.parent_path().string();
            return result;
        }
    }

    const std::string text = writeManifestJson(manifest);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            result.error = "unable to write: " + temp.string();
            return result;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            result.error = "write failed: " + temp.string();
            return result;
        }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        // rename onto an existing file fails on some platforms: replace explicitly.
        std::error_code removeEc;
        std::filesystem::remove(target, removeEc);
        ec.clear();
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            std::filesystem::remove(temp, removeEc);
            result.error = "unable to replace: " + projectJsonPath;
            return result;
        }
    }

    result.ok = true;
    return result;
}

SaveResult saveToDirectory(const ProjectManifest& manifest, const std::string& projectDirectory) {
    std::string jsonPath = projectDirectory;
    if (!jsonPath.empty() && jsonPath.back() != '/' && jsonPath.back() != '\\') {
        jsonPath.push_back('/');
    }
    jsonPath += "project.json";
    return saveToFile(manifest, jsonPath);
}

SaveResult createAppProject(const std::string& projectDirectory, const std::string& name) {
    SaveResult result;
    const std::filesystem::path dir(projectDirectory);
    result.path = (dir / "project.json").string();
    std::error_code ec;
    if (std::filesystem::exists(dir / "project.json", ec)) {
        result.error = result.path + " already exists";
        return result;
    }
    const std::filesystem::path uiPath = dir / "ui" / "main.fuseui";
    std::filesystem::create_directories(uiPath.parent_path(), ec);
    if (ec) {
        result.error = "unable to create " + uiPath.parent_path().string();
        return result;
    }
    if (!std::filesystem::exists(uiPath, ec)) {
        std::ofstream output(uiPath, std::ios::binary | std::ios::trunc);
        const std::string_view text = appTemplateMainUi();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            result.error = "unable to write " + uiPath.string();
            return result;
        }
    }
    std::string projectName = name;
    if (projectName.empty()) {
        projectName = dir.filename().string();
    }
    return saveToFile(appTemplateManifest(projectName), result.path);
}

MigrateResult migrateProjectFile(const std::string& path, bool write) {
    MigrateResult result;
    std::string jsonPath = path;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        jsonPath = (std::filesystem::path(path) / "project.json").string();
    }
    result.path = jsonPath;
    LoadResult loaded = loadFromFile(jsonPath);
    if (loaded.status != LoadStatus::Ok) {
        result.error = loaded.error;
        return result;
    }
    result.fromVersion = loaded.manifest.schemaVersion;
    result.changed = migrateToLatest(loaded.manifest);
    result.text = writeManifestJson(loaded.manifest);
    if (write && result.changed) {
        const SaveResult saved = saveToFile(loaded.manifest, jsonPath);
        if (!saved.ok) {
            result.error = saved.error;
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace fuse::project
