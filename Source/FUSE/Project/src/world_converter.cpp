#include <fuse/project/world_converter.hpp>

#include <fuse/log/logger.hpp>
#include <fuse/scene/serialiser.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

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

bool ensureParentDirectory(const std::string& outputPath) {
    const std::filesystem::path path(outputPath);
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    return !error;
}

std::string extractMissionSceneName(const std::string& text) {
    const std::string markers[] = {"new Scene(", "new SimGroup("};
    for (const std::string& marker : markers) {
        const std::size_t pos = text.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        std::size_t cursor = pos + marker.size();
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }

        std::size_t end = cursor;
        while (end < text.size()) {
            const char ch = text[end];
            if (ch == ')' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '{') {
                break;
            }
            ++end;
        }

        if (end > cursor) {
            return text.substr(cursor, end - cursor);
        }
    }
    return "ImportedMission";
}

std::string extractT2DModuleName(const std::string& text, const std::string& fallbackPath) {
    const std::string markers[] = {"module \"", "module @"};
    for (const std::string& marker : markers) {
        const std::size_t pos = text.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        std::size_t cursor = pos + marker.size();
        if (marker == "module @") {
            while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
        }

        std::size_t end = cursor;
        while (end < text.size()) {
            const char ch = text[end];
            if (ch == '"' || ch == ';' || ch == '\n' || ch == '\r') {
                break;
            }
            ++end;
        }

        if (end > cursor) {
            return text.substr(cursor, end - cursor);
        }
    }

    const std::size_t slash = fallbackPath.find_last_of("/\\");
    std::string leaf = slash != std::string::npos ? fallbackPath.substr(slash + 1) : fallbackPath;
    const std::size_t dot = leaf.find('.');
    if (dot != std::string::npos) {
        leaf = leaf.substr(0, dot);
    }
    return leaf.empty() ? "ImportedModule" : leaf;
}

bool parseFloatTriplet(const std::string& text, float& a, float& b, float& c) {
    std::istringstream stream(text);
    return static_cast<bool>(stream >> a >> b >> c);
}

bool parseFloatQuat(const std::string& text, float& x, float& y, float& z, float& w) {
    std::istringstream stream(text);
    return static_cast<bool>(stream >> x >> y >> z >> w);
}

std::string extractQuotedValue(const std::string& block, const std::string& key) {
    const std::string needle = key + " = \"";
    const std::size_t pos = block.find(needle);
    if (pos == std::string::npos) {
        return {};
    }

    const std::size_t start = pos + needle.size();
    const std::size_t end = block.find('"', start);
    if (end == std::string::npos || end <= start) {
        return {};
    }

    return block.substr(start, end - start);
}

struct MisObject {
    std::string type;
    std::string name;
    bool hasPosition = false;
    bool hasRotation = false;
    bool hasScale = false;
    fuse::scene::SceneEntityTransform transform{};
};

std::vector<MisObject> extractMisObjects(const std::string& text) {
    std::vector<MisObject> objects;
    std::size_t cursor = 0;

    while (cursor < text.size()) {
        const std::size_t newPos = text.find("new ", cursor);
        if (newPos == std::string::npos) {
            break;
        }

        std::size_t typeStart = newPos + 4;
        while (typeStart < text.size() && std::isspace(static_cast<unsigned char>(text[typeStart]))) {
            ++typeStart;
        }

        std::size_t typeEnd = typeStart;
        while (typeEnd < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[typeEnd])) || text[typeEnd] == '_')) {
            ++typeEnd;
        }

        if (typeEnd <= typeStart) {
            cursor = newPos + 4;
            continue;
        }

        if (text[typeEnd] != '(') {
            cursor = typeEnd;
            continue;
        }

        std::size_t nameStart = typeEnd + 1;
        while (nameStart < text.size() && std::isspace(static_cast<unsigned char>(text[nameStart]))) {
            ++nameStart;
        }

        std::size_t nameEnd = nameStart;
        while (nameEnd < text.size()) {
            const char ch = text[nameEnd];
            if (ch == ')' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '{') {
                break;
            }
            ++nameEnd;
        }

        MisObject object;
        object.type = text.substr(typeStart, typeEnd - typeStart);
        if (nameEnd > nameStart) {
            object.name = text.substr(nameStart, nameEnd - nameStart);
        }
        if (object.name.empty()) {
            object.name = object.type;
        }

        const std::size_t blockStart = text.find('{', nameEnd);
        if (blockStart == std::string::npos) {
            cursor = nameEnd;
            continue;
        }

        int depth = 1;
        std::size_t blockEnd = blockStart + 1;
        while (blockEnd < text.size() && depth > 0) {
            if (text[blockEnd] == '{') {
                ++depth;
            } else if (text[blockEnd] == '}') {
                --depth;
            }
            ++blockEnd;
        }

        const std::string block = text.substr(blockStart, blockEnd - blockStart);

        const std::string position = extractQuotedValue(block, "position");
        if (!position.empty()) {
            object.hasPosition = parseFloatTriplet(position,
                                                   object.transform.positionX,
                                                   object.transform.positionY,
                                                   object.transform.positionZ);
        }

        const std::string rotation = extractQuotedValue(block, "rotation");
        if (!rotation.empty()) {
            object.hasRotation = parseFloatQuat(rotation,
                                                 object.transform.rotationX,
                                                 object.transform.rotationY,
                                                 object.transform.rotationZ,
                                                 object.transform.rotationW);
        }

        const std::string scale = extractQuotedValue(block, "scale");
        if (!scale.empty()) {
            object.hasScale = parseFloatTriplet(scale,
                                                object.transform.scaleX,
                                                object.transform.scaleY,
                                                object.transform.scaleZ);
        }

        objects.push_back(std::move(object));
        cursor = newPos + 4;
    }

    return objects;
}

