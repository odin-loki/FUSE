#include <fuse/net/snapshot_delta.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::net {

// Row types shared by the anonymous-namespace helpers and SnapshotDeltaWorkspace::Impl (a named
// namespace, so the workspace may hold them without anonymous-namespace field types).
namespace delta_detail {

struct EntityComponents {
    ecs::vec3 position{};
    ecs::quat rotation{};
    ecs::vec3 scale{1.f, 1.f, 1.f, 0.f};
    ecs::vec3 linear_velocity{};
    ecs::vec3 angular_velocity{};
    f32 mass = 1.f;
};

/// One parsed snapshot row: the entity's components plus which streams (ECS / physics) carry it.
struct EntityRecord {
    u32 index = 0;
    u32 generation = 0;
    EntityComponents components{};
    bool has_ecs = false;
    bool has_physics = false;
};

/// Open-addressing (entity key -> record slot) table; rebuilt per parse, storage reused.
struct RecordIndex {
    std::vector<u64> keys;
    std::vector<u32> slots;
    std::vector<u8> used;
    usize mask = 0;

    void reset(usize expected) {
        usize capacity = 16;
        while (capacity < expected * 2u) {
            capacity <<= 1u;
        }
        keys.resize(capacity);
        slots.resize(capacity);
        used.assign(capacity, 0u);
        mask = capacity - 1u;
    }

    static usize hash(u64 key) {
        key ^= key >> 33u;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 33u;
        return static_cast<usize>(key);
    }

    /// Slot of `key`, or ~0u when absent.
    [[nodiscard]] u32 find(u64 key) const {
        for (usize i = hash(key) & mask;; i = (i + 1u) & mask) {
            if (used[i] == 0u) {
                return ~0u;
            }
            if (keys[i] == key) {
                return slots[i];
            }
        }
    }

    /// Caller guarantees `key` is absent and the table has room (reset() sized it).
    void insert(u64 key, u32 slot) {
        for (usize i = hash(key) & mask;; i = (i + 1u) & mask) {
            if (used[i] == 0u) {
                used[i] = 1u;
                keys[i] = key;
                slots[i] = slot;
                return;
            }
        }
    }
};

} // namespace delta_detail

struct SnapshotDeltaWorkspace::Impl {
    std::vector<delta_detail::EntityRecord> base_records;
    std::vector<delta_detail::EntityRecord> target_records;
    std::vector<delta_detail::EntityRecord> apply_records;
    delta_detail::RecordIndex base_index;
    delta_detail::RecordIndex target_index;
    delta_detail::RecordIndex apply_index;
    /// Patch rows (with their byte buffers) parked while a delta carries fewer patches.
    std::vector<SnapshotEntityPatch> spare_patches;
    /// compute_snapshot_delta's reconstruction check target.
    GameSnapshot rebuilt;
};

SnapshotDeltaWorkspace::SnapshotDeltaWorkspace() : m_impl(std::make_unique<Impl>()) {}
SnapshotDeltaWorkspace::~SnapshotDeltaWorkspace() = default;
SnapshotDeltaWorkspace::SnapshotDeltaWorkspace(SnapshotDeltaWorkspace&&) noexcept = default;
SnapshotDeltaWorkspace& SnapshotDeltaWorkspace::operator=(SnapshotDeltaWorkspace&&) noexcept = default;

void SnapshotDeltaWorkspace::reserve(usize entity_rows) {
    Impl& ws = *m_impl;
    ws.base_records.reserve(entity_rows);
    ws.target_records.reserve(entity_rows);
    ws.apply_records.reserve(entity_rows * 2u);
    ws.base_index.reset(entity_rows);
    ws.target_index.reset(entity_rows);
    ws.apply_index.reset(entity_rows * 2u);
    ws.spare_patches.reserve(entity_rows);
}

namespace {

using delta_detail::EntityComponents;
using delta_detail::EntityRecord;
using delta_detail::RecordIndex;

constexpr usize kEcsRowBytes = 8u + 12u + 16u + 12u;
constexpr usize kPhysicsRowBytes = 8u + 12u + 12u + 4u;

/// Non-owning little reader over a byte span with NetSerializer's truncation semantics: reads
/// past the end yield zero bytes (and still advance), so a truncated stream ends parsing.
class ByteReader {
public:
    ByteReader(const byte* data, usize size) : m_data(data), m_size(size) {}
    explicit ByteReader(const std::vector<byte>& bytes) : ByteReader(bytes.data(), bytes.size()) {}

    [[nodiscard]] bool complete() const { return m_pos >= m_size; }

