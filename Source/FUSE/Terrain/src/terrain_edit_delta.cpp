#include <fuse/terrain/terrain_edit_delta.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace fuse::terrain {
namespace {

constexpr usize kHeightEditStride = sizeof(u32) + sizeof(u32) + sizeof(f32);
constexpr usize kCaveEditStride = sizeof(f32) * 4u;

static_assert(sizeof(f32) == 4, "terrain edit delta expects a 32-bit float");
static_assert(sizeof(u32) == 4, "terrain edit delta expects a 32-bit u32");
static_assert(sizeof(u64) == 8, "terrain edit delta expects a 64-bit u64");

template <typename T>
void append_pod(std::vector<u8>& out, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "edit delta fields are plain values");
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

struct ByteReader {
    const u8* cur = nullptr;
    const u8* end = nullptr;

    [[nodiscard]] bool remaining(usize count) const { return static_cast<usize>(end - cur) >= count; }

    template <typename T>
    [[nodiscard]] bool read(T& value) {
        static_assert(std::is_trivially_copyable<T>::value, "edit delta fields are plain values");
        if (!remaining(sizeof(T))) {
            return false;
        }
        std::memcpy(&value, cur, sizeof(T));
        cur += sizeof(T);
        return true;
    }
};

[[nodiscard]] bool checked_count_bytes(u32 count, usize stride, usize& out) {
    if (stride != 0u && static_cast<usize>(count) > std::numeric_limits<usize>::max() / stride) {
        return false;
    }
    out = static_cast<usize>(count) * stride;
    return true;
}

[[nodiscard]] bool finite_vec3(vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool height_edit_less(const TerrainHeightEdit& a, const TerrainHeightEdit& b) {
    if (a.z != b.z) {
        return a.z < b.z;
    }
    return a.x < b.x;
}

} // namespace

std::vector<u8> serialize_terrain_edit_delta(const TerrainEditDelta& delta) {
    if ((delta.flags & ~kTerrainEditGeneratedFlag) != 0u || !std::isfinite(delta.world_size) ||
        !std::isfinite(delta.max_height)) {
        return {};
    }

    const u64 cell_count = static_cast<u64>(delta.resolution) * static_cast<u64>(delta.resolution);
    if (delta.heights.size() > std::numeric_limits<u32>::max() ||
        delta.caves.size() > std::numeric_limits<u32>::max() ||
        static_cast<u64>(delta.heights.size()) > cell_count) {
        return {};
    }

    for (usize i = 0; i < delta.heights.size(); ++i) {
        const TerrainHeightEdit& edit = delta.heights[i];
        if (edit.x >= delta.resolution || edit.z >= delta.resolution || !std::isfinite(edit.delta) ||
            std::fabs(edit.delta) <= kTerrainHeightEditEpsilon) {
            return {};
        }
        if (i > 0u && !height_edit_less(delta.heights[i - 1u], edit)) {
            return {};
        }
    }
    for (const TerrainCaveEdit& cave : delta.caves) {
        if (!(cave.radius > 0.f) || !std::isfinite(cave.radius) || !finite_vec3(cave.center)) {
            return {};
        }
    }

    usize height_bytes = 0;
    usize cave_bytes = 0;
    if (!checked_count_bytes(static_cast<u32>(delta.heights.size()), kHeightEditStride, height_bytes) ||
        !checked_count_bytes(static_cast<u32>(delta.caves.size()), kCaveEditStride, cave_bytes)) {
        return {};
    }

    std::vector<u8> out;
    out.reserve(44u + height_bytes + cave_bytes);
    append_pod(out, kTerrainEditDeltaMagic);
    append_pod(out, kTerrainEditDeltaVersion);
    append_pod(out, delta.seed);
    append_pod(out, delta.flags);
    append_pod(out, delta.resolution);
    append_pod(out, delta.world_size);
    append_pod(out, delta.max_height);
    append_pod(out, delta.svo_depth);
    append_pod(out, static_cast<u32>(delta.heights.size()));
    append_pod(out, static_cast<u32>(delta.caves.size()));
    for (const TerrainHeightEdit& edit : delta.heights) {
        append_pod(out, edit.x);
        append_pod(out, edit.z);
        append_pod(out, edit.delta);
    }
    for (const TerrainCaveEdit& cave : delta.caves) {
        append_pod(out, cave.center.x);
        append_pod(out, cave.center.y);
        append_pod(out, cave.center.z);
        append_pod(out, cave.radius);
    }
    return out;
}

bool deserialize_terrain_edit_delta(const u8* data, usize size, TerrainEditDelta& out) {
    if (data == nullptr || size == 0u) {
        return false;
    }

    ByteReader reader{data, data + size};
    u32 magic = 0;
    u32 version = 0;
    TerrainEditDelta decoded{};
    u32 height_count = 0;
    u32 cave_count = 0;
    if (!reader.read(magic) || magic != kTerrainEditDeltaMagic || !reader.read(version) ||
        version != kTerrainEditDeltaVersion || !reader.read(decoded.seed) || !reader.read(decoded.flags) ||
        !reader.read(decoded.resolution) || !reader.read(decoded.world_size) || !reader.read(decoded.max_height) ||
        !reader.read(decoded.svo_depth) || !reader.read(height_count) || !reader.read(cave_count)) {
        return false;
    }
    if ((decoded.flags & ~kTerrainEditGeneratedFlag) != 0u || !std::isfinite(decoded.world_size) ||
        !std::isfinite(decoded.max_height)) {
        return false;
    }

    const u64 cell_count = static_cast<u64>(decoded.resolution) * static_cast<u64>(decoded.resolution);
    if (static_cast<u64>(height_count) > cell_count) {
        return false;
    }

    usize height_bytes = 0;
    usize cave_bytes = 0;
    if (!checked_count_bytes(height_count, kHeightEditStride, height_bytes) ||
        !checked_count_bytes(cave_count, kCaveEditStride, cave_bytes)) {
        return false;
    }
    const usize payload = height_bytes + cave_bytes;
    if (payload < height_bytes || !reader.remaining(payload) ||
        static_cast<usize>(reader.end - reader.cur) != payload) {
        return false;
    }

    decoded.heights.reserve(height_count);
    for (u32 i = 0; i < height_count; ++i) {
        TerrainHeightEdit edit{};
        if (!reader.read(edit.x) || !reader.read(edit.z) || !reader.read(edit.delta)) {
            return false;
        }
        if (edit.x >= decoded.resolution || edit.z >= decoded.resolution || !std::isfinite(edit.delta) ||
            std::fabs(edit.delta) <= kTerrainHeightEditEpsilon) {
            return false;
        }
        if (!decoded.heights.empty() && !height_edit_less(decoded.heights.back(), edit)) {
            return false;
        }
        decoded.heights.push_back(edit);
    }

    decoded.caves.reserve(cave_count);
    for (u32 i = 0; i < cave_count; ++i) {
        TerrainCaveEdit cave{};
        if (!reader.read(cave.center.x) || !reader.read(cave.center.y) || !reader.read(cave.center.z) ||
            !reader.read(cave.radius)) {
            return false;
        }
        if (!(cave.radius > 0.f) || !std::isfinite(cave.radius) || !finite_vec3(cave.center)) {
            return false;
        }
        decoded.caves.push_back(cave);
    }

    if (reader.cur != reader.end) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

} // namespace fuse::terrain
