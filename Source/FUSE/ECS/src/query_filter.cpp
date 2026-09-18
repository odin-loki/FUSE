#include <fuse/ecs/query_filter.hpp>

#include <algorithm>

namespace fuse::ecs {

bool query_filter_empty(const QueryFilter& filter) {
    return filter.with.empty() && filter.without.empty();
}

bool query_filter_has_conflict(const QueryFilter& filter) {
    for (const std::type_index& type : filter.with) {
        if (std::find(filter.without.begin(), filter.without.end(), type) != filter.without.end()) {
            return true;
        }
    }
    return false;
}

bool query_filter_is_runnable(const QueryFilter& filter) {
    return !query_filter_has_conflict(filter);
}

namespace {

bool type_index_sets_equal(const std::vector<std::type_index>& lhs, const std::vector<std::type_index>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (const std::type_index& type : lhs) {
        if (std::find(rhs.begin(), rhs.end(), type) == rhs.end()) {
            return false;
        }
    }
    return true;
}

} // namespace

bool query_filter_equal(const QueryFilter& lhs, const QueryFilter& rhs) {
    return type_index_sets_equal(lhs.with, rhs.with) && type_index_sets_equal(lhs.without, rhs.without);
}

u32 count_matching_archetypes(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).matching_archetypes;
}

u32 count_matching_entities(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).matching_entities;
}

bool has_matching_archetypes(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).can_match();
}

bool has_matching_entities(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).can_iterate();
}

QueryFilterPreflight preflight_query_filter(const QueryFilter& filter) {
    QueryFilterPreflight result;
    result.has_conflict = query_filter_has_conflict(filter);
    result.runnable = !result.has_conflict;
    result.empty_table = true;
    result.skipped = true;
    return result;
}

QueryFilterPreflight preflight_query_filter(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    QueryFilterPreflight result;
    result.has_conflict = query_filter_has_conflict(filter);
    result.runnable = !result.has_conflict;
    result.empty_table = archetypes.empty();

    if (!result.runnable) {
        result.skipped = true;
        return result;
    }

    if (result.empty_table) {
        result.skipped = true;
        return result;
    }

    for (const Archetype& archetype : archetypes) {
        if (!archetype_matches(archetype, filter)) {
            continue;
        }

        ++result.matching_archetypes;
        result.matching_entities += static_cast<u32>(archetype.count());
    }

    result.skipped = result.matching_entities == 0;
    return result;
}

bool should_skip_query_iteration(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).should_skip();
}

bool should_skip_query_match(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).should_skip_match();
}

bool can_iterate_query_filter(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).can_iterate();
}

bool can_match_query_filter(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    return preflight_query_filter(archetypes, filter).can_match();
}

bool archetype_matches(const Archetype& archetype, const QueryFilter& filter) {
    if (!query_filter_is_runnable(filter)) {
        return false;
    }

    for (const std::type_index& type : filter.with) {
        if (!archetype.has_component(type)) {
            return false;
        }
    }

    for (const std::type_index& type : filter.without) {
        if (archetype.has_component(type)) {
            return false;
        }
    }

    return true;
}

} // namespace fuse::ecs
