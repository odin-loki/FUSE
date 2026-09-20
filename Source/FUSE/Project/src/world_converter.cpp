#include <fuse/project/world_converter.hpp>

#include <fuse/log/logger.hpp>
#include <fuse/project/importer_extract.hpp>
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
    s32 parentIndex = -1;
    bool hasPosition = false;
    bool hasRotation = false;
    bool hasScale = false;
    std::string datablockRef;
    std::string materialAsset;
    fuse::scene::SceneEntityTransform transform{};
};

std::string makeSceneWiringStubName(const char* kind, const std::string& objectName,
                                    const std::string& refValue) {
    return std::string("__fuse.wire|") + kind + "|" + objectName + "|" + refValue;
}

std::size_t skipMisWhitespace(const std::string& text, std::size_t cursor) {
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    return cursor;
}

std::size_t findMatchingBrace(const std::string& text, std::size_t openBrace) {
    if (openBrace >= text.size() || text[openBrace] != '{') {
        return std::string::npos;
    }

    int depth = 1;
    std::size_t cursor = openBrace + 1;
    while (cursor < text.size() && depth > 0) {
        if (text[cursor] == '{') {
            ++depth;
        } else if (text[cursor] == '}') {
            --depth;
        }
        ++cursor;
    }

    return depth == 0 ? cursor : std::string::npos;
}

bool parseMisObjectHeader(const std::string& text, std::size_t newPos, MisObject& object, std::size_t& blockStartOut) {
    std::size_t typeStart = newPos + 4;
    typeStart = skipMisWhitespace(text, typeStart);

    std::size_t typeEnd = typeStart;
    while (typeEnd < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[typeEnd])) || text[typeEnd] == '_')) {
        ++typeEnd;
    }

    if (typeEnd <= typeStart || typeEnd >= text.size() || text[typeEnd] != '(') {
        return false;
    }

    std::size_t nameStart = skipMisWhitespace(text, typeEnd + 1);
    std::size_t nameEnd = nameStart;
    while (nameEnd < text.size()) {
        const char ch = text[nameEnd];
        if (ch == ')' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '{') {
            break;
        }
        ++nameEnd;
    }

    object.type = text.substr(typeStart, typeEnd - typeStart);
    if (nameEnd > nameStart) {
        object.name = text.substr(nameStart, nameEnd - nameStart);
    }
    if (object.name.empty()) {
        object.name = object.type;
    }

    const std::size_t blockStart = text.find('{', nameEnd);
    if (blockStart == std::string::npos) {
        return false;
    }

    blockStartOut = blockStart;
    return true;
}

void fillMisObjectTransform(MisObject& object, const std::string& block) {
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

    const std::string datablockQuoted = extractQuotedValue(block, "dataBlock");
    if (!datablockQuoted.empty()) {
        object.datablockRef = datablockQuoted;
    } else {
        const std::size_t dbPos = block.find("dataBlock = ");
        if (dbPos != std::string::npos) {
            std::size_t cursor = dbPos + 12;
            while (cursor < block.size() && std::isspace(static_cast<unsigned char>(block[cursor]))) {
                ++cursor;
            }
            std::size_t end = cursor;
            while (end < block.size()) {
                const char ch = block[end];
                if (ch == ';' || ch == '\n' || ch == '\r') {
                    break;
                }
                ++end;
            }
            if (end > cursor) {
                object.datablockRef = block.substr(cursor, end - cursor);
            }
        }
    }

    object.materialAsset = extractQuotedValue(block, "MaterialAsset");
}

