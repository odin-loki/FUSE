#include <fuse/project/world_converter.hpp>

#include <fuse/asset/asset_id.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/environment.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/importer_extract.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_runtime_bind.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/world3d/render_scene.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string_view>
#include <utility>
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

void populateFuselevelStats(const std::string& fuselevelPath, u32& entityCount, u32& wiringStubCount) {
    fuse::scene::Scene scene;
    const fuse::scene::SerialiseResult loaded = fuse::scene::SceneSerialiser::load(fuselevelPath, scene);
    if (loaded.status != fuse::scene::SerialiseStatus::Ok) {
        return;
    }

    entityCount = scene.entityCount();
    wiringStubCount = 0;
    for (const fuse::scene::SceneEntity& entity : scene.entities()) {
        if (fuse::scene::isWireStubEntityName(entity.name)) {
            ++wiringStubCount;
        }
    }
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

bool parseFloatTriplet(const std::string& text, float& a, float& b, float& c) {
    std::istringstream stream(text);
    return static_cast<bool>(stream >> a >> b >> c);
}

struct MisObject {
    std::string type;
    std::string name;
    s32 parentIndex = -1;
    /// Top-level `key = value;` fields of the object's own block (keys lower-cased; child blocks excluded).
    std::vector<std::pair<std::string, std::string>> fields;
    bool hasPosition = false;
    bool hasRotation = false;
    bool hasScale = false;
    std::string datablockRef;
    std::string materialAsset;
    /// FUSE convention (Y-up, right handed) — see torqueToFuse* below.
    fuse::scene::SceneEntityTransform transform{};
};

std::string makeSceneWiringStubName(const char* kind, const std::string& objectName,
                                    const std::string& refValue) {
    return std::string("__fuse.wire|") + kind + "|" + objectName + "|" + refValue;
}

std::string formatAnimatedSpriteWireValue(const T2DSceneNodeStub& node) {
    std::ostringstream value;
    value << (node.imageMap.empty() ? "unknown" : node.imageMap) << ':'
          << (node.animationName.empty() ? "default" : node.animationName) << ':' << node.frameCount
          << ':' << node.animationFps;
    return value.str();
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
        if (text[cursor] == '"') {
            // Braces inside string values do not nest.
            ++cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                cursor += (text[cursor] == '\\') ? 2u : 1u;
            }
        } else if (text[cursor] == '{') {
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
        if (ch == ')' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '{' || ch == ':') {
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

std::string lowerCopy(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string trimCopy(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Top-level `key = value;` statements of a `{ ... }` block (nested object blocks and comments skipped).
std::vector<std::pair<std::string, std::string>> parseMisBlockFields(const std::string& text, std::size_t blockStart,
                                                                     std::size_t blockEnd) {
    std::vector<std::pair<std::string, std::string>> fields;
    // Flatten: copy the block body at depth 1, dropping nested blocks and comments.
    std::string flat;
    int depth = 0;
    for (std::size_t i = blockStart; i < blockEnd; ++i) {
        const char c = text[i];
        if (c == '"') {
            std::size_t end = i + 1;
            while (end < blockEnd && text[end] != '"') {
                end += (text[end] == '\\') ? 2u : 1u;
            }
            if (depth == 1) {
                flat.append(text, i, std::min(end + 1, blockEnd) - i);
            }
            i = end;
            continue;
        }
        if (c == '/' && i + 1 < blockEnd && text[i + 1] == '/') {
            while (i < blockEnd && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 1) {
                flat.push_back(';'); // the child's `new X(...) { }` becomes an empty statement
            }
            continue;
        }
        if (depth == 1) {
            flat.push_back(c);
        }
    }

    std::size_t i = 0;
    while (i < flat.size()) {
        const std::size_t eq = flat.find('=', i);
        const std::size_t semi = flat.find(';', i);
        if (eq == std::string::npos) {
            break;
        }
        if (semi != std::string::npos && semi < eq) {
            i = semi + 1;
            continue;
        }
        const std::string key = lowerCopy(trimCopy(flat.substr(i, eq - i)));
        std::size_t v = skipMisWhitespace(flat, eq + 1);
        std::string value;
        std::size_t next = 0;
        if (v < flat.size() && flat[v] == '"') {
            std::size_t end = v + 1;
            while (end < flat.size() && flat[end] != '"') {
                if (flat[end] == '\\' && end + 1 < flat.size()) {
                    value.push_back(flat[end + 1]);
                    end += 2;
                    continue;
                }
                value.push_back(flat[end++]);
            }
            const std::size_t after = flat.find(';', end);
            next = after == std::string::npos ? flat.size() : after + 1;
        } else {
            const std::size_t end = flat.find(';', v);
            value = trimCopy(flat.substr(v, (end == std::string::npos ? flat.size() : end) - v));
            next = end == std::string::npos ? flat.size() : end + 1;
        }
        if (!key.empty() && key.find_first_of(" \t\r\n(") == std::string::npos) {
            fields.emplace_back(key, value);
        }
        i = next;
    }
    return fields;
}

const std::string* findField(const MisObject& object, const char* key) {
    for (const auto& [name, value] : object.fields) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

f32 fieldFloat(const MisObject& object, const char* key, f32 fallback) {
    const std::string* value = findField(object, key);
    if (value == nullptr) {
        return fallback;
    }
    std::istringstream stream(*value);
    f32 v = 0.f;
    return (stream >> v) && std::isfinite(v) ? v : fallback;
}

/// Up to 4 floats of a field ("r g b a"); returns how many parsed.
u32 fieldFloats(const MisObject& object, const char* key, f32* out, u32 count) {
    const std::string* value = findField(object, key);
    if (value == nullptr) {
        return 0u;
    }
    std::istringstream stream(*value);
    u32 parsed = 0;
    for (; parsed < count; ++parsed) {
        f32 v = 0.f;
        if (!(stream >> v) || !std::isfinite(v)) {
            break;
        }
        out[parsed] = v;
    }
    return parsed;
}

// --- Torque (Z-up: +X right, +Y forward, +Z up) -> FUSE (Y-up: +X right, +Y up, -Z forward) ------------------
// A proper rotation (-90 degrees about X): p' = (x, z, -y). Rotations conjugate (axis mapped the same way,
// angle kept); axis-aligned scale swaps y / z.

void torqueToFusePosition(const f32 in[3], f32 out[3]) {
    out[0] = in[0];
    out[1] = in[2];
    out[2] = -in[1];
}

/// T3D rotation field "ax ay az angleDegrees" (axis-angle) -> FUSE quaternion (x, y, z, w).
void torqueAxisAngleToFuseQuat(const f32 axisAngle[4], f32 q[4]) {
    f32 axis[3];
    torqueToFusePosition(axisAngle, axis);
    const f32 len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const f32 angle = axisAngle[3] * 3.14159265358979323846f / 180.f;
    if (len < 1e-8f || std::fabs(angle) < 1e-12f) {
        q[0] = 0.f;
        q[1] = 0.f;
        q[2] = 0.f;
        q[3] = 1.f;
        return;
    }
    const f32 s = std::sin(angle * 0.5f) / len;
    q[0] = axis[0] * s;
    q[1] = axis[1] * s;
    q[2] = axis[2] * s;
    q[3] = std::cos(angle * 0.5f);
}

void quatRotate(const f32 q[4], const f32 v[3], f32 out[3]) {
    // v' = v + 2w (q x v) + 2 q x (q x v)
    const f32 t[3] = {2.f * (q[1] * v[2] - q[2] * v[1]), 2.f * (q[2] * v[0] - q[0] * v[2]),
                      2.f * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

/// Rotation whose local +Z points along `dir` (lights shine down local -Z).
void quatLookingAlongPositiveZ(const f32 dirIn[3], f32 q[4]) {
    f32 z[3] = {dirIn[0], dirIn[1], dirIn[2]};
    const f32 len = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (len < 1e-8f) {
        z[0] = 0.f;
        z[1] = 1.f;
        z[2] = 0.f;
    } else {
        for (f32& c : z) {
            c /= len;
        }
    }
    f32 x[3] = {z[2], 0.f, -z[0]}; // any unit vector orthogonal to z
    f32 xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-4f) {
        x[0] = 1.f;
        x[1] = 0.f;
        x[2] = 0.f;
        xl = 1.f;
    }
    x[0] /= xl;
    x[2] /= xl;
    const f32 y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    // Rotation matrix columns x, y, z -> quaternion.
    const f32 m00 = x[0], m11 = y[1], m22 = z[2];
    const f32 trace = m00 + m11 + m22;
    if (trace > 0.f) {
        const f32 s = std::sqrt(trace + 1.f) * 2.f;
        q[3] = 0.25f * s;
        q[0] = (y[2] - z[1]) / s;
        q[1] = (z[0] - x[2]) / s;
        q[2] = (x[1] - y[0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (y[2] - z[1]) / s;
        q[0] = 0.25f * s;
        q[1] = (y[0] + x[1]) / s;
        q[2] = (z[0] + x[2]) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (z[0] - x[2]) / s;
        q[0] = (y[0] + x[1]) / s;
        q[1] = 0.25f * s;
        q[2] = (z[1] + y[2]) / s;
    } else {
        const f32 s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (x[1] - y[0]) / s;
        q[0] = (z[0] + x[2]) / s;
        q[1] = (z[1] + y[2]) / s;
        q[2] = 0.25f * s;
    }
}

f32 srgbToLinear(f32 c) {
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// T3D colour field (LinearColorF "r g b a" in 0..1, or ColorI 0..255) -> linear RGB.
bool fieldColorLinear(const MisObject& object, const char* key, f32 out[3]) {
    f32 c[4] = {1.f, 1.f, 1.f, 1.f};
    const u32 n = fieldFloats(object, key, c, 4u);
    if (n < 3u) {
        return false;
    }
    const bool bytes = c[0] > 1.f || c[1] > 1.f || c[2] > 1.f;
    for (u32 i = 0; i < 3u; ++i) {
        out[i] = srgbToLinear(bytes ? c[i] / 255.f : c[i]);
    }
    return true;
}

void fillMisObjectTransform(MisObject& object) {
    f32 v[4] = {0.f, 0.f, 0.f, 0.f};
    if (fieldFloats(object, "position", v, 3u) == 3u) {
        f32 p[3];
        torqueToFusePosition(v, p);
        object.transform.positionX = p[0];
        object.transform.positionY = p[1];
        object.transform.positionZ = p[2];
        object.hasPosition = true;
    }
    f32 aa[4] = {0.f, 0.f, 1.f, 0.f};
    if (fieldFloats(object, "rotation", aa, 4u) == 4u) {
        f32 q[4];
        torqueAxisAngleToFuseQuat(aa, q);
        object.transform.rotationX = q[0];
        object.transform.rotationY = q[1];
        object.transform.rotationZ = q[2];
        object.transform.rotationW = q[3];
        object.hasRotation = true;
    }
    f32 s[3] = {1.f, 1.f, 1.f};
    if (fieldFloats(object, "scale", s, 3u) == 3u) {
        object.transform.scaleX = s[0];
        object.transform.scaleY = s[2];
        object.transform.scaleZ = s[1];
        object.hasScale = true;
    }
    if (const std::string* datablock = findField(object, "datablock")) {
        object.datablockRef = *datablock;
    }
    if (const std::string* material = findField(object, "materialasset")) {
        object.materialAsset = *material;
    } else if (const std::string* legacy = findField(object, "material")) {
        object.materialAsset = *legacy;
    }
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
        object.fields = parseMisBlockFields(text, childBlockStart, childBlockEnd);
        fillMisObjectTransform(object);

        const s32 selfIndex = static_cast<s32>(objects.size());
        objects.push_back(std::move(object));

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

    root.fields = parseMisBlockFields(text, rootBlockStart, rootBlockEnd);
    fillMisObjectTransform(root);

    const s32 rootIndex = static_cast<s32>(objects.size());
    objects.push_back(std::move(root));
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

bool iequalsAscii(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Mesh asset id of a TSStatic shape: ShapeAsset "Module:Name" -> game:/cooked/shapes/Module/Name.fusemesh,
/// shapeName "art/shapes/x.dts" -> game:/cooked/art/shapes/x.fusemesh (DTS / DAE import is L-DTS; the id is
/// what that cook will register).
fuse::asset::AssetId tsStaticShapeAssetId(const MisObject& object) {
    const std::string* asset = findField(object, "shapeasset");
    if (asset != nullptr && !asset->empty()) {
        std::string ref = *asset;
        const std::size_t colon = ref.find(':');
        if (colon != std::string::npos) {
            ref = ref.substr(0, colon) + "/" + ref.substr(colon + 1);
        }
        return fuse::asset::asset_id_of("game:/cooked/shapes/" + ref + ".fusemesh");
    }
    const std::string* shape = findField(object, "shapename");
    if (shape != nullptr && !shape->empty()) {
        std::string path = *shape;
        std::replace(path.begin(), path.end(), '\\', '/');
        const std::size_t dot = path.find_last_of('.');
        const std::size_t slash = path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            path.resize(dot);
        }
        return fuse::asset::asset_id_of("game:/cooked/" + path + ".fusemesh");
    }
    return fuse::asset::AssetId{};
}

void setLocalTransform(fuse::ecs::Transform& t, const fuse::scene::SceneEntityTransform& s) {
    t.position = fuse::ecs::vec3{s.positionX, s.positionY, s.positionZ, 1.f};
    t.rotation = fuse::ecs::quat{s.rotationX, s.rotationY, s.rotationZ, s.rotationW};
    t.scale = fuse::ecs::vec3{s.scaleX, s.scaleY, s.scaleZ, 0.f};
    t.dirty = true;
}

/// Value-initialised component (padding zeroed, so converted files are byte-deterministic).
template <typename T>
T zeroed() {
    T value;
    std::memset(static_cast<void*>(&value), 0, sizeof(T));
    new (&value) T();
    return value;
}

} // namespace

fuse::asset::AssetId t3dMaterialRefAssetId(const std::string& materialRef) {
    if (materialRef.empty()) {
        return fuse::asset::AssetId{};
    }
    const std::string cooked = materialVirtualPathToCookOutput(materialAssetToVirtualPath(materialRef));
    return cooked.empty() ? fuse::asset::AssetId{} : fuse::asset::asset_id_of("game:/" + cooked);
}

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

    // Level-wide inputs other archetypes read (Skylight takes the Sun's ambient colour).
    f32 sunAmbient[3] = {1.f, 1.f, 1.f};
    bool haveSunAmbient = false;
    for (const MisObject& object : objects) {
        if (iequalsAscii(object.type, "Sun") && fieldColorLinear(object, "ambient", sunAmbient)) {
            haveSunAmbient = true;
            break;
        }
    }

    // Per-level material slots (ecs::Mesh::material_id = slot; MeshAssets.material = the cooked .fusemat id).
    std::vector<std::string> materialSlots;
    auto materialSlot = [&](const std::string& ref) -> u32 {
        for (u32 i = 0; i < materialSlots.size(); ++i) {
            if (materialSlots[i] == ref) {
                return i;
            }
        }
        materialSlots.push_back(ref);
        return static_cast<u32>(materialSlots.size() - 1u);
    };

    // Scene entities first (ECS entity i <-> scene entity i), then the legacy wire stubs.
    struct Pending {
        std::size_t object = 0;
        fuse::scene::SceneEntityTransform transform{};
    };
    std::vector<Pending> pending;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const MisObject& object = objects[i];
        if (object.type == "Scene") {
            continue;
        }

        fuse::scene::SceneEntityTransform transform = object.transform;
        if (!object.hasRotation) {
            transform.rotationX = 0.f;
            transform.rotationY = 0.f;
            transform.rotationZ = 0.f;
            transform.rotationW = 1.f;
        }
        if (!object.hasScale) {
            transform.scaleX = 1.f;
            transform.scaleY = 1.f;
            transform.scaleZ = 1.f;
        }
        if (iequalsAscii(object.type, "GroundPlane")) {
            // The unit builtin plane (2 x 2) scaled to kGroundPlaneHalfExtent (scene table and ECS agree).
            transform.scaleX *= kGroundPlaneHalfExtent;
            transform.scaleZ *= kGroundPlaneHalfExtent;
        }
        if (iequalsAscii(object.type, "Sun")) {
            // Direction from azimuth (degrees clockwise from +Y / north) and elevation (T3D Sun::_conformLights).
            const f32 az = fieldFloat(object, "azimuth", 0.f) * 3.14159265358979323846f / 180.f;
            const f32 el = fieldFloat(object, "elevation", 35.f) * 3.14159265358979323846f / 180.f;
            const f32 toSunTorque[3] = {std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)};
            f32 toSun[3];
            torqueToFusePosition(toSunTorque, toSun);
            f32 q[4];
            quatLookingAlongPositiveZ(toSun, q);
            transform.rotationX = q[0];
            transform.rotationY = q[1];
            transform.rotationZ = q[2];
            transform.rotationW = q[3];
            transform.scaleX = 1.f;
            transform.scaleY = 1.f;
            transform.scaleZ = 1.f;
            if (!object.hasPosition) {
                transform.positionY = 10.f;
            }
        }

        s32 parentIndex = -1;
        if (object.parentIndex >= 0 &&
            static_cast<std::size_t>(object.parentIndex) < objectToSceneIndex.size()) {
            parentIndex = objectToSceneIndex[static_cast<std::size_t>(object.parentIndex)];
        }

        scene.addEntity(object.name, transform, parentIndex);
        pending.push_back(Pending{i, transform});
    }

    u32 wiringStubCount = 0;
    u32 unexpectedStubs = 0;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const MisObject& object = objects[i];
        if (object.type == "Scene") {
            continue;
        }

        const s32 wireParentIndex = objectToSceneIndex[i];
        if (wireParentIndex < 0) {
            continue;
        }

        // Legacy binding stubs (read by wire_runtime_bind / t3d_datablock_resolve / the editor's material
        // VFS loads). The components below already carry the same data; these are expected stubs.
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

    // --- UNI-U7-MIS-1: archetype table -> ECS components -------------------------------------------------------
    fuse::ecs::Registry registry;
    registry.init();
    std::vector<fuse::ecs::EntityID> ids;
    ids.reserve(scene.entityCount());
    for (u32 i = 0; i < scene.entityCount(); ++i) {
        ids.push_back(registry.create());
    }
    MissionArchetypeCounts& counts = result.components;
    std::vector<std::string> unhandledStubs;
    for (usize p = 0; p < pending.size(); ++p) {
        const MisObject& object = objects[pending[p].object];
        const fuse::ecs::EntityID e = ids[p];
        const fuse::scene::SceneEntity& sceneEntity = scene.entities()[p];

        auto transform = zeroed<fuse::ecs::Transform>();
        setLocalTransform(transform, pending[p].transform);
        if (sceneEntity.parentIndex >= 0) {
            transform.parent = ids[static_cast<usize>(sceneEntity.parentIndex)];
        }
        registry.add<fuse::ecs::Transform>(e, transform);

        const std::string& type = object.type;
        if (iequalsAscii(type, "SimGroup") || iequalsAscii(type, "SimSet")) {
            ++counts.groups;
        } else if (iequalsAscii(type, "Sun")) {
            auto light = zeroed<fuse::ecs::DirectionalLight>();
            f32 color[3] = {1.f, 1.f, 1.f};
            (void)fieldColorLinear(object, "color", color);
            light.color = fuse::ecs::vec3{color[0], color[1], color[2], 0.f};
            light.intensity = fieldFloat(object, "brightness", 1.f);
            registry.add<fuse::ecs::DirectionalLight>(e, light);
            ++counts.directionalLights;
        } else if (iequalsAscii(type, "PointLight")) {
            auto light = zeroed<fuse::ecs::PointLight>();
            f32 color[3] = {1.f, 1.f, 1.f};
            (void)fieldColorLinear(object, "color", color);
            light.color = fuse::ecs::vec3{color[0], color[1], color[2], 0.f};
            light.intensity = fieldFloat(object, "brightness", 1.f);
            light.radius = fieldFloat(object, "radius", 10.f);
            registry.add<fuse::ecs::PointLight>(e, light);
            ++counts.pointLights;
        } else if (iequalsAscii(type, "SpotLight")) {
            auto light = zeroed<fuse::ecs::SpotLight>();
            f32 color[3] = {1.f, 1.f, 1.f};
            (void)fieldColorLinear(object, "color", color);
            light.color = fuse::ecs::vec3{color[0], color[1], color[2], 0.f};
            light.intensity = fieldFloat(object, "brightness", 1.f);
            light.radius = fieldFloat(object, "range", 10.f);
            // T3D cone angles are full apertures; FUSE stores half angles.
            light.inner_cone_deg = 0.5f * fieldFloat(object, "innerangle", 40.f);
            light.outer_cone_deg = 0.5f * fieldFloat(object, "outerangle", 45.f);
            registry.add<fuse::ecs::SpotLight>(e, light);
            ++counts.spotLights;
        } else if (iequalsAscii(type, "Skylight")) {
            auto ambient = zeroed<fuse::ecs::AmbientLight>();
            ambient.color = haveSunAmbient ? fuse::ecs::vec3{sunAmbient[0], sunAmbient[1], sunAmbient[2], 0.f}
                                           : fuse::ecs::vec3{1.f, 1.f, 1.f, 0.f};
            ambient.intensity = 1.f;
            ambient.use_ddgi = 1u;
            ambient.ddgi_probe_spacing = 2.f;
            registry.add<fuse::ecs::AmbientLight>(e, ambient);
            ++counts.ambientLights;
        } else if (iequalsAscii(type, "SkyBox") || iequalsAscii(type, "ScatterSky")) {
            auto sky = zeroed<fuse::ecs::SkyAtmosphere>();
            if (iequalsAscii(type, "SkyBox")) {
                sky.mode = fuse::ecs::SkyAtmosphere::Cubemap;
                sky.sky_material = t3dMaterialRefAssetId(object.materialAsset);
            } else {
                sky.mode = fuse::ecs::SkyAtmosphere::Scattering;
                sky.sky_brightness = fieldFloat(object, "skybrightness", 25.f);
                sky.rayleigh_scattering = fieldFloat(object, "rayleighscattering", 0.0035f);
                sky.mie_scattering = fieldFloat(object, "miescattering", 0.0045f);
                sky.sun_scale = fieldFloat(object, "sunscale", 1.f);
                sky.exposure = fieldFloat(object, "exposure", 1.f);
            }
            registry.add<fuse::ecs::SkyAtmosphere>(e, sky);
            ++counts.atmospheres;
        } else if (iequalsAscii(type, "LevelInfo")) {
            auto fog = zeroed<fuse::ecs::EnvironmentFog>();
            f32 color[3] = {0.6f, 0.6f, 0.7f};
            (void)fieldColorLinear(object, "fogcolor", color);
            fog.color = fuse::ecs::vec3{color[0], color[1], color[2], 0.f};
            f32 clear[3] = {0.f, 0.f, 0.f};
            (void)fieldColorLinear(object, "canvasclearcolor", clear);
            fog.clear_color = fuse::ecs::vec3{clear[0], clear[1], clear[2], 0.f};
            fog.density = fieldFloat(object, "fogdensity", 0.f);
            fog.density_offset = fieldFloat(object, "fogdensityoffset", 0.f);
            fog.atmosphere_height = fieldFloat(object, "fogatmosphereheight", 0.f);
            fog.visible_distance = fieldFloat(object, "visibledistance", 1000.f);
            fog.near_clip = fieldFloat(object, "nearclip", 0.1f);
            registry.add<fuse::ecs::EnvironmentFog>(e, fog);
            ++counts.fogs;
        } else if (iequalsAscii(type, "GroundPlane")) {
            // Visual: the unit builtin plane (y = 0, 2 x 2), scaled to kGroundPlaneHalfExtent above; T3D's ground
            // plane is infinite, the collider below is too.
            auto mesh = zeroed<fuse::ecs::Mesh>();
            mesh.vertex_buffer = fuse::ecs::MeshVertexBufferHandle(static_cast<u32>(fuse::world3d::BuiltinMesh::Plane), 1u);
            mesh.aabb_min = fuse::ecs::vec3{-1.f, 0.f, -1.f, 0.f};
            mesh.aabb_max = fuse::ecs::vec3{1.f, 0.f, 1.f, 0.f};
            mesh.cast_shadow = false;
            mesh.receive_shadow = true;
            mesh.visible = true;
            mesh.material_id = object.materialAsset.empty() ? 0u : materialSlot(object.materialAsset);
            registry.add<fuse::ecs::Mesh>(e, mesh);
            auto assets = zeroed<fuse::ecs::MeshAssets>();
            assets.material = t3dMaterialRefAssetId(object.materialAsset);
            registry.add<fuse::ecs::MeshAssets>(e, assets);

            // Static plane collider: world normal = rotation * +Y, distance = dot(normal, position).
            const f32 q[4] = {pending[p].transform.rotationX, pending[p].transform.rotationY,
                              pending[p].transform.rotationZ, pending[p].transform.rotationW};
            const f32 up[3] = {0.f, 1.f, 0.f};
            f32 n[3];
            quatRotate(q, up, n);
            auto collider = zeroed<fuse::ecs::Collider>();
            collider.shape = fuse::ecs::Collider::Plane;
            collider.params = fuse::ecs::vec3{n[0], n[1], n[2], 0.f};
            collider.scalar = n[0] * pending[p].transform.positionX + n[1] * pending[p].transform.positionY +
                              n[2] * pending[p].transform.positionZ;
            collider.friction_static = 0.5f;
            collider.friction_dynamic = 0.3f;
            collider.layer = 1u;
            collider.mask = 0xFFFFFFFFu;
            collider.shape_ref = fuse::ecs::Collider::kNoShapeRef;
            registry.add<fuse::ecs::Collider>(e, collider);
            auto body = zeroed<fuse::ecs::RigidBody>();
            body.mass = 0.f;
            body.inv_mass = 0.f;
            body.restitution = 0.4f;
            body.linear_damping = 0.99f;
            body.angular_damping = 0.98f;
            body.is_static = true;
            registry.add<fuse::ecs::RigidBody>(e, body);
            registry.add<fuse::ecs::TagStatic>(e);
            ++counts.meshes;
            ++counts.staticColliders;
        } else if (iequalsAscii(type, "SpawnSphere")) {
            auto spawn = zeroed<fuse::ecs::SpawnMarker>();
            spawn.datablock_id = object.datablockRef.empty() ? 0u : fuse::scene::hashWireRefName(object.datablockRef);
            spawn.active = true;
            registry.add<fuse::ecs::SpawnMarker>(e, spawn);
            ++counts.spawnMarkers;
        } else if (iequalsAscii(type, "TSStatic")) {
            // Shape import (DTS / DAE) is L-DTS: the Mesh stays without a vertex buffer (not drawn) until the
            // cooked-asset bridge maps MeshAssets.mesh to an engine mesh.
            auto mesh = zeroed<fuse::ecs::Mesh>();
            mesh.vertex_buffer = fuse::ecs::MeshVertexBufferHandle{};
            mesh.index_buffer = fuse::ecs::MeshIndexBufferHandle{};
            mesh.cast_shadow = true;
            mesh.receive_shadow = true;
            mesh.visible = true;
            mesh.material_id = object.materialAsset.empty() ? 0u : materialSlot(object.materialAsset);
            registry.add<fuse::ecs::Mesh>(e, mesh);
            auto assets = zeroed<fuse::ecs::MeshAssets>();
            assets.mesh = tsStaticShapeAssetId(object);
            assets.material = t3dMaterialRefAssetId(object.materialAsset);
            registry.add<fuse::ecs::MeshAssets>(e, assets);
            ++counts.meshes;
        } else {
            // No archetype: keep the node (name / transform / hierarchy) and flag it with an unexpected stub.
            ++counts.unhandled;
            result.unhandledClasses.push_back(type);
            unhandledStubs.push_back(makeSceneWiringStubName("archetype", object.name, type));
        }
    }
    for (const std::string& stubName : unhandledStubs) {
        // Stub parented to its owner (owner scene index = position in `pending`).
        s32 owner = -1;
        const std::string ownerName = fuse::scene::parseWireStubEntityName(stubName).owner;
        for (usize p = 0; p < pending.size(); ++p) {
            if (scene.entities()[p].name == ownerName) {
                owner = static_cast<s32>(p);
                break;
            }
        }
        scene.addEntity(stubName, {}, owner);
        ids.push_back(registry.create());
        ++wiringStubCount;
        ++unexpectedStubs;
    }
    // Wire-stub scene entities (legacy binding stubs + archetype stubs) own component-less ECS entities, so
    // ECS entity index == scene entity index across the whole file.
    fuse::ecs::TransformSystemOptions transformOptions;
    transformOptions.parallelDirtyRoots = false;
    fuse::ecs::TransformSystem::update(registry, transformOptions);

    const fuse::scene::SerialiseResult serialised = fuse::scene::SceneSerialiser::saveWithRegistry(
        scene, registry, outputPath, fuse::scene::SceneDimension::World3D);
    if (serialised.status != fuse::scene::SerialiseStatus::Ok) {
        result.status = ConvertStatus::IoError;
        result.note = serialised.error;
        return result;
    }

    result.status = ConvertStatus::Ok;
    result.entityCount = scene.entityCount();
    result.wiringStubCount = wiringStubCount;
    result.unexpectedStubCount = unexpectedStubs;
    result.ecsEntityCount = static_cast<u32>(registry.count());
    result.materialSlots = materialSlots;
    result.note = "converted T3D mission to .fuselevel v3 (" + std::to_string(result.entityCount) + " entities, " +
                  std::to_string(result.wiringStubCount) + " wiring stubs, " + std::to_string(unexpectedStubs) +
                  " unhandled archetype(s))";
    fuse::log::info("convertT3DMissionToFuselevel: %s -> %s (%u entities, %u lights, %u meshes, %u unhandled)",
                    missionPath.c_str(), outputPath.c_str(), result.entityCount,
                    counts.directionalLights + counts.pointLights + counts.spotLights, counts.meshes, counts.unhandled);
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

    std::vector<s32> nodeToSceneIndex(extract.sceneNodes.size(), -1);

    for (std::size_t i = 0; i < extract.sceneNodes.size(); ++i) {
        const T2DSceneNodeStub& node = extract.sceneNodes[i];
        s32 parentIndex = -1;
        if (node.depth > 0) {
            for (std::size_t j = i; j-- > 0;) {
                if (extract.sceneNodes[j].depth == node.depth - 1) {
                    parentIndex = nodeToSceneIndex[j];
                    break;
                }
            }
        }

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
        nodeToSceneIndex[i] = static_cast<s32>(scene.entityCount() - 1u);
    }

    if (scene.entityCount() == 0) {
        scene.addEntity("ModuleRoot");
    }

    u32 wiringStubCount = 0;
    for (std::size_t i = 0; i < extract.sceneNodes.size(); ++i) {
        const T2DSceneNodeStub& node = extract.sceneNodes[i];
        if (!node.hasAnimatedSpriteFields()) {
            continue;
        }

        const s32 wireParentIndex = nodeToSceneIndex[i];
        if (wireParentIndex < 0) {
            continue;
        }

        const std::string ownerName =
            node.objectName.empty() ? node.className : node.objectName;
        scene.addEntity(makeSceneWiringStubName("animated_sprite", ownerName,
                                                formatAnimatedSpriteWireValue(node)),
                        {},
                        wireParentIndex);
        ++wiringStubCount;
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
    result.note = "converted T2D module to .fuselevel (" + std::to_string(result.entityCount) +
                  " entities, " + std::to_string(result.wiringStubCount) +
                  " animated-sprite wiring stubs)";
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
        if (result.entityCount == 0u) {
            populateFuselevelStats(fuselevelPath, result.entityCount, result.wiringStubCount);
        }
        result.note = missionSource.note.empty() ? "3D world ready" : missionSource.note;
    } else {
        result.note = "fuselevel still missing after convert attempt";
    }
    return result;
}

Ensure2DWorldResult ensureDefault2DWorldReady(const LoadResult& projectLoad) {
    Ensure2DWorldResult result;
    if (projectLoad.status != LoadStatus::Ok) {
        result.note = "project load failed";
        return result;
    }
    if (projectLoad.manifest.defaultWorld2D.empty()) {
        result.note = "project missing defaultWorld2D";
        return result;
    }

    auto joinPath = [](const std::string& root, const std::string& relative) {
        if (root.empty()) {
            return relative;
        }
        return (std::filesystem::path(root) / relative).lexically_normal().string();
    };

    const std::string fuselevelPath =
        joinPath(projectLoad.manifest.projectRoot, projectLoad.manifest.defaultWorld2D);
    result.loadedPath = fuselevelPath;

    const LegacySourceResolution moduleSource =
        resolveParityLegacySource(projectLoad.manifest, fuselevelPath, ".cs");
    result.sourceOrigin = moduleSource.origin;

    std::error_code ec;
    const bool fuselevelExists = std::filesystem::exists(fuselevelPath, ec);
    const bool canRefreshFromModule = moduleSource.origin != LegacySourceOrigin::Missing &&
                                      std::filesystem::exists(moduleSource.path, ec) &&
                                      (!fuselevelExists ||
                                       std::filesystem::last_write_time(moduleSource.path) >
                                           std::filesystem::last_write_time(fuselevelPath));
    if (!fuselevelExists || canRefreshFromModule) {
        if (moduleSource.origin == LegacySourceOrigin::Missing) {
            result.note = "missing .fuselevel and legacy .cs: " + fuselevelPath;
            return result;
        }

        const ConvertResult converted =
            convertT2DModuleToFuselevel(moduleSource.path, fuselevelPath);
        if (converted.status != ConvertStatus::Ok) {
            result.note = converted.note.empty() ? "T2D module convert failed" : converted.note;
            return result;
        }
        result.entityCount = converted.entityCount;
        result.wiringStubCount = converted.wiringStubCount;
    }

    result.ok = std::filesystem::exists(fuselevelPath, ec);
    if (result.ok) {
        if (result.entityCount == 0u) {
            populateFuselevelStats(fuselevelPath, result.entityCount, result.wiringStubCount);
        }
        result.note = moduleSource.note.empty() ? "2D world ready" : moduleSource.note;
    } else {
        result.note = "fuselevel still missing after convert attempt";
    }
    return result;
}

} // namespace fuse::project