ConvertResult makeIoError(const std::string& outputPath, const std::string& note) {
    ConvertResult result;
    result.status = ConvertStatus::IoError;
    result.outputPath = outputPath;
    result.note = note;
    return result;
}

} // namespace

ConvertResult convertT3DMissionToFuselevel(const std::string& missionPath,
                                           const std::string& outputPath) {
    ConvertResult result;
    result.outputPath = outputPath;

    const std::string text = readFileToString(missionPath);
    if (text.empty()) {
        return makeIoError(outputPath, "unable to read mission file");
    }

    if (!ensureParentDirectory(outputPath)) {
        return makeIoError(outputPath, "unable to create output directory");
    }

    fuse::scene::Scene scene(extractMissionSceneName(text));
    const std::vector<MisObject> objects = extractMisObjects(text);
    for (const MisObject& object : objects) {
        if (object.type == "Scene") {
            continue;
        }

        fuse::scene::SceneEntityTransform transform = object.transform;
        if (!object.hasRotation) {
            transform.rotationW = 1.f;
        }
        if (!object.hasScale) {
            transform.scaleX = 1.f;
            transform.scaleY = 1.f;
            transform.scaleZ = 1.f;
        }

        scene.addEntity(object.name, transform);
    }

    const fuse::scene::SerialiseResult serialised = fuse::scene::SceneSerialiser::save(scene, outputPath);
    if (serialised.status != fuse::scene::SerialiseStatus::Ok) {
        result.status = ConvertStatus::IoError;
        result.note = serialised.error;
        return result;
    }

    result.status = ConvertStatus::Ok;
    result.entityCount = scene.entityCount();
    result.note = "converted T3D mission to .fuselevel (" + std::to_string(result.entityCount) + " entities)";
    fuse::log::info("convertT3DMissionToFuselevel: %s -> %s (%u entities)",
                    missionPath.c_str(),
                    outputPath.c_str(),
                    result.entityCount);
    return result;
}

ConvertResult convertT2DModuleToFuselevel(const std::string& modulePath,
                                          const std::string& outputPath) {
    ConvertResult result;
    result.outputPath = outputPath;

    const std::string text = readFileToString(modulePath);
    if (text.empty()) {
        return makeIoError(outputPath, "unable to read module file");
    }

    if (!ensureParentDirectory(outputPath)) {
        return makeIoError(outputPath, "unable to create output directory");
    }

    fuse::scene::Scene scene(extractT2DModuleName(text, modulePath));
    scene.addEntity("ModuleRoot");

    const fuse::scene::SerialiseResult serialised = fuse::scene::SceneSerialiser::save(scene, outputPath);
    if (serialised.status != fuse::scene::SerialiseStatus::Ok) {
        result.status = ConvertStatus::IoError;
        result.note = serialised.error;
        return result;
    }

    result.status = ConvertStatus::Ok;
    result.entityCount = scene.entityCount();
    result.note = "converted T2D module to .fuselevel";
    fuse::log::info("convertT2DModuleToFuselevel: %s -> %s", modulePath.c_str(), outputPath.c_str());
    return result;
}

std::vector<ConvertResult> convertManifestWorlds(const ProjectManifest& project,
                                                 const std::string& outputRoot) {
    std::vector<ConvertResult> results;
    const std::string base = outputRoot.empty() ? project.projectRoot : outputRoot;

    auto joinPath = [](const std::string& root, const std::string& relative) {
        if (root.empty()) {
            return relative;
        }
        std::string path = root;
        if (path.back() != '/' && path.back() != '\\') {
            path.push_back('/');
        }
        path += relative;
        return path;
    };

    if (project.dimensions.enable3D && !project.defaultWorld3D.empty()) {
        const std::string sourcePath = joinPath(project.projectRoot, project.defaultWorld3D);
        const std::string targetPath = joinPath(base, project.defaultWorld3D);
        if (sourcePath.size() >= 4 && sourcePath.substr(sourcePath.size() - 4) == ".mis") {
            results.push_back(convertT3DMissionToFuselevel(sourcePath, targetPath));
        }
    }

    if (project.dimensions.enable2D && !project.defaultWorld2D.empty()) {
        const std::string sourcePath = joinPath(project.projectRoot, project.defaultWorld2D);
        const std::string targetPath = joinPath(base, project.defaultWorld2D);
        if (sourcePath.size() >= 3 && sourcePath.substr(sourcePath.size() - 3) == ".cs") {
            results.push_back(convertT2DModuleToFuselevel(sourcePath, targetPath));
        }
    }

    return results;
}

} // namespace fuse::project
