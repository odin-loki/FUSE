#include <fuse/project/loader.hpp>

#include <fuse/project/json_reader.hpp>

#include <fstream>
#include <sstream>

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

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
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

} // namespace fuse::project