    void read(void* out, usize n) {
        auto* dst = static_cast<byte*>(out);
        usize available = m_pos < m_size ? m_size - m_pos : 0u;
        if (available > n) {
            available = n;
        }
        if (available > 0u) {
            std::memcpy(dst, m_data + m_pos, available);
        }
        if (available < n) {
            std::memset(dst + available, 0, n - available);
        }
        m_pos += n;
    }
    u32 u32v() {
        u32 v = 0;
        read(&v, sizeof(v));
        return v;
    }
    f32 f32v() {
        f32 v = 0.f;
        read(&v, sizeof(v));
        return v;
    }
    ecs::vec3 vec3() {
        ecs::vec3 v{};
        v.x = f32v();
        v.y = f32v();
        v.z = f32v();
        return v;
    }
    ecs::quat quat() {
        ecs::quat q{};
        q.x = f32v();
        q.y = f32v();
        q.z = f32v();
        q.w = f32v();
        return q;
    }

private:
    const byte* m_data = nullptr;
    usize m_size = 0;
    usize m_pos = 0;
};

// Appends reuse the vector's capacity: steady-state encoding does not allocate.
void append_raw(std::vector<byte>& out, const void* data, usize n) {
    const usize offset = out.size();
    out.resize(offset + n);
    std::memcpy(out.data() + offset, data, n);
}
void append_u32(std::vector<byte>& out, u32 v) { append_raw(out, &v, sizeof(v)); }
void append_f32(std::vector<byte>& out, f32 v) { append_raw(out, &v, sizeof(v)); }
void append_vec3(std::vector<byte>& out, const ecs::vec3& v) {
    append_f32(out, v.x);
    append_f32(out, v.y);
    append_f32(out, v.z);
}
void append_quat(std::vector<byte>& out, const ecs::quat& q) {
    append_f32(out, q.x);
    append_f32(out, q.y);
    append_f32(out, q.z);
    append_f32(out, q.w);
}

void read_bytes(NetSerializer& in, std::vector<byte>& out, u32 size) {
    // Untrusted wire length: never allocate past the bytes actually present.
    if (static_cast<usize>(size) > in.bytes_remaining()) {
        size = static_cast<u32>(in.bytes_remaining());
    }
    out.resize(size);
    for (u32 i = 0; i < size; ++i) {
        out[i] = in.read_u8();
    }
}

void serialize_masked_ecs(u8 mask, const EntityComponents& components, std::vector<byte>& out) {
    out.clear();
    if ((mask & static_cast<u8>(SnapshotEcsField::Position)) != 0) {
        append_vec3(out, components.position);
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Rotation)) != 0) {
        append_quat(out, components.rotation);
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Scale)) != 0) {
        append_vec3(out, components.scale);
    }
}

void serialize_masked_physics(u8 mask, const EntityComponents& components, std::vector<byte>& out) {
    out.clear();
    if ((mask & static_cast<u8>(SnapshotPhysicsField::LinearVelocity)) != 0) {
        append_vec3(out, components.linear_velocity);
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::AngularVelocity)) != 0) {
        append_vec3(out, components.angular_velocity);
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::Mass)) != 0) {
        append_f32(out, components.mass);
    }
}

EntityComponents decode_masked_ecs(u8 mask, const std::vector<byte>& ecs_bytes) {
    EntityComponents components;
    if (ecs_bytes.empty() || mask == 0) {
        return components;
    }

    ByteReader reader(ecs_bytes);
    if ((mask & static_cast<u8>(SnapshotEcsField::Position)) != 0) {
        components.position = reader.vec3();
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Rotation)) != 0) {
        components.rotation = reader.quat();
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Scale)) != 0) {
        components.scale = reader.vec3();
    }
    return components;
}

EntityComponents decode_masked_physics(u8 mask, const std::vector<byte>& physics_bytes) {
    EntityComponents components;
    if (physics_bytes.empty() || mask == 0) {
        return components;
    }

    ByteReader reader(physics_bytes);
    if ((mask & static_cast<u8>(SnapshotPhysicsField::LinearVelocity)) != 0) {
        components.linear_velocity = reader.vec3();
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::AngularVelocity)) != 0) {
        components.angular_velocity = reader.vec3();
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::Mass)) != 0) {
        components.mass = reader.f32v();
    }
    return components;
}

// Bitwise comparisons: deltas must reproduce the target bytes exactly (-0.0 vs +0.0, NaN payloads).
bool f32_bits_equal(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof(f32)) == 0; }

bool vec3_equal(const ecs::vec3& a, const ecs::vec3& b) {
    return f32_bits_equal(a.x, b.x) && f32_bits_equal(a.y, b.y) && f32_bits_equal(a.z, b.z);
}

bool quat_equal(const ecs::quat& a, const ecs::quat& b) {
    return f32_bits_equal(a.x, b.x) && f32_bits_equal(a.y, b.y) && f32_bits_equal(a.z, b.z) &&
           f32_bits_equal(a.w, b.w);
}

u8 compute_ecs_field_mask(const EntityComponents& base, const EntityComponents& target) {
    u8 mask = 0;
    if (!vec3_equal(base.position, target.position)) {
        mask |= static_cast<u8>(SnapshotEcsField::Position);
    }
    if (!quat_equal(base.rotation, target.rotation)) {
        mask |= static_cast<u8>(SnapshotEcsField::Rotation);
    }
    if (!vec3_equal(base.scale, target.scale)) {
        mask |= static_cast<u8>(SnapshotEcsField::Scale);
    }
    return mask;
}

u8 compute_physics_field_mask(const EntityComponents& base, const EntityComponents& target) {
    u8 mask = 0;
    if (!vec3_equal(base.linear_velocity, target.linear_velocity)) {
        mask |= static_cast<u8>(SnapshotPhysicsField::LinearVelocity);
    }
    if (!vec3_equal(base.angular_velocity, target.angular_velocity)) {
        mask |= static_cast<u8>(SnapshotPhysicsField::AngularVelocity);
    }
    if (!f32_bits_equal(base.mass, target.mass)) {
        mask |= static_cast<u8>(SnapshotPhysicsField::Mass);
    }
    return mask;
}

void merge_masked_components(EntityComponents& base, u8 ecs_mask, u8 physics_mask, const EntityComponents& patch) {
    if ((ecs_mask & static_cast<u8>(SnapshotEcsField::Position)) != 0) {
        base.position = patch.position;
    }
    if ((ecs_mask & static_cast<u8>(SnapshotEcsField::Rotation)) != 0) {
        base.rotation = patch.rotation;
    }
    if ((ecs_mask & static_cast<u8>(SnapshotEcsField::Scale)) != 0) {
        base.scale = patch.scale;
    }
    if ((physics_mask & static_cast<u8>(SnapshotPhysicsField::LinearVelocity)) != 0) {
        base.linear_velocity = patch.linear_velocity;
    }
    if ((physics_mask & static_cast<u8>(SnapshotPhysicsField::AngularVelocity)) != 0) {
        base.angular_velocity = patch.angular_velocity;
    }
    if ((physics_mask & static_cast<u8>(SnapshotPhysicsField::Mass)) != 0) {
        base.mass = patch.mass;
    }
}

