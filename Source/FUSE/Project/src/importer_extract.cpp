#include <fuse/project/importer_extract.hpp>

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace fuse::project {

namespace {

std::string trimToken(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return std::string(text.substr(begin, end - begin));
}

std::string extractQuotedValue(std::string_view line, std::string_view key) {
    const std::size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos) {
        return {};
    }

    const std::size_t quoteBegin = line.find('"', keyPos + key.size());
    if (quoteBegin == std::string_view::npos) {
        return {};
    }

    const std::size_t quoteEnd = line.find('"', quoteBegin + 1);
    if (quoteEnd == std::string_view::npos) {
        return {};
    }

    return std::string(line.substr(quoteBegin + 1, quoteEnd - quoteBegin - 1));
}

std::string extractAssignmentValue(std::string_view line, std::string_view key) {
    const std::size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos) {
        return {};
    }

    std::size_t cursor = keyPos + key.size();
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
        ++cursor;
    }

    if (cursor < line.size() && line[cursor] == '"') {
        return extractQuotedValue(line, key);
    }

    std::size_t end = cursor;
    while (end < line.size()) {
        const char ch = line[end];
        if (ch == ';' || ch == '\n' || ch == '\r') {
            break;
        }
        ++end;
    }

    return trimToken(line.substr(cursor, end - cursor));
}

