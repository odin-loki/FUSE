#pragma once

#include <fuse/ecs/entity.hpp>

#include <cstddef>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {

/// Sorted component-type signature hash.
struct ArchetypeID {
    u64 hash = 0;

    bool operator==(const ArchetypeID& other) const { return hash == other.hash; }
    bool operator!=(const ArchetypeID& other) const { return hash != other.hash; }
};

/// One column per component type — all columns share the same row count.
struct ComponentColumn {
    std::type_index type{typeid(void)};
    usize element_size = 0;
    std::vector<std::byte> storage;

    [[nodiscard]] usize count() const {
        return element_size == 0 ? 0 : storage.size() / element_size;
    }

    void* at(usize row);
    const void* at(usize row) const;
    void push(const void* src);
    void push_default();
    void swap_remove(usize row);
};

/// Entities with identical component sets share one archetype (SoA columns).
struct Archetype {
    ArchetypeID id{};
    std::vector<std::type_index> component_types;
    std::vector<EntityID> entities;
    std::unordered_map<std::type_index, ComponentColumn> columns;

    [[nodiscard]] usize count() const { return entities.size(); }

    [[nodiscard]] bool has_component(std::type_index type) const;

    ComponentColumn& ensure_column(std::type_index type, usize element_size);
    ComponentColumn* find_column(std::type_index type);
    const ComponentColumn* find_column(std::type_index type) const;

    usize append_entity(EntityID id);
    /// Swap-removes `row`. Returns the entity swapped into `row`, or null when `row` was the last row.
    EntityID remove_entity(usize row);
};

ArchetypeID make_archetype_id(const std::vector<std::type_index>& sorted_types);

} // namespace fuse::ecs
