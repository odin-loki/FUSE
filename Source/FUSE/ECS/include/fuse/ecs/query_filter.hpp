#pragma once

#include <fuse/ecs/archetype.hpp>

#include <typeindex>
#include <vector>

namespace fuse::ecs {

/// Required component types for archetype-scoped registry queries.
template <typename... Ts>
struct With {};

/// Excluded component types for archetype-scoped registry queries.
template <typename... Ts>
struct Without {};

struct QueryFilter {
    std::vector<std::type_index> with;
    std::vector<std::type_index> without;
};

/// True when the filter carries no With or Without constraints.
[[nodiscard]] bool query_filter_empty(const QueryFilter& filter);

/// True when any type appears in both `with` and `without`.
[[nodiscard]] bool query_filter_has_conflict(const QueryFilter& filter);

/// True when the filter can be evaluated (no With/Without conflict).
[[nodiscard]] bool query_filter_is_runnable(const QueryFilter& filter);

/// True when both filters carry the same With/Without type sets (order-independent).
[[nodiscard]] bool query_filter_equal(const QueryFilter& lhs, const QueryFilter& rhs);

template <typename... WithTs, typename... WithoutTs>
[[nodiscard]] QueryFilter make_query_filter(With<WithTs...> = {}, Without<WithoutTs...> = {}) {
    QueryFilter filter;
    (filter.with.push_back(std::type_index(typeid(WithTs))), ...);
    (filter.without.push_back(std::type_index(typeid(WithoutTs))), ...);
    return filter;
}

/// Count archetypes whose component signature satisfies `filter` (ignores entity row count).
[[nodiscard]] u32 count_matching_archetypes(const std::vector<Archetype>& archetypes, const QueryFilter& filter);

/// Sum entity row counts for archetypes whose signature satisfies `filter`.
[[nodiscard]] u32 count_matching_entities(const std::vector<Archetype>& archetypes, const QueryFilter& filter);

/// True when at least one archetype signature satisfies `filter` (empty table or conflicting filters yield false).
[[nodiscard]] bool has_matching_archetypes(const std::vector<Archetype>& archetypes, const QueryFilter& filter);

/// True when at least one entity row lives in an archetype whose signature satisfies `filter`.
[[nodiscard]] bool has_matching_entities(const std::vector<Archetype>& archetypes, const QueryFilter& filter);

/// Preflight result for archetype-table queries — bundles runnable/conflict and empty-table guards.
struct QueryFilterPreflight {
    bool runnable = false;
    /// True when any type appears in both `with` and `without`.
    bool has_conflict = false;
    bool empty_table = true;
    /// True when iteration should be skipped (conflict, empty table, or zero matching entity rows).
    bool skipped = true;
    u32 matching_archetypes = 0;
    u32 matching_entities = 0;

    /// True when the filter is runnable and at least one archetype signature satisfies it.
    [[nodiscard]] bool can_match() const { return runnable && !empty_table && matching_archetypes > 0; }

    /// True when entity iteration would visit at least one row.
    [[nodiscard]] bool can_iterate() const { return runnable && matching_entities > 0; }

    /// True when iteration should be skipped (conflict, empty table, or zero matching entities).
    [[nodiscard]] bool should_skip() const { return skipped; }

    /// True when archetype/entity counts can be evaluated without conflict or empty-table guards.
    [[nodiscard]] bool can_count() const { return runnable && !empty_table; }

    /// True when at least one archetype signature satisfies `filter` (ignores entity row count).
    [[nodiscard]] bool has_signature_match() const { return matching_archetypes > 0; }
};

/// Evaluate runnable/conflict guards without an archetype table (counts remain zero).
[[nodiscard]] QueryFilterPreflight preflight_query_filter(const QueryFilter& filter);

/// Evaluate runnable/conflict and empty-table guards plus matching archetype/entity counts.
[[nodiscard]] QueryFilterPreflight preflight_query_filter(const std::vector<Archetype>& archetypes,
                                                          const QueryFilter& filter);

/// True when `each_query` / `each_query_parallel` would visit zero entities for this table + filter.
[[nodiscard]] bool should_skip_query_iteration(const std::vector<Archetype>& archetypes, const QueryFilter& filter);

/// Convenience guard — `preflight_query_filter(archetypes, filter).can_iterate()`.
[[nodiscard]] bool can_iterate_query_filter(const std::vector<Archetype>& archetypes, const QueryFilter& filter);

/// True when `archetype` contains every `with` type and none of the `without` types.
[[nodiscard]] bool archetype_matches(const Archetype& archetype, const QueryFilter& filter);

/// Compile-time With/Without convenience over `make_query_filter` + `archetype_matches`.
template <typename... WithTs>
[[nodiscard]] bool archetype_matches(const Archetype& archetype, With<WithTs...>) {
    return archetype_matches(archetype, make_query_filter(With<WithTs...>{}));
}

template <typename... WithoutTs>
[[nodiscard]] bool archetype_matches(const Archetype& archetype, Without<WithoutTs...> exclude) {
    return archetype_matches(archetype, make_query_filter(With<>{}, exclude));
}

template <typename... WithTs, typename... WithoutTs>
[[nodiscard]] bool archetype_matches(const Archetype& archetype, With<WithTs...>, Without<WithoutTs...>) {
    return archetype_matches(archetype, make_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}));
}

} // namespace fuse::ecs
