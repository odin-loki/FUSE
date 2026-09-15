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

u32 count_matching_archetypes(const std::vector<Archetype>& archetypes, const QueryFilter& filter) {
    u32 count = 0;
    for (const Archetype& archetype : archetypes) {
        if (archetype_matches(archetype, filter)) {
            ++count;
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