/// Parses snapshot rows preserving stream order (ECS rows first, then physics-only rows), so a
/// reconstruction re-emits rows in the same order as the snapshot they came from. A repeated ECS
/// row replaces the earlier one in place. `extra_rows` reserves index room for rows added later.
void parse_entity_records(const GameSnapshot& snapshot, std::vector<EntityRecord>& ordered, RecordIndex& index,
                          usize extra_rows = 0) {
    ordered.clear();
    index.reset(snapshot.ecs_state.size() / kEcsRowBytes + snapshot.physics_state.size() / kPhysicsRowBytes + 2u +
                extra_rows);

    ByteReader ecs_reader(snapshot.ecs_state);
    while (!ecs_reader.complete()) {
        EntityRecord record;
        record.index = ecs_reader.u32v();
        record.generation = ecs_reader.u32v();
        record.components.position = ecs_reader.vec3();
        record.components.rotation = ecs_reader.quat();
        record.components.scale = ecs_reader.vec3();
        record.has_ecs = true;

        const u64 key = (static_cast<u64>(record.generation) << 32) | record.index;
        const u32 slot = index.find(key);
        if (slot != ~0u) {
            ordered[slot] = record;
        } else {
            index.insert(key, static_cast<u32>(ordered.size()));
            ordered.push_back(record);
        }
    }

    ByteReader physics_reader(snapshot.physics_state);
    while (!physics_reader.complete()) {
        const u32 entity_index = physics_reader.u32v();
        const u32 generation = physics_reader.u32v();
        const ecs::vec3 linear_velocity = physics_reader.vec3();
        const ecs::vec3 angular_velocity = physics_reader.vec3();
        const f32 mass = physics_reader.f32v();

        const u64 key = (static_cast<u64>(generation) << 32) | entity_index;
        u32 slot = index.find(key);
        if (slot == ~0u) {
            slot = static_cast<u32>(ordered.size());
            index.insert(key, slot);
            ordered.emplace_back();
        }
        EntityRecord& record = ordered[slot];
        record.index = entity_index;
        record.generation = generation;
        record.components.linear_velocity = linear_velocity;
        record.components.angular_velocity = angular_velocity;
        record.components.mass = mass;
        record.has_physics = true;
    }
}

void append_entity_record(std::vector<byte>& ecs_out, std::vector<byte>& physics_out, const EntityRecord& record) {
    if (record.has_ecs) {
        append_u32(ecs_out, record.index);
        append_u32(ecs_out, record.generation);
        append_vec3(ecs_out, record.components.position);
        append_quat(ecs_out, record.components.rotation);
        append_vec3(ecs_out, record.components.scale);
    }

    if (record.has_physics) {
        append_u32(physics_out, record.index);
        append_u32(physics_out, record.generation);
        append_vec3(physics_out, record.components.linear_velocity);
        append_vec3(physics_out, record.components.angular_velocity);
        append_f32(physics_out, record.components.mass);
    }
}

/// Resizes `delta.entity_patches` to `count`, parking surplus rows (and their byte buffers) in
/// the workspace and reusing parked rows when growing, so patch buffers keep their capacity.
void set_patch_count(SnapshotDelta& delta, usize count, std::vector<SnapshotEntityPatch>& spare) {
    std::vector<SnapshotEntityPatch>& patches = delta.entity_patches;
    while (patches.size() > count) {
        spare.push_back(std::move(patches.back()));
        patches.pop_back();
    }
    while (patches.size() < count) {
        if (!spare.empty()) {
            patches.push_back(std::move(spare.back()));
            spare.pop_back();
        } else {
            patches.emplace_back();
        }
    }
}

u64 entity_mask_bit(u32 entity_index) {
    if (entity_index >= 64) {
        return 0;
    }
    return 1ull << entity_index;
}

u32 popcount_u64(u64 value) {
    u32 count = 0;
    while (value != 0) {
        count += static_cast<u32>(value & 1ull);
        value >>= 1;
    }
    return count;
}

u32 popcount_u8(u8 value) {
    u32 count = 0;
    while (value != 0) {
        count += static_cast<u32>(value & 1u);
        value >>= 1;
    }
    return count;
}

bool entity_index_trackable(u32 entity_index) {
    return entity_index < 64;
}

/// Incremental FNV-1a 64 (same result as fnv1a64_bytes over the concatenated bytes).
struct FnvStream {
    u64 hash = 14695981039346656037ull;
    void bytes(const byte* data, usize size) {
        for (usize i = 0; i < size; ++i) {
            hash ^= static_cast<u64>(data[i]);
            hash *= 1099511628211ull;
        }
    }
    void u8v(u8 v) { bytes(&v, 1u); }
    void u32v(u32 v) {
        byte raw[4];
        std::memcpy(raw, &v, sizeof(v));
        bytes(raw, sizeof(raw));
    }
};

} // namespace

bool snapshots_equivalent(const GameSnapshot& base, const GameSnapshot& target) {
    return base.frame == target.frame && base.checksum == target.checksum && base.ecs_state == target.ecs_state &&
           base.physics_state == target.physics_state;
}

bool ecs_field_mask_contains(u8 mask, SnapshotEcsField field) {
    return (mask & static_cast<u8>(field)) == static_cast<u8>(field);
}

