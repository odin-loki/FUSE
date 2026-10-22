#include <fuse/scene/serialiser.hpp>

#include <fuse/scene/scene_snapshot.hpp>

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/registry_serialiser.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fuse::scene {

namespace {

constexpr u32 kHeaderSize = 64u;
constexpr u32 kCameraBlockSize = 40u; // f32×9 + u32 active flag
constexpr u32 kTransformBlockSize = 40u; // f32×10 (position, rotation, scale)
static_assert(kTransformBlockSize == 10u * sizeof(f32), "transform block is ten f32 fields");
constexpr u8 kCameraMarker = 'C';
constexpr u8 kTransformTableMarker = 'T';
constexpr u8 kHierarchyTableMarker = 'H';
constexpr u8 kEcsBlockMarker = 'E';
// Header reserved[] byte indices.
constexpr usize kReservedCamera = 0;
constexpr usize kReservedTransforms = 1;
constexpr usize kReservedHierarchy = 2;
constexpr usize kReservedEcs = 3;
constexpr usize kReservedDimension = 4;

struct SceneHeader {
    u32 magic = SceneSerialiser::MAGIC;
    u32 version = SceneSerialiser::VERSION;
    u32 entityCount = 0;
    u32 archetypeCount = 0;
    u32 assetCount = 0;
    u32 svoPresent = 0;
    u8 reserved[40] = {};
};

void writeU32(std::vector<u8>& buffer, u32 value) {
    buffer.push_back(static_cast<u8>(value & 0xFFu));
    buffer.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    buffer.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    buffer.push_back(static_cast<u8>((value >> 24) & 0xFFu));
}

void writeF32(std::vector<u8>& buffer, float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(buffer, bits);
}

void writeString(std::vector<u8>& buffer, const std::string& text) {
    writeU32(buffer, static_cast<u32>(text.size()));
    buffer.insert(buffer.end(), text.begin(), text.end());
}

bool readU32(const u8*& cursor, const u8* end, u32& out) {
    if (cursor + 4 > end) {
        return false;
    }
    out = static_cast<u32>(cursor[0]) |
          (static_cast<u32>(cursor[1]) << 8) |
          (static_cast<u32>(cursor[2]) << 16) |
          (static_cast<u32>(cursor[3]) << 24);
    cursor += 4;
    return true;
}

bool readF32(const u8*& cursor, const u8* end, float& out) {
    u32 bits = 0;
    if (!readU32(cursor, end, bits)) {
        return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

bool readString(const u8*& cursor, const u8* end, std::string& out) {
    u32 length = 0;
    if (!readU32(cursor, end, length)) {
        return false;
    }
    if (cursor + length > end) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(cursor), length);
    cursor += length;
    return true;
}

void writeS32(std::vector<u8>& buffer, s32 value) {
    writeU32(buffer, static_cast<u32>(value));
}

bool readS32(const u8*& cursor, const u8* end, s32& out) {
    u32 bits = 0;
    if (!readU32(cursor, end, bits)) {
        return false;
    }
    out = static_cast<s32>(bits);
    return true;
}

bool sceneHasHierarchy(const Scene& scene) {
    for (const SceneEntity& entity : scene.entities()) {
        if (entity.parentIndex >= 0) {
            return true;
        }
    }
    return false;
}

void writeTransformBlock(std::vector<u8>& buffer, const SceneEntityTransform& transform) {
    writeF32(buffer, transform.positionX);
    writeF32(buffer, transform.positionY);
    writeF32(buffer, transform.positionZ);
    writeF32(buffer, transform.rotationX);
    writeF32(buffer, transform.rotationY);
    writeF32(buffer, transform.rotationZ);
    writeF32(buffer, transform.rotationW);
    writeF32(buffer, transform.scaleX);
    writeF32(buffer, transform.scaleY);
    writeF32(buffer, transform.scaleZ);
}

bool readTransformBlock(const u8*& cursor, const u8* end, SceneEntityTransform& transform) {
    return readF32(cursor, end, transform.positionX) && readF32(cursor, end, transform.positionY) &&
           readF32(cursor, end, transform.positionZ) && readF32(cursor, end, transform.rotationX) &&
           readF32(cursor, end, transform.rotationY) && readF32(cursor, end, transform.rotationZ) &&
           readF32(cursor, end, transform.rotationW) && readF32(cursor, end, transform.scaleX) &&
           readF32(cursor, end, transform.scaleY) && readF32(cursor, end, transform.scaleZ);
}

void writeCameraBlock(std::vector<u8>& buffer, const Camera& camera) {
    writeF32(buffer, camera.fovDeg);
    writeF32(buffer, camera.nearPlane);
    writeF32(buffer, camera.farPlane);
    writeF32(buffer, camera.aspectRatio);
    writeF32(buffer, camera.positionX);
    writeF32(buffer, camera.positionY);
    writeF32(buffer, camera.positionZ);
    writeF32(buffer, camera.yawDeg);
    writeF32(buffer, camera.pitchDeg);
    writeU32(buffer, camera.isActive ? 1u : 0u);
}

bool readCameraBlock(const u8*& cursor, const u8* end, Camera& camera) {
    if (!readF32(cursor, end, camera.fovDeg) ||
        !readF32(cursor, end, camera.nearPlane) ||
        !readF32(cursor, end, camera.farPlane) ||
        !readF32(cursor, end, camera.aspectRatio) ||
        !readF32(cursor, end, camera.positionX) ||
        !readF32(cursor, end, camera.positionY) ||
        !readF32(cursor, end, camera.positionZ) ||
        !readF32(cursor, end, camera.yawDeg) ||
        !readF32(cursor, end, camera.pitchDeg)) {
        return false;
    }

    u32 active = 0;
    if (!readU32(cursor, end, active)) {
        return false;
    }
    camera.isActive = active != 0u;
    return true;
}

bool writeFile(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool readFile(const std::string& path, std::vector<u8>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamsize size = input.tellg();
    if (size < 0) {
        return false;
    }
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return !input.fail();
}


/// Everything a `.fuselevel` holds, parsed but not yet applied.
struct ParsedScene {
    u32 version = 0;
    bool legacyMagic = false;
    SceneDimension dimension = SceneDimension::World3D;
    Camera camera;
    std::string name;
    std::vector<SceneEntity> entities;
    bool hasEcsBlock = false;
    std::vector<u8> ecsBytes;
};

SerialiseResult failWith(SerialiseStatus status, std::string message) {
    SerialiseResult result;
    result.status = status;
    result.error = std::move(message);
    return result;
}

void writeSceneBody(std::vector<u8>& buffer, const Scene& scene, bool writeHierarchy) {
    writeCameraBlock(buffer, scene.camera());
    writeString(buffer, scene.name());

    const u32 entityCount = scene.entityCount();
    writeU32(buffer, entityCount);
    for (const SceneEntity& entity : scene.entities()) {
        writeString(buffer, entity.name);
    }

    writeU32(buffer, entityCount);
    for (const SceneEntity& entity : scene.entities()) {
        writeTransformBlock(buffer, entity.transform);
    }

    if (writeHierarchy) {
        writeU32(buffer, entityCount);
        for (const SceneEntity& entity : scene.entities()) {
            writeS32(buffer, entity.parentIndex);
        }
    }
}

/// `readEcs` = false skips the v3 ECS block (still bounds-checked).
SerialiseResult parseScene(const std::vector<u8>& buffer, bool readEcs, ParsedScene& out) {
    if (buffer.size() < kHeaderSize) {
        return failWith(SerialiseStatus::TruncatedFile, "scene file too small");
    }

    SceneHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));

    // Compat loader: pre-rename 'ENGC' files share the v1/v2 layout, only the magic differs.
    out.legacyMagic = header.magic == SceneSerialiser::LEGACY_MAGIC_ENGC;
    if (header.magic != SceneSerialiser::MAGIC && !out.legacyMagic) {
        return failWith(SerialiseStatus::InvalidMagic, "invalid scene magic");
    }

    if (header.version != SceneSerialiser::VERSION && header.version != SceneSerialiser::VERSION_HIERARCHY &&
        header.version != SceneSerialiser::VERSION_ECS) {
        return failWith(SerialiseStatus::UnsupportedVersion, "unsupported scene version");
    }
    out.version = header.version;

    const bool hasHierarchyTable =
        header.version >= SceneSerialiser::VERSION_HIERARCHY && header.reserved[kReservedHierarchy] == kHierarchyTableMarker;
    const bool hasEcsBlock = header.version == SceneSerialiser::VERSION_ECS;
    if (hasEcsBlock) {
        if (header.reserved[kReservedEcs] != kEcsBlockMarker) {
            return failWith(SerialiseStatus::TruncatedFile, "missing ECS block marker");
        }
        const u8 dimension = header.reserved[kReservedDimension];
        if (dimension > static_cast<u8>(SceneDimension::World2D)) {
            return failWith(SerialiseStatus::UnsupportedVersion, "unknown scene dimension");
        }
        out.dimension = static_cast<SceneDimension>(dimension);
    }

    if (buffer.size() < kHeaderSize + kCameraBlockSize) {
        return failWith(SerialiseStatus::TruncatedFile, "scene file missing camera block");
    }

    if (header.reserved[kReservedCamera] != kCameraMarker) {
        return failWith(SerialiseStatus::TruncatedFile, "missing camera block marker");
    }

    const bool hasTransformTable = header.reserved[kReservedTransforms] == kTransformTableMarker;

    const u8* cursor = buffer.data() + kHeaderSize;
    const u8* end = buffer.data() + buffer.size();

    if (!readCameraBlock(cursor, end, out.camera)) {
        return failWith(SerialiseStatus::TruncatedFile, "truncated camera block");
    }

    if (!readString(cursor, end, out.name)) {
        return failWith(SerialiseStatus::TruncatedFile, "truncated scene name");
    }

    u32 objectCount = 0;
    if (!readU32(cursor, end, objectCount)) {
        return failWith(SerialiseStatus::TruncatedFile, "truncated object table");
    }

    if (objectCount != header.entityCount) {
        return failWith(SerialiseStatus::TruncatedFile, "entity count mismatch");
    }
    // Every entity needs at least its 4-byte name length: reject absurd counts before reserving.
    if (objectCount > static_cast<usize>(end - cursor) / 4u) {
        return failWith(SerialiseStatus::TruncatedFile, "truncated object table");
    }

    out.entities.clear();
    out.entities.reserve(objectCount);

    for (u32 i = 0; i < objectCount; ++i) {
        std::string objectName;
        if (!readString(cursor, end, objectName)) {
            return failWith(SerialiseStatus::TruncatedFile, "truncated object name");
        }

        SceneEntity entity;
        entity.name = std::move(objectName);
        out.entities.push_back(std::move(entity));
    }

    if (hasTransformTable) {
        u32 transformCount = 0;
        if (!readU32(cursor, end, transformCount)) {
            return failWith(SerialiseStatus::TruncatedFile, "truncated transform table");
        }

        if (transformCount != objectCount) {
            return failWith(SerialiseStatus::TruncatedFile, "transform count mismatch");
        }

        for (u32 i = 0; i < transformCount; ++i) {
            if (!readTransformBlock(cursor, end, out.entities[i].transform)) {
                return failWith(SerialiseStatus::TruncatedFile, "truncated entity transform");
            }
        }
    }

    if (hasHierarchyTable) {
        u32 parentCount = 0;
        if (!readU32(cursor, end, parentCount)) {
            return failWith(SerialiseStatus::TruncatedFile, "truncated hierarchy table");
        }

        if (parentCount != objectCount) {
            return failWith(SerialiseStatus::TruncatedFile, "hierarchy count mismatch");
        }

        for (u32 i = 0; i < parentCount; ++i) {
            if (!readS32(cursor, end, out.entities[i].parentIndex)) {
                return failWith(SerialiseStatus::TruncatedFile, "truncated entity parent index");
            }
        }
    }

    out.hasEcsBlock = hasEcsBlock;
    if (hasEcsBlock) {
        u32 ecsBytes = 0;
        if (!readU32(cursor, end, ecsBytes)) {
            return failWith(SerialiseStatus::TruncatedFile, "truncated ECS block size");
        }
        if (static_cast<usize>(end - cursor) < ecsBytes) {
            return failWith(SerialiseStatus::TruncatedFile, "truncated ECS block");
        }
        if (readEcs) {
            out.ecsBytes.assign(cursor, cursor + ecsBytes);
        }
        cursor += ecsBytes;
    }

    return SerialiseResult{SerialiseStatus::Ok, {}, out.legacyMagic};
}

Scene buildScene(ParsedScene& parsed) {
    Scene loaded(std::move(parsed.name));
    loaded.camera() = parsed.camera;
    loaded.clearEntities();
    for (SceneEntity& entity : parsed.entities) {
        loaded.addEntity(std::move(entity.name), entity.transform, entity.parentIndex);
    }
    loaded.camera().update();
    return loaded;
}

/// Unique scratch path for handing an ECS block to RegistrySerialiser (which reads / writes files).
std::filesystem::path scratchEcsPath(const std::string& nearPath) {
    static std::atomic<u64> counter{0};
    const u64 serial = counter.fetch_add(1u, std::memory_order_relaxed);
    const u64 ticks = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string leaf = "fuse_scene_ecs_" + std::to_string(ticks) + "_" + std::to_string(serial) + ".fecs";
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec || dir.empty()) {
        dir = std::filesystem::path(nearPath).parent_path();
    }
    return dir / leaf;
}

SerialiseResult registryToBytes(const ecs::Registry& registry, const std::string& scenePath, std::vector<u8>& out) {
    const std::filesystem::path scratch = scratchEcsPath(scenePath);
    const ecs::RegistrySerialiseResult saved = ecs::RegistrySerialiser::save(registry, scratch.string());
    std::error_code ec;
    if (!saved.ok) {
        std::filesystem::remove(scratch, ec);
        return failWith(SerialiseStatus::IoError, "ECS block: " + saved.error);
    }
    const bool read = readFile(scratch.string(), out);
    std::filesystem::remove(scratch, ec);
    if (!read) {
        return failWith(SerialiseStatus::IoError, "ECS block: unable to read back " + scratch.string());
    }
    return SerialiseResult{SerialiseStatus::Ok, {}, false};
}

SerialiseResult registryFromBytes(const std::vector<u8>& bytes, const std::string& scenePath, ecs::Registry& out,
                                  ecs::RegistrySerialiseResult& loaded) {
    const std::filesystem::path scratch = scratchEcsPath(scenePath);
    std::error_code ec;
    if (!writeFile(scratch.string(), bytes)) {
        std::filesystem::remove(scratch, ec);
        return failWith(SerialiseStatus::IoError, "ECS block: unable to stage " + scratch.string());
    }
    loaded = ecs::RegistrySerialiser::load(scratch.string(), out);
    std::filesystem::remove(scratch, ec);
    if (!loaded.ok) {
        return failWith(SerialiseStatus::TruncatedFile, "ECS block: " + loaded.error);
    }
    return SerialiseResult{SerialiseStatus::Ok, {}, false};
}

/// v1/v2 compat: one Transform entity per scene entity, parents mapped by index.
void registryFromSceneEntities(const Scene& scene, ecs::Registry& registry) {
    registry.init();
    std::vector<ecs::EntityID> ids;
    ids.reserve(scene.entityCount());
    for (const SceneEntity& entity : scene.entities()) {
        const ecs::EntityID id = registry.create();
        ecs::Transform transform{};
        const SceneEntityTransform& t = entity.transform;
        transform.position = {t.positionX, t.positionY, t.positionZ, 1.f};
        transform.rotation = {t.rotationX, t.rotationY, t.rotationZ, t.rotationW};
        transform.scale = {t.scaleX, t.scaleY, t.scaleZ, 0.f};
        registry.add(id, transform);
        ids.push_back(id);
    }
    for (usize i = 0; i < ids.size(); ++i) {
        const s32 parent = scene.entities()[i].parentIndex;
        if (parent >= 0 && static_cast<usize>(parent) < ids.size() && static_cast<usize>(parent) != i) {
            registry.get<ecs::Transform>(ids[i])->parent = ids[static_cast<usize>(parent)];
        }
    }
}

} // namespace

