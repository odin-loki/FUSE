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
    u32 count = 0;
    for (const Archetype& archetype : archetypes) {
        if (archetype_matches(archetype, filter)) {
            ++count;
        }
    }
    return count;
}

u32 count_matching_entities(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    if (query_filter_has_conflict(filter)) {
        return 0;
    }

    u32 count = 0;
    for (const Archetype& archetype : archetypes) {
        if (archetype_matches(archetype, filter)) {
            count += static_cast<u32>(archetype.count());
        }
    }
    return count;
}

bool archetype_matches(const Archetype& archetype, const QueryFilter& filter) {
    if (query_filter_has_conflict(filter)) {
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