bool physics_field_mask_contains(u8 mask, SnapshotPhysicsField field) {
    return (mask & static_cast<u8>(field)) == static_cast<u8>(field);
}

bool ecs_field_mask_valid(u8 mask) {
    return (mask & ~static_cast<u8>(SnapshotEcsField::All)) == 0;
}

bool physics_field_mask_valid(u8 mask) {
    return (mask & ~static_cast<u8>(SnapshotPhysicsField::All)) == 0;
}

u8 ecs_field_mask_sanitize(u8 mask) {
    return mask & static_cast<u8>(SnapshotEcsField::All);
}

u8 physics_field_mask_sanitize(u8 mask) {
    return mask & static_cast<u8>(SnapshotPhysicsField::All);
}

bool snapshot_delta_kind_valid(SnapshotDeltaKind kind) {
    switch (kind) {
    case SnapshotDeltaKind::None:
    case SnapshotDeltaKind::Full:
    case SnapshotDeltaKind::EntityPatch:
        return true;
    }
    return false;
}

u8 ecs_field_mask_union(u8 a, u8 b) {
    return a | b;
}

u8 physics_field_mask_union(u8 a, u8 b) {
    return a | b;
}

u32 ecs_field_mask_count(u8 mask) {
    return popcount_u8(mask);
}

u32 physics_field_mask_count(u8 mask) {
    return popcount_u8(mask);
}

bool entity_index_in_changed_mask(u64 changed_entity_mask, u32 entity_index) {
    const u64 bit = entity_mask_bit(entity_index);
    return bit != 0 && (changed_entity_mask & bit) != 0;
}

u32 count_changed_entities_in_mask(u64 changed_entity_mask) {
    return popcount_u64(changed_entity_mask);
}

bool validate_changed_entity_mask(const SnapshotDelta& delta) {
    switch (delta.kind) {
    case SnapshotDeltaKind::None:
    case SnapshotDeltaKind::Full:
        return delta.changed_entity_mask == 0;
    case SnapshotDeltaKind::EntityPatch: {
        u64 expected_mask = 0;
        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            const u64 bit = entity_mask_bit(patch.entity_index);
            if (bit == 0) {
                return false;
            }
            expected_mask |= bit;
        }
        return delta.changed_entity_mask == expected_mask;
    }
    }

    return true;
}

bool entity_mask_popcount_matches_patches(const SnapshotDelta& delta) {
    if (delta.kind != SnapshotDeltaKind::EntityPatch) {
        return true;
    }
    return count_changed_entities_in_mask(delta.changed_entity_mask) == delta.entity_patches.size();
}

u32 expected_ecs_patch_bytes(u8 changed_ecs_fields) {
    u32 bytes = 0;
    if ((changed_ecs_fields & static_cast<u8>(SnapshotEcsField::Position)) != 0) {
        bytes += 12;
    }
    if ((changed_ecs_fields & static_cast<u8>(SnapshotEcsField::Rotation)) != 0) {
        bytes += 16;
    }
    if ((changed_ecs_fields & static_cast<u8>(SnapshotEcsField::Scale)) != 0) {
        bytes += 12;
    }
    return bytes;
}

u32 expected_physics_patch_bytes(u8 changed_physics_fields) {
    u32 bytes = 0;
    if ((changed_physics_fields & static_cast<u8>(SnapshotPhysicsField::LinearVelocity)) != 0) {
        bytes += 12;
    }
    if ((changed_physics_fields & static_cast<u8>(SnapshotPhysicsField::AngularVelocity)) != 0) {
        bytes += 12;
    }
    if ((changed_physics_fields & static_cast<u8>(SnapshotPhysicsField::Mass)) != 0) {
        bytes += 4;
    }
    return bytes;
}

bool validate_entity_patch_payload_sizes(const SnapshotEntityPatch& patch) {
    if (patch.changed_ecs_fields == 0) {
        if (!patch.ecs_bytes.empty()) {
            return false;
        }
    } else if (patch.ecs_bytes.size() != expected_ecs_patch_bytes(patch.changed_ecs_fields)) {
        return false;
    }

    if (patch.changed_physics_fields == 0) {
        if (!patch.physics_bytes.empty()) {
            return false;
        }
    } else if (patch.physics_bytes.size() != expected_physics_patch_bytes(patch.changed_physics_fields)) {
        return false;
    }

    return true;
}

bool validate_entity_patch_field_bits(const SnapshotEntityPatch& patch) {
    return ecs_field_mask_valid(patch.changed_ecs_fields) && physics_field_mask_valid(patch.changed_physics_fields);
}

bool validate_entity_patch_index_in_mask(const SnapshotEntityPatch& patch, u64 changed_entity_mask) {
    return entity_index_in_changed_mask(changed_entity_mask, patch.entity_index);
}

bool validate_entity_patch_indices_unique(const SnapshotDelta& delta) {
    if (delta.kind != SnapshotDeltaKind::EntityPatch) {
        return true;
    }

    for (usize i = 0; i < delta.entity_patches.size(); ++i) {
        for (usize j = i + 1; j < delta.entity_patches.size(); ++j) {
            const SnapshotEntityPatch& a = delta.entity_patches[i];
            const SnapshotEntityPatch& b = delta.entity_patches[j];
            if (a.entity_index == b.entity_index && a.entity_generation == b.entity_generation) {
                return false;
            }
        }
    }
    return true;
}

bool validate_full_delta_payload(const SnapshotDelta& delta) {
    if (delta.kind != SnapshotDeltaKind::Full) {
        return true;
    }
    return !delta.full_ecs_state.empty() || !delta.full_physics_state.empty();
}