SerialiseResult SceneSerialiser::save(const Scene& scene, const std::string& path) {
    SerialiseResult result;

    const bool writeHierarchy = sceneHasHierarchy(scene);

    SceneHeader header;
    header.entityCount = scene.entityCount();
    header.version = writeHierarchy ? VERSION_HIERARCHY : VERSION;
    header.reserved[kReservedCamera] = kCameraMarker;
    header.reserved[kReservedTransforms] = kTransformTableMarker;
    if (writeHierarchy) {
        header.reserved[kReservedHierarchy] = kHierarchyTableMarker;
    }

    std::vector<u8> buffer;
    buffer.resize(kHeaderSize, 0);
    std::memcpy(buffer.data(), &header, sizeof(header));
    writeSceneBody(buffer, scene, writeHierarchy);

    if (!writeFile(path, buffer)) {
        result.status = SerialiseStatus::IoError;
        result.error = "unable to write scene file: " + path;
        return result;
    }

    result.status = SerialiseStatus::Ok;
    return result;
}

SerialiseResult SceneSerialiser::saveWithRegistry(const Scene& scene, const ecs::Registry& registry,
                                                  const std::string& path, SceneDimension dimension) {
    std::vector<u8> ecsBytes;
    const SerialiseResult ecsResult = registryToBytes(registry, path, ecsBytes);
    if (ecsResult.status != SerialiseStatus::Ok) {
        return ecsResult;
    }

    SceneHeader header;
    header.entityCount = scene.entityCount();
    header.version = VERSION_ECS;
    header.reserved[kReservedCamera] = kCameraMarker;
    header.reserved[kReservedTransforms] = kTransformTableMarker;
    header.reserved[kReservedHierarchy] = kHierarchyTableMarker;
    header.reserved[kReservedEcs] = kEcsBlockMarker;
    header.reserved[kReservedDimension] = static_cast<u8>(dimension);

    std::vector<u8> buffer;
    buffer.resize(kHeaderSize, 0);
    std::memcpy(buffer.data(), &header, sizeof(header));
    writeSceneBody(buffer, scene, true);
    writeU32(buffer, static_cast<u32>(ecsBytes.size()));
    buffer.insert(buffer.end(), ecsBytes.begin(), ecsBytes.end());

    // Write next to the target, then swap in: a failed save never leaves a half-written level.
    const std::string temp = path + ".tmp";
    if (!writeFile(temp, buffer)) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return failWith(SerialiseStatus::IoError, "unable to write scene file: " + path);
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::filesystem::remove(temp, removeEc);
            return failWith(SerialiseStatus::IoError, "unable to replace scene file: " + path);
        }
    }
    return SerialiseResult{SerialiseStatus::Ok, {}, false};
}

