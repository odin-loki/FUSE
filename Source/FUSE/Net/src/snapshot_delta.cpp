#include <fuse/net/snapshot_delta.hpp>

#include <algorithm>
#include <unordered_map>

namespace fuse::net {

namespace {

struct EntityRecord {
    u32 index = 0;
    u32 generation = 0;
    std::vector<byte> ecs_bytes;
    std::vector<byte> physics_bytes;
};

void read_bytes(NetSerializer& in, std::vector<byte>& out, u32 size) {
    out.resize(size);
    for (u32 i = 0; i < size; ++i) {
        out[i] = in.read_u8();
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

        NetSerializer payload;
        payload.write_vec3(ecs_reader.read_vec3());
        payload.write_quat(ecs_reader.read_quat());
        payload.write_vec3(ecs_reader.read_vec3());
        record.ecs_bytes = std::move(payload.buffer);

        const u64 key = (static_cast<u64>(record.generation) << 32) | record.index;
        records[key] = std::move(record);
    }

    NetSerializer physics_reader;
    physics_reader.buffer = snapshot.physics_state;
    physics_reader.reset_read();
    while (!physics_reader.read_complete()) {
        const u32 index = physics_reader.read_u32();
        const u32 generation = physics_reader.read_u32();

        NetSerializer payload;
        payload.write_vec3(physics_reader.read_vec3());
        payload.write_vec3(physics_reader.read_vec3());
        payload.write_f32(physics_reader.read_f32());

        const u64 key = (static_cast<u64>(generation) << 32) | index;
        EntityRecord& record = records[key];
        record.index = index;
        record.generation = generation;
        record.physics_bytes = std::move(payload.buffer);
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
    ecs_out.buffer.insert(ecs_out.buffer.end(), record.ecs_bytes.begin(), record.ecs_bytes.end());

    if (!record.physics_bytes.empty()) {
        physics_out.write_u32(record.index);
        physics_out.write_u32(record.generation);
        physics_out.buffer.insert(physics_out.buffer.end(), record.physics_bytes.begin(), record.physics_bytes.end());
    }
}

} // namespace

SnapshotDelta compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target) {
    SnapshotDelta delta;
    delta.base_frame = base.frame;
    delta.target_frame = target.frame;
    delta.base_checksum = base.checksum;
    delta.target_checksum = target.checksum;

    if (base.checksum == target.checksum && base.ecs_state == target.ecs_state &&
        base.physics_state == target.physics_state) {
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
        if (it == base_map.end() || it->second.ecs_bytes != target_record.ecs_bytes ||
            it->second.physics_bytes != target_record.physics_bytes) {
            SnapshotEntityPatch patch;
            patch.entity_index = target_record.index;
            patch.entity_generation = target_record.generation;
            patch.ecs_bytes = target_record.ecs_bytes;
            patch.physics_bytes = target_record.physics_bytes;
            delta.entity_patches.push_back(std::move(patch));
        }
    }

    const usize patch_bytes = [&]() {
        usize total = 0;
        for (const SnapshotEntityPatch& patch : delta.entity_patches) {
            total += patch.ecs_bytes.size() + patch.physics_bytes.size() + 8;
        }
        return total;
    }();

    const usize full_bytes = target.ecs_state.size() + target.physics_state.size();
    if (delta.entity_patches.empty() || patch_bytes >= full_bytes) {
        delta.kind = SnapshotDeltaKind::Full;
        delta.full_ecs_state = target.ecs_state;
        delta.full_physics_state = target.physics_state;
        delta.entity_patches.clear();
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
            EntityRecord updated;
            updated.index = patch.entity_index;
            updated.generation = patch.entity_generation;
            updated.ecs_bytes = patch.ecs_bytes;
            updated.physics_bytes = patch.physics_bytes;

            const auto it = index_by_key.find(key);
            if (it == index_by_key.end()) {
                index_by_key[key] = records.size();
                records.push_back(std::move(updated));
            } else {
                records[it->second] = std::move(updated);
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

void serialize_snapshot_delta(const SnapshotDelta& delta, NetSerializer& out) {
    out.write_u8(static_cast<u8>(delta.kind));
    out.write_u32(delta.base_frame);
    out.write_u32(delta.target_frame);
    out.write_u32(static_cast<u32>(delta.base_checksum & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>((delta.base_checksum >> 32) & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>(delta.target_checksum & 0xFFFFFFFFu));
    out.write_u32(static_cast<u32>((delta.target_checksum >> 32) & 0xFFFFFFFFu));

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
    delta.base_checksum = (static_cast<u64>(base_hi) << 32) | base_lo;
    delta.target_checksum = (static_cast<u64>(target_hi) << 32) | target_lo;

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
            read_bytes(in, patch.ecs_bytes, in.read_u32());
            read_bytes(in, patch.physics_bytes, in.read_u32());
        }
        break;
    }
    }

    return delta;
}

} // namespace fuse::net