void extractMisObjectsRecursive(const std::string& text, std::size_t blockStart, std::size_t blockEnd,
                              s32 parentIndex, std::vector<MisObject>& objects) {
    std::size_t cursor = blockStart + 1;
    while (cursor < blockEnd) {
        const std::size_t newPos = text.find("new ", cursor);
        if (newPos == std::string::npos || newPos >= blockEnd) {
            break;
        }

        MisObject object;
        std::size_t childBlockStart = 0;
        if (!parseMisObjectHeader(text, newPos, object, childBlockStart) || childBlockStart >= blockEnd) {
            cursor = newPos + 4;
            continue;
        }

        const std::size_t childBlockEnd = findMatchingBrace(text, childBlockStart);
        if (childBlockEnd == std::string::npos || childBlockEnd > blockEnd) {
            cursor = newPos + 4;
            continue;
        }

        object.parentIndex = parentIndex;
        const std::string block = text.substr(childBlockStart, childBlockEnd - childBlockStart);
        fillMisObjectTransform(object, block);

        const s32 selfIndex = static_cast<s32>(objects.size());
        objects.push_back(object);

        extractMisObjectsRecursive(text, childBlockStart, childBlockEnd, selfIndex, objects);
        cursor = childBlockEnd;
    }
}

std::vector<MisObject> extractMisHierarchy(const std::string& text) {
    std::vector<MisObject> objects;

    const std::size_t rootPos = text.find("new ");
    if (rootPos == std::string::npos) {
        return objects;
    }

    MisObject root;
    std::size_t rootBlockStart = 0;
    if (!parseMisObjectHeader(text, rootPos, root, rootBlockStart)) {
        return objects;
    }

    const std::size_t rootBlockEnd = findMatchingBrace(text, rootBlockStart);
    if (rootBlockEnd == std::string::npos) {
        return objects;
    }

    const std::string rootBlock = text.substr(rootBlockStart, rootBlockEnd - rootBlockStart);
    fillMisObjectTransform(root, rootBlock);

    const s32 rootIndex = static_cast<s32>(objects.size());
    objects.push_back(root);
    extractMisObjectsRecursive(text, rootBlockStart, rootBlockEnd, rootIndex, objects);
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
    const std::vector<MisObject> objects = extractMisHierarchy(text);

    std::vector<s32> objectToSceneIndex(objects.size(), -1);
    s32 nextSceneIndex = 0;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].type == "Scene") {
            continue;
        }
        objectToSceneIndex[i] = nextSceneIndex++;
    }

    for (std::size_t i = 0; i < objects.size(); ++i) {
        const MisObject& object = objects[i];
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

        s32 parentIndex = -1;
        if (object.parentIndex >= 0 &&
            static_cast<std::size_t>(object.parentIndex) < objectToSceneIndex.size()) {
            parentIndex = objectToSceneIndex[static_cast<std::size_t>(object.parentIndex)];
        }

        scene.addEntity(object.name, transform, parentIndex);
    }

    u32 wiringStubCount = 0;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const MisObject& object = objects[i];
        if (object.type == "Scene") {
            continue;
        }

        const s32 wireParentIndex = objectToSceneIndex[i];
        if (wireParentIndex < 0) {
            continue;
        }

        if (!object.datablockRef.empty()) {
            scene.addEntity(makeSceneWiringStubName("datablock", object.name, object.datablockRef),
                            {},
                            wireParentIndex);
            ++wiringStubCount;
        }

        if (!object.materialAsset.empty()) {
            scene.addEntity(makeSceneWiringStubName("material", object.name, object.materialAsset),
                            {},
                            wireParentIndex);
            ++wiringStubCount;
        }
    }

    const fuse::scene::SerialiseResult serialised = fuse::scene::SceneSerialiser::save(scene, outputPath);
    if (serialised.status != fuse::scene::SerialiseStatus::Ok) {
        result.status = ConvertStatus::IoError;
        result.note = serialised.error;
        return result;
    }

    result.status = ConvertStatus::Ok;
    result.entityCount = scene.entityCount();
    result.wiringStubCount = wiringStubCount;
    result.note = "converted T3D mission to .fuselevel (" + std::to_string(result.entityCount) +
                  " entities, " + std::to_string(result.wiringStubCount) + " wiring stubs)";
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

    const T2DModuleExtract extract = extractT2DModuleFields(text, modulePath);
    fuse::scene::Scene scene(extract.moduleName);

    std::vector<s32> parentIndices;
    parentIndices.reserve(extract.sceneNodes.size());

    for (std::size_t i = 0; i < extract.sceneNodes.size(); ++i) {
        const T2DSceneNodeStub& node = extract.sceneNodes[i];
        s32 parentIndex = -1;
        if (node.depth > 0) {
            for (std::size_t j = i; j-- > 0;) {
                if (extract.sceneNodes[j].depth == node.depth - 1) {
                    parentIndex = static_cast<s32>(j);
                    break;
                }
            }
        }
        parentIndices.push_back(parentIndex);

        fuse::scene::SceneEntityTransform transform{};
        if (!node.position.empty()) {
            float x = 0.f;
            float y = 0.f;
            float z = 0.f;
            if (parseFloatTriplet(node.position, x, y, z)) {
                transform.positionX = x;
                transform.positionY = y;
                transform.positionZ = z;
            } else {
                std::istringstream stream(node.position);
                if (stream >> x >> y) {
                    transform.positionX = x;
                    transform.positionY = y;
                }
            }
        }

        const std::string entityName =
            node.objectName.empty() ? node.className : node.objectName;
        scene.addEntity(entityName, transform, parentIndex);
    }

    if (scene.entityCount() == 0) {
        scene.addEntity("ModuleRoot");
    }

    const fuse::scene::SerialiseResult serialised = fuse::scene::SceneSerialiser::save(scene, outputPath);
    if (serialised.status != fuse::scene::SerialiseStatus::Ok) {
        result.status = ConvertStatus::IoError;
        result.note = serialised.error;
        return result;
    }

    result.status = ConvertStatus::Ok;
    result.entityCount = scene.entityCount();
    result.note = "converted T2D module to .fuselevel (" + std::to_string(result.entityCount) +
                  " entities from toybox scan)";
    fuse::log::info("convertT2DModuleToFuselevel: %s -> %s (%u entities)",
                    modulePath.c_str(),
                    outputPath.c_str(),
                    result.entityCount);
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

