#include <fuse/net/snapshot_delta.hpp>

#include <algorithm>
#include <unordered_map>

namespace fuse::net {

namespace {

struct EntityComponents {
    ecs::vec3 position{};
    ecs::quat rotation{};
    ecs::vec3 scale{1.f, 1.f, 1.f, 0.f};
    ecs::vec3 linear_velocity{};
    ecs::vec3 angular_velocity{};
    f32 mass = 1.f;
};

struct EntityRecord {
    u32 index = 0;
    u32 generation = 0;
    EntityComponents components{};
    std::vector<byte> ecs_bytes;
    std::vector<byte> physics_bytes;
};

void read_bytes(NetSerializer& in, std::vector<byte>& out, u32 size) {
    out.resize(size);
    for (u32 i = 0; i < size; ++i) {
        out[i] = in.read_u8();
    }
}

EntityComponents decode_ecs_components(const std::vector<byte>& ecs_bytes) {
    EntityComponents components;
    if (ecs_bytes.empty()) {
        return components;
    }

    NetSerializer reader;
    reader.buffer = ecs_bytes;
    reader.reset_read();
    components.position = reader.read_vec3();
    components.rotation = reader.read_quat();
    components.scale = reader.read_vec3();
    return components;
}

EntityComponents decode_physics_components(const std::vector<byte>& physics_bytes) {
    EntityComponents components;
    if (physics_bytes.empty()) {
        return components;
    }

    NetSerializer reader;
    reader.buffer = physics_bytes;
    reader.reset_read();
    components.linear_velocity = reader.read_vec3();
    components.angular_velocity = reader.read_vec3();
    components.mass = reader.read_f32();
    return components;
}

void encode_ecs_components(const EntityComponents& components, std::vector<byte>& out) {
    NetSerializer writer;
    writer.write_vec3(components.position);
    writer.write_quat(components.rotation);
    writer.write_vec3(components.scale);
    out = std::move(writer.buffer);
}

void encode_physics_components(const EntityComponents& components, std::vector<byte>& out) {
    NetSerializer writer;
    writer.write_vec3(components.linear_velocity);
    writer.write_vec3(components.angular_velocity);
    writer.write_f32(components.mass);
    out = std::move(writer.buffer);
}

void serialize_masked_ecs(u8 mask, const EntityComponents& components, std::vector<byte>& out) {
    NetSerializer writer;
    if ((mask & static_cast<u8>(SnapshotEcsField::Position)) != 0) {
        writer.write_vec3(components.position);
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Rotation)) != 0) {
        writer.write_quat(components.rotation);
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Scale)) != 0) {
        writer.write_vec3(components.scale);
    }
    out = std::move(writer.buffer);
}

void serialize_masked_physics(u8 mask, const EntityComponents& components, std::vector<byte>& out) {
    NetSerializer writer;
    if ((mask & static_cast<u8>(SnapshotPhysicsField::LinearVelocity)) != 0) {
        writer.write_vec3(components.linear_velocity);
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::AngularVelocity)) != 0) {
        writer.write_vec3(components.angular_velocity);
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::Mass)) != 0) {
        writer.write_f32(components.mass);
    }
    out = std::move(writer.buffer);
}

EntityComponents decode_masked_ecs(u8 mask, const std::vector<byte>& ecs_bytes) {
    EntityComponents components;
    if (ecs_bytes.empty() || mask == 0) {
        return components;
    }

    NetSerializer reader;
    reader.buffer = ecs_bytes;
    reader.reset_read();
    if ((mask & static_cast<u8>(SnapshotEcsField::Position)) != 0) {
        components.position = reader.read_vec3();
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Rotation)) != 0) {
        components.rotation = reader.read_quat();
    }
    if ((mask & static_cast<u8>(SnapshotEcsField::Scale)) != 0) {
        components.scale = reader.read_vec3();
    }
    return components;
}

