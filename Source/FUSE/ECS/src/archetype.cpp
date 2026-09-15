#include <fuse/ecs/archetype.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::ecs {

void* ComponentColumn::at(usize row) {
    return storage.data() + row * element_size;
}

const void* ComponentColumn::at(usize row) const {
    return storage.data() + row * element_size;
}

void ComponentColumn::push(const void* src) {
    const usize old_bytes = storage.size();
    storage.resize(old_bytes + element_size);
    std::memcpy(storage.data() + old_bytes, src, element_size);
}

void ComponentColumn::push_default() {
    const usize old_bytes = storage.size();
    storage.resize(old_bytes + element_size);
    std::memset(storage.data() + old_bytes, 0, element_size);
}

void ComponentColumn::swap_remove(usize row) {
    const usize n = count();
    if (row >= n) {
        return;
    }
    if (row + 1 < n) {
        std::byte* dst = storage.data() + row * element_size;
        const std::byte* src = storage.data() + (n - 1) * element_size;
        std::memcpy(dst, src, element_size);
    }
    storage.resize((n - 1) * element_size);
}

bool Archetype::has_component(std::type_index type) const {
    return columns.find(type) != columns.end();
}

ComponentColumn& Archetype::ensure_column(std::type_index type, usize element_size) {
    ComponentColumn& column = columns[type];
    if (column.element_size == 0) {
        column.type = type;
        column.element_size = element_size;
    }
    return column;
}

ComponentColumn* Archetype::find_column(std::type_index type) {
    auto it = columns.find(type);
    return it == columns.end() ? nullptr : &it->second;
}

const ComponentColumn* Archetype::find_column(std::type_index type) const {
    auto it = columns.find(type);
    return it == columns.end() ? nullptr : &it->second;
}

usize Archetype::append_entity(EntityID id) {
    entities.push_back(id);
    return entities.size() - 1;
}

EntityID Archetype::remove_entity(usize row) {
    if (row >= count()) {
        return EntityID::null();
    }

    EntityID swapped = EntityID::null();
    if (row + 1 < count()) {
        swapped = entities.back();
    }

    entities[row] = entities.back();
    entities.pop_back();

    for (auto& [type, column] : columns) {
        column.swap_remove(row);
    }

    return swapped;
}

ArchetypeID make_archetype_id(const std::vector<std::type_index>& sorted_types) {
    u64 hash = 1469598103934665603ull;
    for (const std::type_index& type : sorted_types) {
        hash ^= static_cast<u64>(type.hash_code());
        hash *= 1099511628211ull;
    }
    return ArchetypeID{hash};
}

} // namespace fuse::ecs
