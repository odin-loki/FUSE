#include <fuse/scene/serialiser.hpp>

#include <cstring>
#include <fstream>
#include <vector>

namespace fuse::scene {

namespace {

constexpr u32 kHeaderSize = 64u;
constexpr u32 kCameraBlockSize = 40u; // f32×9 + u32 active flag

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

} // namespace

SerialiseResult SceneSerialiser::save(const Scene& scene, const std::string& path) {
    SerialiseResult result;

    SceneHeader header;
    header.entityCount = scene.objectCount();
    header.reserved[0] = 'C'; // camera block follows header in this stub

    std::vector<u8> buffer;
    buffer.resize(kHeaderSize, 0);
    std::memcpy(buffer.data(), &header, sizeof(header));

    writeCameraBlock(buffer, scene.camera());
    writeString(buffer, scene.name());

    const u32 objectCount = scene.objectCount();
    writeU32(buffer, objectCount);
    for (const std::string& objectName : scene.objectNames()) {
        writeString(buffer, objectName);
    }

    if (!writeFile(path, buffer)) {
        result.status = SerialiseStatus::IoError;
        result.error = "unable to write scene file: " + path;
        return result;
    }

    result.status = SerialiseStatus::Ok;
    return result;
}

SerialiseResult SceneSerialiser::load(const std::string& path, Scene& scene) {
    SerialiseResult result;

    std::vector<u8> buffer;
    if (!readFile(path, buffer)) {
        result.status = SerialiseStatus::IoError;
        result.error = "unable to read scene file: " + path;
        return result;
    }

    if (buffer.size() < kHeaderSize) {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "scene file too small";
        return result;
    }

    SceneHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));

    if (header.magic != MAGIC) {
        result.status = SerialiseStatus::InvalidMagic;
        result.error = "invalid scene magic";
        return result;
    }

    if (header.version != VERSION) {
        result.status = SerialiseStatus::UnsupportedVersion;
        result.error = "unsupported scene version";
        return result;
    }

    if (buffer.size() < kHeaderSize + kCameraBlockSize) {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "scene file missing camera block";
        return result;
    }

    if (header.reserved[0] != 'C') {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "missing camera block marker";
        return result;
    }

    const u8* cursor = buffer.data() + kHeaderSize;
    const u8* end = buffer.data() + buffer.size();

    Camera camera;
    if (!readCameraBlock(cursor, end, camera)) {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "truncated camera block";
        return result;
    }

    std::string sceneName;
    if (!readString(cursor, end, sceneName)) {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "truncated scene name";
        return result;
    }

    u32 objectCount = 0;
    if (!readU32(cursor, end, objectCount)) {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "truncated object table";
        return result;
    }

    if (objectCount != header.entityCount) {
        result.status = SerialiseStatus::TruncatedFile;
        result.error = "entity count mismatch";
        return result;
    }

    Scene loaded(std::move(sceneName));
    loaded.camera() = camera;
    loaded.clearObjects();

    for (u32 i = 0; i < objectCount; ++i) {
        std::string objectName;
        if (!readString(cursor, end, objectName)) {
            result.status = SerialiseStatus::TruncatedFile;
            result.error = "truncated object name";
            return result;
        }
        loaded.addObjectName(std::move(objectName));
    }

    scene = std::move(loaded);
    scene.camera().update();
    result.status = SerialiseStatus::Ok;
    return result;
}

} // namespace fuse::scene
