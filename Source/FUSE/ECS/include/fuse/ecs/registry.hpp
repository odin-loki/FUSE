#pragma once

#include <fuse/ecs/archetype.hpp>
#include <fuse/ecs/component.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/query_filter.hpp>
#include <fuse/jobs/parallel_for.hpp>

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

    /// Iterates entities with required types while excluding Without types.
    template <typename... WithTs, typename... WithoutTs, typename Fn>
    void each(Fn&& fn, Without<WithoutTs...> exclude);

    /// Iterates entities matching required With component types.
    template <typename... WithTs, typename Fn>
    void each_query(Fn&& fn);

    /// Iterates entities matching With types while excluding Without types.
    template <typename... WithTs, typename... WithoutTs, typename Fn>
    void each_query(Fn&& fn, Without<WithoutTs...> exclude);

    /// Parallel iteration over matching archetypes via JobScheduler::parallel_for.
    template <typename... Ts, typename Fn>
    void each_parallel(Fn&& fn, u32 batchSize = 256);

    /// Parallel iteration with With/Without component filters.
    template <typename... WithTs, typename... WithoutTs, typename Fn>
    void each_parallel(Fn&& fn, Without<WithoutTs...> exclude, u32 batchSize = 256);

    /// Parallel iteration with required With component types.
    template <typename... WithTs, typename Fn>
    void each_query_parallel(Fn&& fn, u32 batchSize = 256);

    /// Parallel iteration with With/Without component filters.
    template <typename... WithTs, typename... WithoutTs, typename Fn>
    void each_query_parallel(Fn&& fn, Without<WithoutTs...> exclude, u32 batchSize = 256);

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

    template <typename... WithTs, typename Fn>
    void each_query_impl_(const QueryFilter& filter, Fn&& fn);

    template <typename... WithTs, typename Fn>
    void each_query_parallel_impl_(const QueryFilter& filter, Fn&& fn, u32 batchSize);

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
    each_query<Ts...>(std::forward<Fn>(fn));
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each(Fn&& fn, Without<WithoutTs...> exclude) {
    each_query<WithTs...>(std::forward<Fn>(fn), exclude);
}

template <typename... WithTs, typename Fn>
void Registry::each_query(Fn&& fn) {
    (assertComponent<WithTs>(), ...);
    each_query_impl_<WithTs...>(make_query_filter(With<WithTs...>{}), std::forward<Fn>(fn));
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each_query(Fn&& fn, Without<WithoutTs...> /*exclude*/) {
    (assertComponent<WithTs>(), ...);
    (assertComponent<WithoutTs>(), ...);
    each_query_impl_<WithTs...>(make_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}), std::forward<Fn>(fn));
}

template <typename... WithTs, typename Fn>
void Registry::each_query_impl_(const QueryFilter& filter, Fn&& fn) {
    for (Archetype& archetype : m_archetypes) {
        if (!archetype_matches(archetype, filter)) {
            continue;
        }

        for (usize row = 0; row < archetype.count(); ++row) {
            EntityID id = archetype.entities[row];
            fn(id, *static_cast<WithTs*>(archetype.find_column(std::type_index(typeid(WithTs)))->at(row))...);
        }
    }
}

template <typename... Ts, typename Fn>
void Registry::each_parallel(Fn&& fn, u32 batchSize) {
    each_query_parallel<Ts...>(std::forward<Fn>(fn), batchSize);
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each_parallel(Fn&& fn, Without<WithoutTs...> exclude, u32 batchSize) {
    each_query_parallel<WithTs...>(std::forward<Fn>(fn), exclude, batchSize);
}

template <typename... WithTs, typename Fn>
void Registry::each_query_parallel(Fn&& fn, u32 batchSize) {
    (assertComponent<WithTs>(), ...);
    each_query_parallel_impl_<WithTs...>(make_query_filter(With<WithTs...>{}), std::forward<Fn>(fn), batchSize);
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each_query_parallel(Fn&& fn, Without<WithoutTs...> /*exclude*/, u32 batchSize) {
    (assertComponent<WithTs>(), ...);
    (assertComponent<WithoutTs>(), ...);
    each_query_parallel_impl_<WithTs...>(make_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}),
                                         std::forward<Fn>(fn),
                                         batchSize);
}

template <typename... WithTs, typename Fn>
void Registry::each_query_parallel_impl_(const QueryFilter& filter, Fn&& fn, u32 batchSize) {
    if (batchSize == 0) {
        batchSize = 1;
    }

    for (Archetype& archetype : m_archetypes) {
        if (!archetype_matches(archetype, filter)) {
            continue;
        }

        const usize rowCount = archetype.count();
        if (rowCount == 0) {
            continue;
        }

        jobs::parallel_for(0, static_cast<u32>(rowCount), batchSize, [&](u32 row) {
            EntityID id = archetype.entities[row];
            fn(id, *static_cast<WithTs*>(archetype.find_column(std::type_index(typeid(WithTs)))->at(row))...);
        });
    }
}

} // namespace fuse::ecs