bool validate_delta_patch_field_bits(const SnapshotDelta& delta) {
    if (delta.kind != SnapshotDeltaKind::EntityPatch) {
        return true;
    }
    for (const SnapshotEntityPatch& patch : delta.entity_patches) {
        if (!validate_entity_patch_field_bits(patch)) {
            return false;
        }
    }
    return true;
}

bool validate_delta_target_frame(const SnapshotDelta& delta) {
    return delta.target_frame >= delta.base_frame;
}

bool validate_entity_index_trackable(u32 entity_index) {
    return entity_index_trackable(entity_index);
}

bool validate_delta_trackable_indices(const SnapshotDelta& delta) {
    if (delta.kind != SnapshotDeltaKind::EntityPatch) {
        return true;
    }
    for (const SnapshotEntityPatch& patch : delta.entity_patches) {
        if (!validate_entity_index_trackable(patch.entity_index)) {
            return false;
        }
    }
    return true;
}

bool ecs_field_mask_subset(u8 subset, u8 superset) {
    return (subset & superset) == subset;
}

bool physics_field_mask_subset(u8 subset, u8 superset) {
    return (subset & superset) == subset;
}

bool ecs_field_mask_nonempty(u8 mask) {
    return mask != 0;
}

bool physics_field_mask_nonempty(u8 mask) {
    return mask != 0;
}

bool validate_entity_patch_masks(const SnapshotEntityPatch& patch) {
    if (patch.changed_ecs_fields == 0 && patch.changed_physics_fields == 0) {
        return false;
    }
    if (!validate_entity_patch_field_bits(patch)) {
        return false;
    }
    if (patch.changed_ecs_fields != 0 && patch.ecs_bytes.empty()) {
        return false;
    }
    if (patch.changed_physics_fields != 0 && patch.physics_bytes.empty()) {
        return false;
    }
    return validate_entity_patch_payload_sizes(patch);
}

bool validate_delta_payload(const SnapshotDelta& delta) {
    switch (delta.kind) {
    case SnapshotDeltaKind::None:
        return delta.entity_patches.empty() && delta.full_ecs_state.empty() && delta.full_physics_state.empty() &&
               delta.changed_entity_mask == 0;
    case SnapshotDeltaKind::Full:
        return delta.entity_patches.empty() && delta.changed_entity_mask == 0 && validate_full_delta_payload(delta);
    case SnapshotDeltaKind::EntityPatch:
        if (delta.entity_patches.empty()) {
            return false;
        }
        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            if (!validate_entity_patch_masks(patch)) {
                return false;
            }
            if (!validate_entity_patch_index_in_mask(patch, delta.changed_entity_mask)) {
                return false;
            }
        }
        return validate_changed_entity_mask(delta) && entity_mask_popcount_matches_patches(delta) &&
               validate_entity_patch_indices_unique(delta);
    }

    return false;
}

bool is_empty_snapshot_delta(const SnapshotDelta& delta) {
    return delta.kind == SnapshotDeltaKind::None;
}

bool is_full_snapshot_delta(const SnapshotDelta& delta) {
    return delta.kind == SnapshotDeltaKind::Full;
}

bool is_entity_patch_snapshot_delta(const SnapshotDelta& delta) {
    return delta.kind == SnapshotDeltaKind::EntityPatch;
}

bool should_skip_delta_apply(const SnapshotDelta& delta) {
    return is_empty_snapshot_delta(delta);
}

SnapshotDelta make_empty_snapshot_delta(u32 base_frame, u32 target_frame, u64 base_checksum, u64 target_checksum) {
    SnapshotDelta delta;
    delta.base_frame = base_frame;
    delta.target_frame = target_frame;
    delta.base_checksum = base_checksum;
    delta.target_checksum = target_checksum;
    delta.kind = SnapshotDeltaKind::None;
    return delta;
}

void compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target, SnapshotDelta& delta,
                            SnapshotDeltaWorkspace& workspace) {
    SnapshotDeltaWorkspace::Impl& ws = workspace.impl();
    delta.base_frame = base.frame;
    delta.target_frame = target.frame;
    delta.base_checksum = base.checksum;
    delta.target_checksum = target.checksum;
    delta.changed_entity_mask = 0;
    delta.full_ecs_state.clear();
    delta.full_physics_state.clear();

    const auto make_full = [&]() {
        delta.kind = SnapshotDeltaKind::Full;
        delta.full_ecs_state = target.ecs_state;
        delta.full_physics_state = target.physics_state;
        set_patch_count(delta, 0u, ws.spare_patches);
        delta.changed_entity_mask = 0;
    };

    if (snapshots_equivalent(base, target) ||
        (base.ecs_state == target.ecs_state && base.physics_state == target.physics_state)) {
        delta.kind = SnapshotDeltaKind::None;
        set_patch_count(delta, 0u, ws.spare_patches);
        return;
    }

    parse_entity_records(base, ws.base_records, ws.base_index);
    parse_entity_records(target, ws.target_records, ws.target_index);

    usize patch_count = 0;
    for (const EntityRecord& target_record : ws.target_records) {
        const u64 key = (static_cast<u64>(target_record.generation) << 32) | target_record.index;
        const u32 base_slot = ws.base_index.find(key);
        const bool is_new = base_slot == ~0u;
        const u8 ecs_mask = is_new ? static_cast<u8>(SnapshotEcsField::All)
                                   : compute_ecs_field_mask(ws.base_records[base_slot].components,
                                                            target_record.components);
        const u8 physics_mask = is_new ? static_cast<u8>(SnapshotPhysicsField::All)
                                       : compute_physics_field_mask(ws.base_records[base_slot].components,
                                                                    target_record.components);

        if (ecs_mask == 0 && physics_mask == 0) {
            continue;
        }

        if (!entity_index_trackable(target_record.index)) {
            make_full();
            return;
        }

        set_patch_count(delta, patch_count + 1u, ws.spare_patches);
        SnapshotEntityPatch& patch = delta.entity_patches[patch_count++];
        patch.entity_index = target_record.index;
        patch.entity_generation = target_record.generation;
        patch.changed_ecs_fields = ecs_mask;
        patch.changed_physics_fields = physics_mask;
        serialize_masked_ecs(ecs_mask, target_record.components, patch.ecs_bytes);
        serialize_masked_physics(physics_mask, target_record.components, patch.physics_bytes);
        delta.changed_entity_mask |= entity_mask_bit(target_record.index);
    }
    set_patch_count(delta, patch_count, ws.spare_patches);

    usize patch_bytes = 0;
    for (const SnapshotEntityPatch& patch : delta.entity_patches) {
        patch_bytes += patch.ecs_bytes.size() + patch.physics_bytes.size() + 10;
    }

    const usize full_bytes = target.ecs_state.size() + target.physics_state.size();
    if (delta.entity_patches.empty() || patch_bytes >= full_bytes) {
        make_full();
        return;
    }

    delta.kind = SnapshotDeltaKind::EntityPatch;

    // Patches cannot express removed entities or a row order that differs from the canonical
    // reconstruction order — verify the reconstruction is byte-exact, else send the full state.
    apply_snapshot_delta(base, delta, ws.rebuilt, workspace);
    if (ws.rebuilt.ecs_state != target.ecs_state || ws.rebuilt.physics_state != target.physics_state) {
        make_full();
    }
}