EntityComponents decode_masked_physics(u8 mask, const std::vector<byte>& physics_bytes) {
    EntityComponents components;
    if (physics_bytes.empty() || mask == 0) {
        return components;
    }

    NetSerializer reader;
    reader.buffer = physics_bytes;
    reader.reset_read();
    if ((mask & static_cast<u8>(SnapshotPhysicsField::LinearVelocity)) != 0) {
        components.linear_velocity = reader.read_vec3();
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::AngularVelocity)) != 0) {
        components.angular_velocity = reader.read_vec3();
    }
    if ((mask & static_cast<u8>(SnapshotPhysicsField::Mass)) != 0) {
        components.mass = reader.read_f32();
    }
    return components;
}

bool vec3_equal(const ecs::vec3& a, const ecs::vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool quat_equal(const ecs::quat& a, const ecs::quat& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
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
    if (base.mass != target.mass) {
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

std::vector<EntityRecord> parse_entity_records(const GameSnapshot& snapshot) {
    std::unordered_map<u64, EntityRecord> records;

    NetSerializer ecs_reader;
    ecs_reader.buffer = snapshot.ecs_state;
    ecs_reader.reset_read();
    while (!ecs_reader.read_complete()) {
        EntityRecord record;
        record.index = ecs_reader.read_u32();
        record.generation = ecs_reader.read_u32();
        record.components.position = ecs_reader.read_vec3();
        record.components.rotation = ecs_reader.read_quat();
        record.components.scale = ecs_reader.read_vec3();
        encode_ecs_components(record.components, record.ecs_bytes);

        const u64 key = (static_cast<u64>(record.generation) << 32) | record.index;
        records[key] = std::move(record);
    }

    NetSerializer physics_reader;
    physics_reader.buffer = snapshot.physics_state;
    physics_reader.reset_read();
    while (!physics_reader.read_complete()) {
        const u32 index = physics_reader.read_u32();
        const u32 generation = physics_reader.read_u32();
        ecs::vec3 linear_velocity = physics_reader.read_vec3();
        ecs::vec3 angular_velocity = physics_reader.read_vec3();
        const f32 mass = physics_reader.read_f32();

        const u64 key = (static_cast<u64>(generation) << 32) | index;
        EntityRecord& record = records[key];
        record.index = index;
        record.generation = generation;
        record.components.linear_velocity = linear_velocity;
        record.components.angular_velocity = angular_velocity;
        record.components.mass = mass;
        encode_physics_components(record.components, record.physics_bytes);
    }

    std::vector<EntityRecord> ordered;
    ordered.reserve(records.size());
    for (auto& entry : records) {
        ordered.push_back(std::move(entry.second));
    }
    std::sort(ordered.begin(), ordered.end(), [](const EntityRecord& a, const EntityRecord& b) {
        if (a.generation != b.generation) {
            return a.generation < b.generation;
        }
        return a.index < b.index;
    });
    return ordered;
}

void append_entity_record(NetSerializer& ecs_out, NetSerializer& physics_out, const EntityRecord& record) {
    ecs_out.write_u32(record.index);
    ecs_out.write_u32(record.generation);
    ecs_out.write_vec3(record.components.position);
    ecs_out.write_quat(record.components.rotation);
    ecs_out.write_vec3(record.components.scale);

    if (!record.physics_bytes.empty()) {
        physics_out.write_u32(record.index);
        physics_out.write_u32(record.generation);
        physics_out.write_vec3(record.components.linear_velocity);
        physics_out.write_vec3(record.components.angular_velocity);
        physics_out.write_f32(record.components.mass);
    }
}

u64 entity_mask_bit(u32 entity_index) {
    if (entity_index >= 64) {
        return 0;
    }
    return 1ull << entity_index;
}

} // namespace

bool snapshots_equivalent(const GameSnapshot& base, const GameSnapshot& target) {
    return base.frame == target.frame && base.checksum == target.checksum && base.ecs_state == target.ecs_state &&
           base.physics_state == target.physics_state;
}

SnapshotDelta compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target) {
    SnapshotDelta delta;
    delta.base_frame = base.frame;
    delta.target_frame = target.frame;
    delta.base_checksum = base.checksum;
    delta.target_checksum = target.checksum;

    if (snapshots_equivalent(base, target)) {
        delta.kind = SnapshotDeltaKind::None;
        return delta;
    }

    const std::vector<EntityRecord> base_records = parse_entity_records(base);
    const std::vector<EntityRecord> target_records = parse_entity_records(target);

    std::unordered_map<u64, EntityRecord> base_map;
    base_map.reserve(base_records.size());
    for (const EntityRecord& record : base_records) {
        const u64 key = (static_cast<u64>(record.generation) << 32) | record.index;
        base_map[key] = record;
    }

    for (const EntityRecord& target_record : target_records) {
        const u64 key = (static_cast<u64>(target_record.generation) << 32) | target_record.index;
        const auto it = base_map.find(key);
        const bool is_new = it == base_map.end();
        const u8 ecs_mask =
            is_new ? static_cast<u8>(SnapshotEcsField::All)
                   : compute_ecs_field_mask(it->second.components, target_record.components);
        const u8 physics_mask = is_new ? static_cast<u8>(SnapshotPhysicsField::All)
                                       : compute_physics_field_mask(it->second.components, target_record.components);

        if (ecs_mask == 0 && physics_mask == 0) {
            continue;
        }

        SnapshotEntityPatch patch;
        patch.entity_index = target_record.index;
        patch.entity_generation = target_record.generation;
        patch.changed_ecs_fields = ecs_mask;
        patch.changed_physics_fields = physics_mask;
        serialize_masked_ecs(ecs_mask, target_record.components, patch.ecs_bytes);
        serialize_masked_physics(physics_mask, target_record.components, patch.physics_bytes);
        delta.changed_entity_mask |= entity_mask_bit(target_record.index);
        delta.entity_patches.push_back(std::move(patch));
    }

    const usize patch_bytes = [&]() {
        usize total = 0;
        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            total += patch.ecs_bytes.size() + patch.physics_bytes.size() + 10;
        }
        return total;
    }();

    const usize full_bytes = target.ecs_state.size() + target.physics_state.size();
    if (delta.entity_patches.empty() || patch_bytes >= full_bytes) {
        delta.kind = SnapshotDeltaKind::Full;
        delta.full_ecs_state = target.ecs_state;
        delta.full_physics_state = target.physics_state;
        delta.entity_patches.clear();
        delta.changed_entity_mask = 0;
        return delta;
    }

    delta.kind = SnapshotDeltaKind::EntityPatch;
    return delta;
}

