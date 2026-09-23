#pragma once

#include <fuse/ecs/archetype.hpp>
#include <fuse/ecs/component.hpp>
#include <fuse/ecs/detail/parallel_iteration.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/query_filter.hpp>
#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <functional>
#include <span>
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
    /// Revives a destroyed entity with exactly `id` (same index and generation), e.g. for editor
    /// undo of a delete so older history that refers to `id` stays valid. Succeeds only when the
    /// slot is free (on the free list or reserved) and its last generation is `id.generation`
    /// (the slot was not reused since `id` died); returns `EntityID::null()` otherwise.
    EntityID create_at(EntityID id);
    /// True when `create_at(id)` would succeed.
    [[nodiscard]] bool can_create_at(EntityID id) const;
    /// Destroys `id` but keeps its slot off the free list, so `create()` cannot reuse the index
    /// and only `create_at(id)` can bring it back. Pair with `release_reserved` when the revival
    /// can no longer happen (e.g. the undo step that owns it is dropped).
    void destroy_entity_reserved(EntityID id);
    /// Returns a reserved slot (destroyed via `destroy_entity_reserved`) to the free list.
    /// No-op when `id` is not a reserved slot of this registry.
    void release_reserved(EntityID id);
    [[nodiscard]] bool is_reserved(EntityID id) const;
    void destroy_entity(EntityID id);
    [[nodiscard]] bool alive(EntityID id) const;
    [[nodiscard]] usize count() const;

    /// Runtime component list of a live entity (sorted by type_index; empty when dead). Lets
    /// type-erased tools (editor inspector, serialisers) enumerate what an entity carries.
    [[nodiscard]] std::vector<std::type_index> component_types(EntityID id) const;
    /// Type-erased read access to one component of a live entity; null when absent or dead.
    [[nodiscard]] const void* get_raw(EntityID id, std::type_index type) const;

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

    /// True when the entity has every listed component type (matches `each<Ts...>` semantics).
    template <typename... Ts>
    std::enable_if_t<(sizeof...(Ts) > 1), bool> has_all(EntityID id) const;

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

    /// Chunk (span) iteration — the idiomatic single-threaded fast path. Calls
    /// `fn(std::span<const EntityID> ids, std::span<Ts>... columns)` once per matching non-empty
    /// archetype chunk; every span has the same length and `columns[i]` belongs to `ids[i]`.
    /// Columns are contiguous SoA storage, so a plain indexed loop over the spans has no per-entity
    /// indirection and auto-vectorises when its body allows. Chunks hold at most `maxChunkRows`
    /// rows (0 = whole archetype). The callback must not add/remove components or create/destroy
    /// entities (same rule as `each`).
    template <typename... Ts, typename Fn>
    void each_chunk(Fn&& fn, usize maxChunkRows = 0);

    /// Chunk iteration with With/Without component filters.
    template <typename... WithTs, typename... WithoutTs, typename Fn>
    void each_chunk(Fn&& fn, Without<WithoutTs...> exclude, usize maxChunkRows = 0);

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
    friend class RegistrySerialiser;

    struct EntityRecord {
        u32 archetype_index = 0;
        u32 row = 0;
        u32 generation = 0;
        bool alive = false;
        bool reserved = false; ///< dead, off the free list, revivable only via create_at
    };

    friend struct Archetype;

    void ensureInitialized();

    [[nodiscard]] EntityRecord* record(EntityID id);
    [[nodiscard]] const EntityRecord* record(EntityID id) const;
    void destroy_entity_(EntityID id, bool reserve);

    u32 find_or_create_archetype(const std::vector<std::type_index>& sorted_types);
    void migrate_entity(EntityID id, const std::vector<std::type_index>& target_types,
                        const std::unordered_map<std::type_index, const void*>& new_values,
                        const std::unordered_map<std::type_index, usize>& type_sizes);

    template <typename T>
    static void assertComponent() {
        static_assert(IsComponentV<T>, "T must be a plain ECS component with component_name");
        static_assert(std::is_trivially_destructible<T>::value, "components must be trivially destructible");
    }

    /// Typed base pointer of T's column in a matching archetype (columns are SoA byte storage).
    template <typename T>
    static T* column_base_(Archetype& archetype) {
        ComponentColumn* column = archetype.find_column(std::type_index(typeid(T)));
        return column != nullptr ? reinterpret_cast<T*>(column->storage.data()) : nullptr;
    }

    /// Visits every matching non-empty archetype with its ids pointer, row count and typed column
    /// base pointers (resolved once per archetype, never per row).
    template <typename... WithTs, typename Fn>
    void for_each_matching_archetype_(const QueryFilter& filter, Fn&& fn);

    template <typename... WithTs, typename Fn>
    void each_query_impl_(const QueryFilter& filter, Fn&& fn);

    template <typename... WithTs, typename Fn>
    void each_chunk_impl_(const QueryFilter& filter, Fn&& fn, usize maxChunkRows);

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
    for (const std::type_index& type : current.component_types) {
        if (const ComponentColumn* column = current.find_column(type)) {
            sizes[type] = column->element_size;
        }
    }

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
    std::sort(target_types.begin(), target_types.end());

    std::unordered_map<std::type_index, usize> sizes;
    for (const std::type_index& type : current.component_types) {
        if (const ComponentColumn* column = current.find_column(type)) {
            sizes[type] = column->element_size;
        }
    }

    migrate_entity(id, target_types, {}, sizes);
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
    if (column == nullptr || rec->row >= column->count()) {
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
    if (column == nullptr || rec->row >= column->count()) {
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
    if (rec->archetype_index >= m_archetypes.size()) {
        return false;
    }
    const Archetype& archetype = m_archetypes[rec->archetype_index];
    const ComponentColumn* column = archetype.find_column(std::type_index(typeid(T)));
    return column != nullptr && rec->row < column->count();
}

template <typename... Ts>
std::enable_if_t<(sizeof...(Ts) > 1), bool> Registry::has_all(EntityID id) const {
    (assertComponent<Ts>(), ...);

    const EntityRecord* rec = record(id);
    if (rec == nullptr) {
        return false;
    }
    if (rec->archetype_index >= m_archetypes.size()) {
        return false;
    }

    const Archetype& archetype = m_archetypes[rec->archetype_index];
    return (... && archetype.has_component(std::type_index(typeid(Ts))));
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
    each_query_impl_<WithTs...>(cached_query_filter(With<WithTs...>{}), std::forward<Fn>(fn));
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each_query(Fn&& fn, Without<WithoutTs...> /*exclude*/) {
    (assertComponent<WithTs>(), ...);
    (assertComponent<WithoutTs>(), ...);
    each_query_impl_<WithTs...>(cached_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}), std::forward<Fn>(fn));
}