SnapshotDelta compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target) {
    SnapshotDeltaWorkspace workspace;
    SnapshotDelta delta;
    compute_snapshot_delta(base, target, delta, workspace);
    return delta;
}

void apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta, GameSnapshot& out,
                          SnapshotDeltaWorkspace& workspace) {
    out.frame = delta.target_frame;
    out.checksum = delta.target_checksum;

    switch (delta.kind) {
    case SnapshotDeltaKind::None:
        break;
    case SnapshotDeltaKind::Full:
        out.ecs_state = delta.full_ecs_state;
        out.physics_state = delta.full_physics_state;
        return;
    case SnapshotDeltaKind::EntityPatch: {
        SnapshotDeltaWorkspace::Impl& ws = workspace.impl();
        std::vector<EntityRecord>& records = ws.apply_records;
        parse_entity_records(base, records, ws.apply_index, delta.entity_patches.size());

        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            const u64 key = (static_cast<u64>(patch.entity_generation) << 32) | patch.entity_index;
            EntityComponents patch_components = decode_masked_ecs(patch.changed_ecs_fields, patch.ecs_bytes);
            const EntityComponents physics_components =
                decode_masked_physics(patch.changed_physics_fields, patch.physics_bytes);
            merge_masked_components(patch_components, 0, patch.changed_physics_fields, physics_components);

            const u32 slot = ws.apply_index.find(key);
            if (slot == ~0u) {
                EntityRecord created;
                created.index = patch.entity_index;
                created.generation = patch.entity_generation;
                created.components = patch_components;
                created.has_ecs = patch.changed_ecs_fields != 0;
                created.has_physics = patch.changed_physics_fields != 0;
                ws.apply_index.insert(key, static_cast<u32>(records.size()));
                records.push_back(created);
            } else {
                EntityRecord& existing = records[slot];
                merge_masked_components(existing.components, patch.changed_ecs_fields, patch.changed_physics_fields,
                                        patch_components);
                existing.has_ecs = existing.has_ecs || patch.changed_ecs_fields != 0;
                existing.has_physics = existing.has_physics || patch.changed_physics_fields != 0;
            }
        }

        out.ecs_state.clear();
        out.physics_state.clear();
        for (const EntityRecord& record : records) {
            append_entity_record(out.ecs_state, out.physics_state, record);
        }
        return;
    }
    }

    // None (or an unknown kind): the base state carries over unchanged.
    out.ecs_state = base.ecs_state;
    out.physics_state = base.physics_state;
}

GameSnapshot apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta) {
    SnapshotDeltaWorkspace workspace;
    GameSnapshot result;
    apply_snapshot_delta(base, delta, result, workspace);
    return result;
}

bool verify_delta_base_checksum(const GameSnapshot& base, const SnapshotDelta& delta) {
    if (delta.base_checksum == 0) {
        return true;
    }
    return base.checksum == delta.base_checksum;
}

u64 compute_delta_checksum(const SnapshotDelta& delta) {
    // FNV-1a streamed over the canonical encoding (no scratch buffer).
    FnvStream fnv;
    fnv.u8v(static_cast<u8>(delta.kind));
    fnv.u32v(delta.base_frame);
    fnv.u32v(delta.target_frame);
    fnv.u32v(static_cast<u32>(delta.changed_entity_mask & 0xFFFFFFFFu));
    fnv.u32v(static_cast<u32>((delta.changed_entity_mask >> 32) & 0xFFFFFFFFu));
    fnv.u32v(static_cast<u32>(delta.entity_patches.size()));
    for (const SnapshotEntityPatch& patch : delta.entity_patches) {
        fnv.u32v(patch.entity_index);
        fnv.u32v(patch.entity_generation);
        fnv.u8v(patch.changed_ecs_fields);
        fnv.u8v(patch.changed_physics_fields);
        fnv.u32v(static_cast<u32>(patch.ecs_bytes.size()));
        fnv.bytes(patch.ecs_bytes.data(), patch.ecs_bytes.size());
        fnv.u32v(static_cast<u32>(patch.physics_bytes.size()));
        fnv.bytes(patch.physics_bytes.data(), patch.physics_bytes.size());
    }
    if (delta.kind == SnapshotDeltaKind::Full) {
        fnv.u32v(static_cast<u32>(delta.full_ecs_state.size()));
        fnv.bytes(delta.full_ecs_state.data(), delta.full_ecs_state.size());
        fnv.u32v(static_cast<u32>(delta.full_physics_state.size()));
        fnv.bytes(delta.full_physics_state.data(), delta.full_physics_state.size());
    }
    return fnv.hash;
}

