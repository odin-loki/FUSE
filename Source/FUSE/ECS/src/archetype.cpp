#include <fuse/ecs/archetype.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace fuse::ecs {

namespace {

std::pmr::memory_resource* resourceOrDefault(std::pmr::memory_resource* resource) {
    return resource != nullptr ? resource : std::pmr::new_delete_resource();
}

} // namespace

ComponentColumn::ComponentColumn(std::type_index column_type, usize size, std::pmr::memory_resource* resource)
    : type(column_type), element_size(size), storage(resourceOrDefault(resource)) {}

ComponentColumn::ComponentColumn(const ComponentColumn& other)
    : type(other.type), element_size(other.element_size), storage(other.storage, other.storage.get_allocator()) {}

ComponentColumn& ComponentColumn::operator=(const ComponentColumn& other) {
    if (this != &other) {
        // pmr containers never propagate their resource on assignment; rebuild storage on the source's
        // resource so a copied column stays where the original lives (e.g. CUDA managed memory).
        std::pmr::vector<std::byte> copy(other.storage, other.storage.get_allocator());
        type = other.type;
        element_size = other.element_size;
        std::destroy_at(&storage);
        std::construct_at(&storage, std::move(copy));
    }
    return *this;
}

ComponentColumn& ComponentColumn::operator=(ComponentColumn&& other) noexcept {
    if (this != &other) {
        type = other.type;
        element_size = other.element_size;
        std::destroy_at(&storage);
        std::construct_at(&storage, std::move(other.storage));
    }
    return *this;
}

void ComponentColumn::rebind(std::pmr::memory_resource* resource) {
    resource = resourceOrDefault(resource);
    if (memory_resource() == resource || *memory_resource() == *resource) {
        return;
    }
    std::pmr::vector<std::byte> moved(storage.begin(), storage.end(), resource);
    std::destroy_at(&storage);
    std::construct_at(&storage, std::move(moved));
}

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

void ComponentColumn::write_at(usize row, const void* src) {
    if (element_size == 0 || src == nullptr) {
        return;
    }
    const usize needed_bytes = (row + 1) * element_size;
    if (storage.size() < needed_bytes) {
        storage.resize(needed_bytes);
    }
    std::memcpy(storage.data() + row * element_size, src, element_size);
}

void ComponentColumn::write_default_at(usize row) {
    if (element_size == 0) {
        return;
    }
    const usize needed_bytes = (row + 1) * element_size;
    if (storage.size() < needed_bytes) {
        storage.resize(needed_bytes);
    }
    std::memset(storage.data() + row * element_size, 0, element_size);
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
    return std::find(component_types.begin(), component_types.end(), type) != component_types.end();
}

ComponentColumn& Archetype::ensure_column(std::type_index type, usize element_size,
                                          std::pmr::memory_resource* resource) {
    auto it = columns.find(type);
    if (it == columns.end()) {
        it = columns.try_emplace(type, type, element_size, resource).first;
    }
    ComponentColumn& column = it->second;
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
