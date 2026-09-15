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

template <typename... WithTs, typename... WithoutTs>
[[nodiscard]] QueryFilter make_query_filter(With<WithTs...> = {}, Without<WithoutTs...> = {}) {
    QueryFilter filter;
    (filter.with.push_back(std::type_index(typeid(WithTs))), ...);
    (filter.without.push_back(std::type_index(typeid(WithoutTs))), ...);
    return filter;
}

/// True when `archetype` contains every `with` type and none of the `without` types.
[[nodiscard]] bool archetype_matches(const Archetype& archetype, const QueryFilter& filter);

/// Compile-time With/Without convenience over `make_query_filter` + `archetype_matches`.
template <typename... WithTs>
[[nodiscard]] bool archetype_matches(const Archetype& archetype, With<WithTs...>) {
    return archetype_matches(archetype, make_query_filter(With<WithTs...>{}));
}

template <typename... WithTs, typename... WithoutTs>
[[nodiscard]] bool archetype_matches(const Archetype& archetype, With<WithTs...>, Without<WithoutTs...>) {
    return archetype_matches(archetype, make_query_filter(With<WithTs...>{}, Without<WithoutTs...>{}));
}

} // namespace fuse::ecs