GameSnapshot apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta) {
    GameSnapshot result = base;
    result.frame = delta.target_frame;
    result.checksum = delta.target_checksum;

    switch (delta.kind) {
    case SnapshotDeltaKind::None:
        return result;
    case SnapshotDeltaKind::Full:
        result.ecs_state = delta.full_ecs_state;
        result.physics_state = delta.full_physics_state;
        return result;
    case SnapshotDeltaKind::EntityPatch: {
        std::vector<EntityRecord> records = parse_entity_records(base);
        std::unordered_map<u64, usize> index_by_key;
        index_by_key.reserve(records.size());
        for (usize i = 0; i < records.size(); ++i) {
            const u64 key = (static_cast<u64>(records[i].generation) << 32) | records[i].index;
            index_by_key[key] = i;
        }

        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            const u64 key = (static_cast<u64>(patch.entity_generation) << 32) | patch.entity_index;
            EntityComponents patch_components = decode_masked_ecs(patch.changed_ecs_fields, patch.ecs_bytes);
            const EntityComponents physics_components =
                decode_masked_physics(patch.changed_physics_fields, patch.physics_bytes);
            merge_masked_components(patch_components, 0, patch.changed_physics_fields, physics_components);

            const auto it = index_by_key.find(key);
            if (it == index_by_key.end()) {
                EntityRecord created;
                created.index = patch.entity_index;
                created.generation = patch.entity_generation;
                created.components = patch_components;
                index_by_key[key] = records.size();
                records.push_back(std::move(created));
            } else {
                merge_masked_components(records[it->second].components, patch.changed_ecs_fields,
                                        patch.changed_physics_fields, patch_components);
                encode_ecs_components(records[it->second].components, records[it->second].ecs_bytes);
                encode_physics_components(records[it->second].components, records[it->second].physics_bytes);
            }
        }

        NetSerializer ecs_out;
        NetSerializer physics_out;
        for (const EntityRecord& record : records) {
            append_entity_record(ecs_out, physics_out, record);
        }
        result.ecs_state = std::move(ecs_out.buffer);
        result.physics_state = std::move(physics_out.buffer);
        return result;
    }
    }

    return result;
}

