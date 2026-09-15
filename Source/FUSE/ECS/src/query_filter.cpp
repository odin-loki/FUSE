#include <fuse/ecs/query_filter.hpp>

namespace fuse::ecs {

bool archetype_matches(const Archetype& archetype, const QueryFilter& filter) {
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