bool parseU32Token(std::string_view text, u32& out) {
    std::string trimmed = trimToken(text);
    if (trimmed.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long value = std::strtoul(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }

    out = static_cast<u32>(value);
    return true;
}

bool parseBoolToken(std::string_view text, bool& out) {
    const std::string trimmed = trimToken(text);
    if (trimmed == "true" || trimmed == "1") {
        out = true;
        return true;
    }
    if (trimmed == "false" || trimmed == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parseS32Token(std::string_view text, s32& out) {
    u32 unsignedValue = 0;
    if (!parseU32Token(text, unsignedValue)) {
        return false;
    }
    out = static_cast<s32>(unsignedValue);
    return true;
}

bool parseFloatToken(std::string_view text, float& out) {
    std::string trimmed = trimToken(text);
    if (trimmed.empty()) {
        return false;
    }

    char* end = nullptr;
    const float value = std::strtof(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }

    out = value;
    return true;
}

bool parseFloatPairToken(std::string_view text, float& x, float& y) {
    std::string trimmed = trimToken(text);
    if (trimmed.empty()) {
        return false;
    }

    for (char& ch : trimmed) {
        if (ch == ',') {
            ch = ' ';
        }
    }

    char* cursor = trimmed.data();
    char* end = nullptr;
    x = std::strtof(cursor, &end);
    if (end == cursor) {
        return false;
    }
    cursor = end;
    y = std::strtof(cursor, &end);
    return end != cursor;
}

T2DPhysicsShape inferPhysicsShapeFromClass(const std::string& className) {
    if (className.find("Box") != std::string::npos ||
        className.find("Rect") != std::string::npos) {
        return T2DPhysicsShape::Box;
    }
    if (className.find("Circle") != std::string::npos ||
        className.find("Sphere") != std::string::npos) {
        return T2DPhysicsShape::Circle;
    }
    return T2DPhysicsShape::None;
}

bool isCompositeSpriteClass(const std::string& className) {
    return className.find("CompositeSprite") != std::string::npos;
}

bool parseNewObjectLine(std::string_view line, std::string& className, std::string& objectName) {
    const std::size_t pos = line.find("new ");
    if (pos == std::string_view::npos) {
        return false;
    }

    std::size_t cursor = pos + 4;
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
        ++cursor;
    }

    std::size_t classEnd = cursor;
    while (classEnd < line.size()) {
        const char ch = line[classEnd];
        if (ch == '(' || ' ' == ch || '\t' == ch) {
            break;
        }
        ++classEnd;
    }

    if (classEnd <= cursor) {
        return false;
    }

    className = trimToken(line.substr(cursor, classEnd - cursor));

    const std::size_t openParen = line.find('(', classEnd);
    if (openParen == std::string_view::npos) {
        objectName = className;
        return true;
    }

    std::size_t nameBegin = openParen + 1;
    while (nameBegin < line.size() && std::isspace(static_cast<unsigned char>(line[nameBegin]))) {
        ++nameBegin;
    }

    std::size_t nameEnd = nameBegin;
    while (nameEnd < line.size()) {
        const char ch = line[nameEnd];
        if (ch == ')' || ch == ' ' || ch == '\t' || ch == '{') {
            break;
        }
        ++nameEnd;
    }

    if (nameEnd > nameBegin) {
        objectName = trimToken(line.substr(nameBegin, nameEnd - nameBegin));
    } else {
        objectName = className;
    }

    return true;
}

s32 countBraceDepth(std::string_view line) {
    s32 delta = 0;
    for (char ch : line) {
        if (ch == '{') {
            ++delta;
        } else if (ch == '}') {
            --delta;
        }
    }
    return delta;
}

} // namespace

T3DMissionExtract extractT3DMissionFields(const std::string& missionText) {
    T3DMissionExtract extract;
    s32 depth = 0;
    T3DSimObjectStub* current = nullptr;

    std::size_t lineBegin = 0;
    while (lineBegin < missionText.size()) {
        std::size_t lineEnd = missionText.find('\n', lineBegin);
        if (lineEnd == std::string::npos) {
            lineEnd = missionText.size();
        }

        const std::string_view line(missionText.data() + lineBegin, lineEnd - lineBegin);
        lineBegin = lineEnd + 1;

        std::string className;
        std::string objectName;
        if (parseNewObjectLine(line, className, objectName)) {
            if (extract.missionName.empty() &&
                (className == "Scene" || className == "SimGroup")) {
                extract.missionName = objectName;
            }

            T3DSimObjectStub stub;
            stub.className = className;
            stub.objectName = objectName;
            extract.simObjects.push_back(stub);
            current = &extract.simObjects.back();
        }

        const std::string materialAsset = extractQuotedValue(line, "MaterialAsset = ");
        if (!materialAsset.empty()) {
            extract.materials.push_back({materialAsset});
            if (current != nullptr) {
                current->materialAsset = materialAsset;
            }
        }

        const std::string shaderAsset = extractQuotedValue(line, "ShaderData = ");
        if (!shaderAsset.empty()) {
            extract.shaders.push_back({shaderAsset});
            if (current != nullptr) {
                current->shaderAsset = shaderAsset;
            }
        }

        const std::string datablock = extractAssignmentValue(line, "dataBlock = ");
        if (!datablock.empty()) {
            extract.datablocks.push_back({datablock});
            if (current != nullptr) {
                current->datablockRef = datablock;
            }
        }

        if (current != nullptr) {
            const std::string position = extractAssignmentValue(line, "position = ");
            if (!position.empty()) {
                current->position = position;
            }

            const std::string rotation = extractAssignmentValue(line, "rotation = ");
            if (!rotation.empty()) {
                current->rotation = rotation;
            }

            const std::string scale = extractAssignmentValue(line, "scale = ");
            if (!scale.empty()) {
                current->scale = scale;
            }
        }

        depth += countBraceDepth(line);
        if (depth <= 0) {
            current = nullptr;
        }
    }

    if (extract.missionName.empty()) {
        extract.missionName = "UnnamedMission";
    }

    return extract;
}

T2DModuleExtract extractT2DModuleFields(const std::string& moduleText, const std::string& fallbackPath) {
    T2DModuleExtract extract;
    s32 depth = 0;
    T2DSceneNodeStub* current = nullptr;

    const std::string markers[] = {"module \"", "module @"};
    for (const std::string& marker : markers) {
        const std::size_t pos = moduleText.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        std::size_t cursor = pos + marker.size();
        if (marker == "module @") {
            while (cursor < moduleText.size() && std::isspace(static_cast<unsigned char>(moduleText[cursor]))) {
                ++cursor;
            }
        }

        std::size_t end = cursor;
        while (end < moduleText.size()) {
            const char ch = moduleText[end];
            if (ch == '"' || ch == ';' || ch == '\n' || ch == '\r') {
                break;
            }
            ++end;
        }

        if (end > cursor) {
            extract.moduleName = moduleText.substr(cursor, end - cursor);
            break;
        }
    }

    if (extract.moduleName.empty()) {
        const std::size_t slash = fallbackPath.find_last_of("/\\");
        std::string leaf = slash != std::string::npos ? fallbackPath.substr(slash + 1) : fallbackPath;
        const std::size_t dot = leaf.find('.');
        if (dot != std::string::npos) {
            leaf = leaf.substr(0, dot);
        }
        extract.moduleName = leaf.empty() ? "UnnamedModule" : leaf;
    }

    std::size_t lineBegin = 0;
    while (lineBegin < moduleText.size()) {
        std::size_t lineEnd = moduleText.find('\n', lineBegin);
        if (lineEnd == std::string::npos) {
            lineEnd = moduleText.size();
        }

        const std::string_view line(moduleText.data() + lineBegin, lineEnd - lineBegin);
        lineBegin = lineEnd + 1;

        std::string className;
        std::string objectName;
        if (parseNewObjectLine(line, className, objectName)) {
            T2DSceneNodeStub node;
            node.className = className;
            node.objectName = objectName;
            node.depth = depth;
            node.isCompositeSprite = isCompositeSpriteClass(className);
            node.physicsShape = inferPhysicsShapeFromClass(className);
            extract.sceneNodes.push_back(node);
            current = &extract.sceneNodes.back();
        }

        if (current != nullptr) {
            const std::string position = extractAssignmentValue(line, "position = ");
            if (!position.empty()) {
                current->position = position;
            }

            u32 layerValue = 0;
            const std::string layer = extractAssignmentValue(line, "layer = ");
            if (!layer.empty() && parseU32Token(layer, layerValue)) {
                current->layer = static_cast<s32>(layerValue);
            }

            const std::string sceneLayer = extractAssignmentValue(line, "SceneLayer = ");
            if (!sceneLayer.empty() && parseU32Token(sceneLayer, layerValue)) {
                current->layer = static_cast<s32>(layerValue);
            }

            u32 sortValue = 0;
            const std::string sortPoint = extractAssignmentValue(line, "sortPoint = ");
            if (!sortPoint.empty() && parseU32Token(sortPoint, sortValue)) {
                current->sortKey = sortValue;
            }

            const std::string sortKey = extractAssignmentValue(line, "sortKey = ");
            if (!sortKey.empty() && parseU32Token(sortKey, sortValue)) {
                current->sortKey = sortValue;
            }

            bool physics = false;
            const std::string physicsEnabled = extractAssignmentValue(line, "physicsEnabled = ");
            if (!physicsEnabled.empty() && parseBoolToken(physicsEnabled, physics)) {
                current->physicsEnabled = physics;
            }

            const std::string usePhysics = extractAssignmentValue(line, "usePhysics = ");
            if (!usePhysics.empty() && parseBoolToken(usePhysics, physics)) {
                current->physicsEnabled = physics;
            }

            s32 collisionLayerValue = 0;
            const std::string collisionLayer = extractAssignmentValue(line, "collisionLayer = ");
            if (!collisionLayer.empty() && parseS32Token(collisionLayer, collisionLayerValue)) {
                current->collisionLayer = collisionLayerValue;
            }

            const std::string sceneGroup = extractAssignmentValue(line, "SceneGroup = ");
            if (!sceneGroup.empty() && parseS32Token(sceneGroup, collisionLayerValue)) {
                current->collisionLayer = collisionLayerValue;
            }

            u32 maskValue = 0;
            const std::string collisionMask = extractAssignmentValue(line, "collisionMask = ");
            if (!collisionMask.empty() && parseU32Token(collisionMask, maskValue)) {
                current->collisionMask = maskValue;
            }

            const std::string shapeType = extractAssignmentValue(line, "shapeType = ");
            if (!shapeType.empty()) {
                if (shapeType.find("box") != std::string::npos ||
                    shapeType.find("Box") != std::string::npos) {
                    current->physicsShape = T2DPhysicsShape::Box;
                } else if (shapeType.find("circle") != std::string::npos ||
                           shapeType.find("Circle") != std::string::npos) {
                    current->physicsShape = T2DPhysicsShape::Circle;
                }
            }

            float radius = 0.f;
            const std::string collisionRadius = extractAssignmentValue(line, "collisionRadius = ");
            if (!collisionRadius.empty() && parseFloatToken(collisionRadius, radius)) {
                current->physicsRadius = radius;
                if (current->physicsShape == T2DPhysicsShape::None) {
                    current->physicsShape = T2DPhysicsShape::Circle;
                }
            }

            const std::string size = extractAssignmentValue(line, "size = ");
            if (!size.empty()) {
                float halfWidth = 0.f;
                float halfHeight = 0.f;
                if (parseFloatPairToken(size, halfWidth, halfHeight)) {
                    current->boxHalfWidth = halfWidth * 0.5f;
                    current->boxHalfHeight = halfHeight * 0.5f;
                    if (current->physicsShape == T2DPhysicsShape::None) {
                        current->physicsShape = T2DPhysicsShape::Box;
                    }
                }
            }
        }

        depth += countBraceDepth(line);
        if (depth < 0) {
            depth = 0;
        }
        if (depth <= 0) {
            current = nullptr;
        }
    }

    return extract;
}

} // namespace fuse::project