template <typename... WithTs, typename Fn>
void Registry::for_each_matching_archetype_(const QueryFilter& filter, Fn&& fn) {
    for (Archetype& archetype : m_archetypes) {
        const usize rowCount = archetype.count();
        if (rowCount == 0 || !archetype_matches(archetype, filter)) {
            continue;
        }
        fn(static_cast<const EntityID*>(archetype.entities.data()), rowCount, column_base_<WithTs>(archetype)...);
    }
}

template <typename... WithTs, typename Fn>
void Registry::each_query_impl_(const QueryFilter& filter, Fn&& fn) {
    // Columns are resolved once per archetype; the row loop only indexes typed pointers, and `fn`
    // is a template parameter (no std::function / virtual dispatch), so it inlines into the loop.
    for_each_matching_archetype_<WithTs...>(filter, [&](const EntityID* ids, usize rowCount, auto*... columns) {
        for (usize row = 0; row < rowCount; ++row) {
            fn(ids[row], columns[row]...);
        }
    });
}

template <typename... Ts, typename Fn>
void Registry::each_chunk(Fn&& fn, usize maxChunkRows) {
    (assertComponent<Ts>(), ...);
    each_chunk_impl_<Ts...>(cached_query_filter(With<Ts...>{}), std::forward<Fn>(fn), maxChunkRows);
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each_chunk(Fn&& fn, Without<WithoutTs...> /*exclude*/, usize maxChunkRows) {
    (assertComponent<WithTs>(), ...);
    (assertComponent<WithoutTs>(), ...);
    each_chunk_impl_<WithTs...>(cached_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}),
                                std::forward<Fn>(fn),
                                maxChunkRows);
}

template <typename... WithTs, typename Fn>
void Registry::each_chunk_impl_(const QueryFilter& filter, Fn&& fn, usize maxChunkRows) {
    for_each_matching_archetype_<WithTs...>(filter, [&](const EntityID* ids, usize rowCount, auto*... columns) {
        const usize step = maxChunkRows == 0 ? rowCount : maxChunkRows;
        for (usize begin = 0; begin < rowCount; begin += step) {
            const usize n = std::min(step, rowCount - begin);
            fn(std::span<const EntityID>(ids + begin, n),
               std::span<std::remove_pointer_t<decltype(columns)>>(columns + begin, n)...);
        }
    });
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
    each_query_parallel_impl_<WithTs...>(cached_query_filter(With<WithTs...>{}), std::forward<Fn>(fn), batchSize);
}

template <typename... WithTs, typename... WithoutTs, typename Fn>
void Registry::each_query_parallel(Fn&& fn, Without<WithoutTs...> /*exclude*/, u32 batchSize) {
    (assertComponent<WithTs>(), ...);
    (assertComponent<WithoutTs>(), ...);
    each_query_parallel_impl_<WithTs...>(cached_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}),
                                         std::forward<Fn>(fn),
                                         batchSize);
}

template <typename... WithTs, typename Fn>
void Registry::each_query_parallel_impl_(const QueryFilter& filter, Fn&& fn, u32 batchSize) {
    batchSize = detail::normalize_batch_size(batchSize);

    for_each_matching_archetype_<WithTs...>(filter, [&](const EntityID* ids, usize rowCount, auto*... columns) {
        jobs::parallel_for(0, static_cast<u32>(rowCount), batchSize, [&](u32 row) { fn(ids[row], columns[row]...); });
    });
}

} // namespace fuse::ecs