SnapshotDeltaPreflight preflight_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta) {
    SnapshotDeltaPreflight result;
    result.empty_delta = is_empty_snapshot_delta(delta);
    result.base_checksum_ok = verify_delta_base_checksum(base, delta);
    result.entity_mask_ok = validate_changed_entity_mask(delta);
    result.mask_popcount_ok = entity_mask_popcount_matches_patches(delta);
    result.base_frame_ok = delta.base_frame == base.frame;
    result.payload_ok = validate_delta_payload(delta);
    result.full_payload_ok = validate_full_delta_payload(delta);
    result.target_frame_ok = validate_delta_target_frame(delta);
    result.trackable_indices_ok = validate_delta_trackable_indices(delta);
    result.field_bits_ok = validate_delta_patch_field_bits(delta);
    result.duplicate_index_ok = validate_entity_patch_indices_unique(delta);
    return result;
}

bool can_apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta) {
    return preflight_snapshot_delta(base, delta).can_apply();
}

bool can_apply_or_skip_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta) {
    return can_apply_snapshot_delta(base, delta) || should_skip_delta_apply(delta);
}

DeltaApplyResult apply_snapshot_delta_verified(const GameSnapshot& base, const SnapshotDelta& delta) {
    DeltaApplyResult result;
    const SnapshotDeltaPreflight preflight = preflight_snapshot_delta(base, delta);
    result.base_checksum_ok = preflight.base_checksum_ok;
    result.entity_mask_ok = preflight.entity_mask_ok;
    result.base_frame_ok = preflight.base_frame_ok;
    result.payload_ok = preflight.payload_ok;
    result.mask_popcount_ok = preflight.mask_popcount_ok;
    result.field_bits_ok = preflight.field_bits_ok;
    result.duplicate_index_ok = preflight.duplicate_index_ok;
    result.full_payload_ok = preflight.full_payload_ok;
    result.target_frame_ok = preflight.target_frame_ok;
    result.trackable_indices_ok = preflight.trackable_indices_ok;
    result.snapshot = apply_snapshot_delta(base, delta);

    if (delta.target_checksum != 0) {
        result.snapshot.checksum = delta.target_checksum;
        result.target_checksum_ok = verify_snapshot_checksum(result.snapshot);
    } else {
        result.target_checksum_ok = true;
    }

    return result;
}

