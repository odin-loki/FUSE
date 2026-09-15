#include <fuse/ecs/registry.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace fuse::ecs {

void Registry::init(usize max_entities) {
    destroy();
    m_max_entities = max_entities;
    m_records.reserve(max_entities);

    Archetype empty{};
    empty.id = ArchetypeID{0};
    m_archetypes.push_back(std::move(empty));
    m_archetype_lookup[0] = 0;
}

void Registry::destroy() {
    m_records.clear();
    m_free_list.clear();
    m_archetypes.clear();
    m_archetype_lookup.clear();
    m_alive_count = 0;
}

EntityID Registry::create() {
    u32 index = 0;
    u32 generation = 1;

    if (!m_free_list.empty()) {
        index = m_free_list.back();
        m_free_list.pop_back();
        EntityRecord& rec = m_records[index];
        if (rec.generation == 0) {
            rec.generation = 1;
        } else {
            ++rec.generation;
        }
        generation = rec.generation;
        rec.alive = true;
        rec.archetype_index = 0;
        rec.row = static_cast<u32>(m_archetypes[0].append_entity(EntityID{index, generation}));
    } else {
        if (m_records.size() >= m_max_entities) {
            return EntityID::null();
        }
        index = static_cast<u32>(m_records.size());
        m_records.push_back(EntityRecord{0, 0, generation, true});
        EntityRecord& rec = m_records[index];
        rec.row = static_cast<u32>(m_archetypes[0].append_entity(EntityID{index, generation}));
    }

    ++m_alive_count;
    return EntityID{index, generation};
}

void Registry::destroy_entity(EntityID id) {
    EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return;
    }

    Archetype& archetype = m_archetypes[rec->archetype_index];
    EntityID swapped = archetype.remove_entity(rec->row);
    if (swapped.valid()) {
        EntityRecord* moved = record(swapped);
        if (moved != nullptr) {
            moved->row = rec->row;
        }
    }

    rec->alive = false;
    rec->archetype_index = 0;
    rec->row = 0;
    m_free_list.push_back(id.index);
    --m_alive_count;
}

bool Registry::alive(EntityID id) const {
    return record(id) != nullptr;
}

usize Registry::count() const {
    return m_alive_count;
}

Registry::EntityRecord* Registry::record(EntityID id) {
    if (!id.valid() || id.index >= m_records.size()) {
        return nullptr;
    }
    EntityRecord& rec = m_records[id.index];
    if (!rec.alive || rec.generation != id.generation) {
        return nullptr;
    }
    return &rec;
}

const Registry::EntityRecord* Registry::record(EntityID id) const {
    if (!id.valid() || id.index >= m_records.size()) {
        return nullptr;
    }
    const EntityRecord& rec = m_records[id.index];
    if (!rec.alive || rec.generation != id.generation) {
        return nullptr;
    }
    return &rec;
}

u32 Registry::find_or_create_archetype(const std::vector<std::type_index>& sorted_types) {
    const ArchetypeID id = make_archetype_id(sorted_types);
    auto found = m_archetype_lookup.find(id.hash);
    if (found != m_archetype_lookup.end()) {
        return found->second;
    }

    Archetype archetype{};
    archetype.id = id;
    archetype.component_types = sorted_types;
    const u32 index = static_cast<u32>(m_archetypes.size());
    m_archetypes.push_back(std::move(archetype));
    m_archetype_lookup[id.hash] = index;
    return index;
}

void Registry::migrate_entity(EntityID id, const std::vector<std::type_index>& target_types,
                              const std::unordered_map<std::type_index, const void*>& new_values,
                              const std::unordered_map<std::type_index, usize>& type_sizes) {
    EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return;
    }

    Archetype& source = m_archetypes[rec->archetype_index];
    const u32 target_index = find_or_create_archetype(target_types);
    Archetype& target = m_archetypes[target_index];

    for (const std::type_index& type : target_types) {
        usize element_size = 0;
        auto size_it = type_sizes.find(type);
        if (size_it != type_sizes.end()) {
            element_size = size_it->second;
        } else if (const ComponentColumn* existing = source.find_column(type)) {
            element_size = existing->element_size;
        } else if (const ComponentColumn* existing_target = target.find_column(type)) {
            element_size = existing_target->element_size;
        }
        target.ensure_column(type, element_size);
    }

    const usize new_row = target.append_entity(id);

    for (const std::type_index& type : target_types) {
        ComponentColumn& dst = *target.find_column(type);
        auto value_it = new_values.find(type);
        if (value_it != new_values.end()) {
            dst.push(value_it->second);
        } else if (const ComponentColumn* src = source.find_column(type)) {
            dst.push(src->at(rec->row));
        } else {
            dst.push_default();
        }
    }

    EntityID swapped = source.remove_entity(rec->row);
    if (swapped.valid()) {
        EntityRecord* moved = record(swapped);
        if (moved != nullptr) {
            moved->row = rec->row;
        }
    }

    rec->archetype_index = target_index;
    rec->row = static_cast<u32>(new_row);
}

} // namespace fuse::ecs
