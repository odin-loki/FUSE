#include <fuse/project/loader.hpp>

#include <fuse/project/json_reader.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

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

    if (result.manifest.schemaVersion != 1u) {
        result.status = LoadStatus::UnsupportedSchema;
        result.error = "unsupported schemaVersion (expected 1)";
        return result;
    }

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

} // namespace fuse::project
