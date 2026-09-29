#pragma once

#include <fuse/ecs/entity.hpp>

#include <cstddef>
#include <memory_resource>
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
///
/// `storage` draws from a std::pmr::memory_resource (new/delete unless the registry maps the component
/// type to another resource, see Registry::set_column_memory_resource), e.g. CUDA managed memory so a
/// kernel reads the column through the same pointer as host code. Copies and moves keep the source
/// column's resource.
struct ComponentColumn {
    std::type_index type{typeid(void)};
    usize element_size = 0;
    std::pmr::vector<std::byte> storage{std::pmr::new_delete_resource()};

    ComponentColumn() = default;
    ComponentColumn(std::type_index column_type, usize size, std::pmr::memory_resource* resource);
    ComponentColumn(const ComponentColumn& other);
    ComponentColumn(ComponentColumn&& other) noexcept = default;
    ComponentColumn& operator=(const ComponentColumn& other);
    ComponentColumn& operator=(ComponentColumn&& other) noexcept;
    ~ComponentColumn() = default;

    [[nodiscard]] usize count() const {
        return element_size == 0 ? 0 : storage.size() / element_size;
    }

    /// Resource `storage` allocates from.
    [[nodiscard]] std::pmr::memory_resource* memory_resource() const { return storage.get_allocator().resource(); }
    /// Moves the bytes into storage from `resource` (null = new/delete). No-op when already there.
    void rebind(std::pmr::memory_resource* resource);

    void* at(usize row);
    const void* at(usize row) const;
    void push(const void* src);
    void push_default();
    void write_at(usize row, const void* src);
    void write_default_at(usize row);
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

    /// Column of `type`, created on first use with storage from `resource` (null = new/delete).
    ComponentColumn& ensure_column(std::type_index type, usize element_size,
                                   std::pmr::memory_resource* resource = nullptr);
    ComponentColumn* find_column(std::type_index type);
    const ComponentColumn* find_column(std::type_index type) const;

    usize append_entity(EntityID id);
    /// Swap-removes `row`. Returns the entity swapped into `row`, or null when `row` was the last row.
    EntityID remove_entity(usize row);
};

ArchetypeID make_archetype_id(const std::vector<std::type_index>& sorted_types);

} // namespace fuse::ecs