SerialiseResult SceneSerialiser::load(const std::string& path, Scene& scene) {
    std::vector<u8> buffer;
    if (!readFile(path, buffer)) {
        return failWith(SerialiseStatus::IoError, "unable to read scene file: " + path);
    }

    ParsedScene parsed;
    const SerialiseResult result = parseScene(buffer, false, parsed);
    if (result.status != SerialiseStatus::Ok) {
        return result;
    }

    scene = buildScene(parsed);
    return result;
}

SerialiseResult SceneSerialiser::loadWithRegistry(const std::string& path, Scene& scene, ecs::Registry& registry,
                                                  SceneFileInfo* info) {
    std::vector<u8> buffer;
    if (!readFile(path, buffer)) {
        return failWith(SerialiseStatus::IoError, "unable to read scene file: " + path);
    }

    ParsedScene parsed;
    const SerialiseResult result = parseScene(buffer, true, parsed);
    if (result.status != SerialiseStatus::Ok) {
        return result;
    }

    Scene loaded = buildScene(parsed);
    ecs::Registry loadedRegistry;
    usize archetypes = 0;
    usize ecsEntities = 0;
    if (parsed.hasEcsBlock) {
        ecs::RegistrySerialiseResult ecsLoad;
        const SerialiseResult ecsResult = registryFromBytes(parsed.ecsBytes, path, loadedRegistry, ecsLoad);
        if (ecsResult.status != SerialiseStatus::Ok) {
            return ecsResult;
        }
        archetypes = ecsLoad.archetypeCount;
        ecsEntities = ecsLoad.entityCount;
    } else {
        registryFromSceneEntities(loaded, loadedRegistry);
        ecsEntities = loaded.entityCount();
        archetypes = ecsEntities > 0u ? 1u : 0u;
    }

    scene = std::move(loaded);
    registry = std::move(loadedRegistry);
    if (info != nullptr) {
        info->version = parsed.version;
        info->dimension = parsed.dimension;
        info->hasEcsBlock = parsed.hasEcsBlock;
        info->ecsEntityCount = ecsEntities;
        info->ecsArchetypeCount = archetypes;
    }
    return result;
}

} // namespace fuse::scene