void serialize_snapshot_delta(const SnapshotDelta& delta, NetSerializer& out) {
    out.write_u8(static_cast<u8>(delta.kind));
    out.write_u32(delta.base_frame);
    out.write_u32(delta.target_frame);
    out.write_u32(static_cast<u32>(delta.base_checksum & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>((delta.base_checksum >> 32) & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>(delta.target_checksum & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>((delta.target_checksum >> 32) & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>(delta.changed_entity_mask & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>((delta.changed_entity_mask >> 32) & 0xFFFFFFFFu));

    switch (delta.kind) {
    case SnapshotDeltaKind::None:
        break;
    case SnapshotDeltaKind::Full:
        out.write_u32(static_cast<u32>(delta.full_ecs_state.size()));
        out.buffer.insert(out.buffer.end(), delta.full_ecs_state.begin(), delta.full_ecs_state.end());
        out.write_u32(static_cast<u32>(delta.full_physics_state.size()));
        out.buffer.insert(out.buffer.end(), delta.full_physics_state.begin(), delta.full_physics_state.end());
        break;
    case SnapshotDeltaKind::EntityPatch:
        out.write_u32(static_cast<u32>(delta.entity_patches.size()));
        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            out.write_u32(patch.entity_index);
            out.write_u32(patch.entity_generation);
            out.write_u8(patch.changed_ecs_fields);
            out.write_u8(patch.changed_physics_fields);
            out.write_u32(static_cast<u32>(patch.ecs_bytes.size()));
            out.buffer.insert(out.buffer.end(), patch.ecs_bytes.begin(), patch.ecs_bytes.end());
            out.write_u32(static_cast<u32>(patch.physics_bytes.size()));
            out.buffer.insert(out.buffer.end(), patch.physics_bytes.begin(), patch.physics_bytes.end());
        }
        break;
    }
}

void deserialize_snapshot_delta(NetSerializer& in, SnapshotDelta& delta, SnapshotDeltaWorkspace& workspace) {
    std::vector<SnapshotEntityPatch>& spare = workspace.impl().spare_patches;
    delta.kind = static_cast<SnapshotDeltaKind>(in.read_u8());
    delta.base_frame = in.read_u32();
    delta.target_frame = in.read_u32();
    const u32 base_lo = in.read_u32();
    const u32 base_hi = in.read_u32();
    const u32 target_lo = in.read_u32();
    const u32 target_hi = in.read_u32();
    const u32 mask_lo = in.read_u32();
    const u32 mask_hi = in.read_u32();
    delta.base_checksum = (static_cast<u64>(base_hi) << 32) | base_lo;
    delta.target_checksum = (static_cast<u64>(target_hi) << 32) | target_lo;
    delta.changed_entity_mask = (static_cast<u64>(mask_hi) << 32) | mask_lo;
    delta.full_ecs_state.clear();
    delta.full_physics_state.clear();

    switch (delta.kind) {
    case SnapshotDeltaKind::None:
        set_patch_count(delta, 0u, spare);
        break;
    case SnapshotDeltaKind::Full: {
        set_patch_count(delta, 0u, spare);
        read_bytes(in, delta.full_ecs_state, in.read_u32());
        read_bytes(in, delta.full_physics_state, in.read_u32());
        break;
    }
    case SnapshotDeltaKind::EntityPatch: {
        // Each wire patch is at least 18 bytes; bound the count by what the buffer can hold.
        constexpr usize kMinWirePatchBytes = 18;
        u32 patch_count = in.read_u32();
        if (static_cast<usize>(patch_count) > in.bytes_remaining() / kMinWirePatchBytes) {
            patch_count = static_cast<u32>(in.bytes_remaining() / kMinWirePatchBytes);
        }
        set_patch_count(delta, patch_count, spare);
        for (u32 i = 0; i < patch_count; ++i) {
            SnapshotEntityPatch& patch = delta.entity_patches[i];
            patch.entity_index = in.read_u32();
            patch.entity_generation = in.read_u32();
            patch.changed_ecs_fields = in.read_u8();
            patch.changed_physics_fields = in.read_u8();
            read_bytes(in, patch.ecs_bytes, in.read_u32());
            read_bytes(in, patch.physics_bytes, in.read_u32());
        }
        break;
    }
    default:
        set_patch_count(delta, 0u, spare);
        break;
    }
}

SnapshotDelta deserialize_snapshot_delta(NetSerializer& in) {
    SnapshotDeltaWorkspace workspace;
    SnapshotDelta delta;
    deserialize_snapshot_delta(in, delta, workspace);
    return delta;
}

void SnapshotHistoryRing::init(u32 capacity_frames) {
    m_buffer.init(capacity_frames);
}

void SnapshotHistoryRing::clear() {
    m_buffer.clear();
}

void SnapshotHistoryRing::push(GameSnapshot snapshot) {
    const u32 frame = snapshot.frame;
    m_buffer.store_snapshot(frame, std::move(snapshot));
}

const GameSnapshot* SnapshotHistoryRing::get(u32 frame) const {
    return m_buffer.snapshot(frame);
}

const GameSnapshot* SnapshotHistoryRing::newest() const {
    if (!m_buffer.has_frame(m_buffer.newest_stored_frame())) {
        return nullptr;
    }
    return m_buffer.snapshot(m_buffer.newest_stored_frame());
}

u32 SnapshotHistoryRing::stored_frame_count() const {
    if (m_buffer.capacity() == 0 || !m_buffer.has_frame(m_buffer.newest_stored_frame())) {
        return 0;
    }

    const u32 span = m_buffer.newest_stored_frame() - m_buffer.oldest_stored_frame() + 1;
    return std::min(span, m_buffer.capacity());
}

u32 SnapshotHistoryRing::remaining_capacity() const {
    if (m_buffer.capacity() == 0) {
        return 0;
    }
    return m_buffer.capacity() - stored_frame_count();
}

std::optional<GameSnapshot> SnapshotHistoryRing::pop_oldest() {
    return m_buffer.evict_oldest_snapshot();
}

SnapshotHistoryPreflight SnapshotHistoryRing::preflight_apply_delta(u32 base_frame,
                                                                    const SnapshotDelta& delta) const {
    SnapshotHistoryPreflight result;
    result.ring_empty = empty();
    if (result.ring_empty) {
        return result;
    }

    result.has_baseline = has_baseline(base_frame);
    if (!result.has_baseline) {
        return result;
    }

    const GameSnapshot* base = m_buffer.snapshot(base_frame);
    result.delta_preflight = preflight_snapshot_delta(*base, delta);
    result.skipped = result.delta_preflight.empty_delta;
    return result;
}

bool SnapshotHistoryRing::should_skip_apply_delta(u32 base_frame, const SnapshotDelta& delta) const {
    const SnapshotHistoryPreflight preflight = preflight_apply_delta(base_frame, delta);
    return preflight.ring_empty || !preflight.has_baseline || preflight.skipped;
}

bool SnapshotHistoryRing::can_apply_or_skip_delta(u32 base_frame, const SnapshotDelta& delta) const {
    return preflight_apply_delta(base_frame, delta).can_apply_or_skip();
}

bool SnapshotHistoryRing::can_apply_delta(u32 base_frame, const SnapshotDelta& delta) const {
    return preflight_apply_delta(base_frame, delta).can_apply();
}

bool SnapshotHistoryRing::apply_delta_and_store(u32 base_frame, const SnapshotDelta& delta, GameSnapshot* out) {
    const SnapshotHistoryPreflight preflight = preflight_apply_delta(base_frame, delta);
    if (!preflight.can_apply()) {
        return false;
    }

    const GameSnapshot* base = m_buffer.snapshot(base_frame);
    if (base == nullptr) {
        return false;
    }

    GameSnapshot stored;
    if (preflight.skipped) {
        stored = *base;
        stored.frame = delta.target_frame;
        if (delta.target_checksum != 0) {
            stored.checksum = delta.target_checksum;
        }
    } else {
        const DeltaApplyResult applied = apply_snapshot_delta_verified(*base, delta);
        if (!applied.target_checksum_ok) {
            return false;
        }
        stored = applied.snapshot;
        if (stored.checksum == 0) {
            stored.checksum = compute_snapshot_checksum(stored);
        }
    }

    push(std::move(stored));

    if (out != nullptr) {
        const GameSnapshot* result = m_buffer.snapshot(delta.target_frame);
        if (result != nullptr) {
            *out = *result;
        }
    }
    return true;
}

} // namespace fuse::net