Ensure3DWorldResult ensureDefault3DWorldReady(const LoadResult& projectLoad) {
    Ensure3DWorldResult result;
    if (projectLoad.status != LoadStatus::Ok) {
        result.note = "project load failed";
        return result;
    }
    if (projectLoad.manifest.defaultWorld3D.empty()) {
        result.note = "project missing defaultWorld3D";
        return result;
    }

    auto joinPath = [](const std::string& root, const std::string& relative) {
        if (root.empty()) {
            return relative;
        }
        return (std::filesystem::path(root) / relative).lexically_normal().string();
    };

    const std::string fuselevelPath =
        joinPath(projectLoad.manifest.projectRoot, projectLoad.manifest.defaultWorld3D);
    result.loadedPath = fuselevelPath;

    const LegacySourceResolution missionSource =
        resolveParityLegacySource(projectLoad.manifest, fuselevelPath, ".mis");
    result.sourceOrigin = missionSource.origin;

    std::error_code ec;
    const bool fuselevelExists = std::filesystem::exists(fuselevelPath, ec);
    const bool canRefreshFromMis = missionSource.origin != LegacySourceOrigin::Missing &&
                                   std::filesystem::exists(missionSource.path, ec) &&
                                   (!fuselevelExists ||
                                    std::filesystem::last_write_time(missionSource.path) >
                                        std::filesystem::last_write_time(fuselevelPath));
    if (!fuselevelExists || canRefreshFromMis) {
        if (missionSource.origin == LegacySourceOrigin::Missing) {
            result.note = "missing .fuselevel and legacy .mis: " + fuselevelPath;
            return result;
        }

        const ConvertResult converted =
            convertT3DMissionToFuselevel(missionSource.path, fuselevelPath);
        if (converted.status != ConvertStatus::Ok) {
            result.note = converted.note.empty() ? "T3D mission convert failed" : converted.note;
            return result;
        }
        result.entityCount = converted.entityCount;
        result.wiringStubCount = converted.wiringStubCount;
    }

    result.ok = std::filesystem::exists(fuselevelPath, ec);
    if (result.ok) {
        result.note = missionSource.note.empty() ? "3D world ready" : missionSource.note;
    } else {
        result.note = "fuselevel still missing after convert attempt";
    }
    return result;
}

} // namespace fuse::project
