#pragma once

#include <fuse/ecs/archetype.hpp>
#include <fuse/ecs/component.hpp>
#include <fuse/ecs/entity.hpp>

#include <algorithm>
#include <functional>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace fuse::ecs {

class Registry {
public:
    void init(usize max_entities = kMaxEntities);
    void destroy();

    EntityID create();
    void destroy_entity(EntityID id);
    [[nodiscard]] bool alive(EntityID id) const;
    [[nodiscard]] usize count() const;

    template <typename T>
    T& add(EntityID id, T value = {});

    template <typename T>
    void remove(EntityID id);

    template <typename T>
    T* get(EntityID id);

    template <typename T>
    const T* get(EntityID id) const;

    template <typename T>
    bool has(EntityID id) const;

    /// Iterates entities that have all listed component types.
    template <typename... Ts, typename Fn>
    void each(Fn&& fn);

    [[nodiscard]] usize archetype_count() const { return m_archetypes.size(); }

private:
    struct EntityRecord {
        u32 archetype_index = 0;
        u32 row = 0;
        u32 generation = 0;
        bool alive = false;
    };

    friend struct Archetype;

    [[nodiscard]] EntityRecord* record(EntityID id);
    [[nodiscard]] const EntityRecord* record(EntityID id) const;

    u32 find_or_create_archetype(const std::vector<std::type_index>& sorted_types);
    void migrate_entity(EntityID id, const std::vector<std::type_index>& target_types,
                        const std::unordered_map<std::type_index, const void*>& new_values,
                        const std::unordered_map<std::type_index, usize>& type_sizes);

    template <typename T>
    static void assertComponent() {
        static_assert(IsComponentV<T>, "T must be a plain ECS component with component_name");
        static_assert(std::is_trivially_destructible<T>::value, "components must be trivially destructible");
    }

    std::vector<EntityRecord> m_records;
    std::vector<u32> m_free_list;
    std::vector<Archetype> m_archetypes;
    std::unordered_map<u64, u32> m_archetype_lookup;
    usize m_alive_count = 0;
    usize m_max_entities = kMaxEntities;
};

template <typename T>
T& Registry::add(EntityID id, T value) {
    assertComponent<T>();

    EntityRecord* rec = record(id);
    if (rec == nullptr) {
        static T s_fallback{};
        return s_fallback;
    }

    Archetype& current = m_archetypes[rec->archetype_index];
    std::vector<std::type_index> target_types = current.component_types;
    const std::type_index tidx = std::type_index(typeid(T));

    if (current.has_component(tidx)) {
        *get<T>(id) = value;
        return *get<T>(id);
    }

    target_types.push_back(tidx);
    std::sort(target_types.begin(), target_types.end());

    std::unordered_map<std::type_index, const void*> values;
    values[tidx] = &value;

    std::unordered_map<std::type_index, usize> sizes;
    sizes[tidx] = sizeof(T);

    migrate_entity(id, target_types, values, sizes);
    return *get<T>(id);
}

template <typename T>
void Registry::remove(EntityID id) {
    assertComponent<T>();

    EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return;
    }

    Archetype& current = m_archetypes[rec->archetype_index];
    const std::type_index tidx = std::type_index(typeid(T));
    if (!current.has_component(tidx)) {
        return;
    }

    std::vector<std::type_index> target_types;
    for (const std::type_index& type : current.component_types) {
        if (type != tidx) {
            target_types.push_back(type);
        }
    }

    migrate_entity(id, target_types, {}, {});
}

template <typename T>
T* Registry::get(EntityID id) {
    assertComponent<T>();

    const EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return nullptr;
    }

    Archetype& archetype = m_archetypes[rec->archetype_index];
    ComponentColumn* column = archetype.find_column(std::type_index(typeid(T)));
    if (column == nullptr) {
        return nullptr;
    }

    return static_cast<T*>(column->at(rec->row));
}

template <typename T>
const T* Registry::get(EntityID id) const {
    assertComponent<T>();

    const EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return nullptr;
    }

    const Archetype& archetype = m_archetypes[rec->archetype_index];
    const ComponentColumn* column = archetype.find_column(std::type_index(typeid(T)));
    if (column == nullptr) {
        return nullptr;
    }

    return static_cast<const T*>(column->at(rec->row));
}

template <typename T>
bool Registry::has(EntityID id) const {
    assertComponent<T>();

    const EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return false;
    }
    return m_archetypes[rec->archetype_index].has_component(std::type_index(typeid(T)));
}

template <typename... Ts, typename Fn>
void Registry::each(Fn&& fn) {
    const std::vector<std::type_index> required = {std::type_index(typeid(Ts))...};

    for (Archetype& archetype : m_archetypes) {
        bool matches = true;
        for (const std::type_index& type : required) {
            if (!archetype.has_component(type)) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        for (usize row = 0; row < archetype.count(); ++row) {
            EntityID id = archetype.entities[row];
            fn(id, *static_cast<Ts*>(archetype.find_column(std::type_index(typeid(Ts)))->at(row))...);
        }
    }
}

} // namespace fuse::ecs