bool verify_delta_base_checksum(const GameSnapshot& base, const SnapshotDelta& delta) {
    if (delta.base_checksum == 0) {
        return true;
    }
    return base.checksum == delta.base_checksum;
}

u64 compute_delta_checksum(const SnapshotDelta& delta) {
    NetSerializer writer;
    writer.write_u8(static_cast<u8>(delta.kind));
    writer.write_u32(delta.base_frame);
    writer.write_u32(delta.target_frame);
    writer.write_u32(static_cast<u32>(delta.changed_entity_mask & 0xFFFFFFFFu));
    writer.write_u32(static_cast<u32>((delta.changed_entity_mask >> 32) & 0xFFFFFFFFu));
    writer.write_u32(static_cast<u32>(delta.entity_patches.size()));
    for (const SnapshotEntityPatch& patch : delta.entity_patches) {
        writer.write_u32(patch.entity_index);
        writer.write_u32(patch.entity_generation);
        writer.write_u8(patch.changed_ecs_fields);
        writer.write_u8(patch.changed_physics_fields);
        writer.write_u32(static_cast<u32>(patch.ecs_bytes.size()));
        writer.buffer.insert(writer.buffer.end(), patch.ecs_bytes.begin(), patch.ecs_bytes.end());
        writer.write_u32(static_cast<u32>(patch.physics_bytes.size()));
        writer.buffer.insert(writer.buffer.end(), patch.physics_bytes.begin(), patch.physics_bytes.end());
    }
    if (delta.kind == SnapshotDeltaKind::Full) {
        writer.write_u32(static_cast<u32>(delta.full_ecs_state.size()));
        writer.buffer.insert(writer.buffer.end(), delta.full_ecs_state.begin(), delta.full_ecs_state.end());
        writer.write_u32(static_cast<u32>(delta.full_physics_state.size()));
        writer.buffer.insert(writer.buffer.end(), delta.full_physics_state.begin(), delta.full_physics_state.end());
    }
    return fnv1a64_bytes(writer.buffer.data(), writer.buffer.size());
}

DeltaApplyResult apply_snapshot_delta_verified(const GameSnapshot& base, const SnapshotDelta& delta) {
    DeltaApplyResult result;
    result.base_checksum_ok = verify_delta_base_checksum(base, delta);
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

SnapshotDelta deserialize_snapshot_delta(NetSerializer& in) {
    SnapshotDelta delta;
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

    switch (delta.kind) {
    case SnapshotDeltaKind::None:
        break;
    case SnapshotDeltaKind::Full: {
        read_bytes(in, delta.full_ecs_state, in.read_u32());
        read_bytes(in, delta.full_physics_state, in.read_u32());
        break;
    }
    case SnapshotDeltaKind::EntityPatch: {
        const u32 patch_count = in.read_u32();
        delta.entity_patches.resize(patch_count);
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
    }

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

bool SnapshotHistoryRing::apply_delta_and_store(u32 base_frame, const SnapshotDelta& delta, GameSnapshot* out) {
    const GameSnapshot* base = m_buffer.snapshot(base_frame);
    if (base == nullptr) {
        return false;
    }

    const DeltaApplyResult applied = apply_snapshot_delta_verified(*base, delta);
    if (!applied.base_checksum_ok) {
        return false;
    }

    GameSnapshot stored = applied.snapshot;
    if (stored.checksum == 0) {
        stored.checksum = compute_snapshot_checksum(stored);
    }
    push(std::move(stored));

    if (out != nullptr) {
        *out = *m_buffer.snapshot(delta.target_frame);
    }
    return true;
}

} // namespace fuse::net
