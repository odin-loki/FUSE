#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <map>
#include <optional>

namespace fuse::compat {

/// Bijection between a legacy SimObject id and a fuse::ecs::EntityID.
///
/// Unbound lookups return EntityID::null() (generation 0). Index 0 with a non-zero generation is a
/// real entity. Each sim object id and each entity is in at most one row. bind() replaces either
/// side's previous row, so rebinding an entity moves its sim object id. Lookups do not allocate.
class SimObjectEntityTable {
public:
    /// Bind simObjectId to entity. Drops any previous row for that id or that entity.
    /// An entity that is not valid() (generation 0, including EntityID::null()) is not stored;
    /// any existing row for simObjectId is removed.
    void bind(fuse::u32 simObjectId, fuse::ecs::EntityID entity);

    /// Entity bound to simObjectId, or EntityID::null() if unbound.
    [[nodiscard]] fuse::ecs::EntityID find(fuse::u32 simObjectId) const;

    /// Same mapping as find(). Inverse of simObjectFor() for a live row.
    [[nodiscard]] fuse::ecs::EntityID entityFor(fuse::u32 simObjectId) const;

    /// Sim object id bound to entity, or empty if unbound.
    [[nodiscard]] std::optional<fuse::u32> simObjectFor(fuse::ecs::EntityID entity) const;

    /// Drop the row for simObjectId. Returns whether a row existed.
    bool unbind(fuse::u32 simObjectId);

    /// Drop the row for entity. Returns whether a row existed.
    bool unbindEntity(fuse::ecs::EntityID entity);

    void clear();

private:
    struct EntityOrder {
        bool operator()(fuse::ecs::EntityID a, fuse::ecs::EntityID b) const {
            if (a.index != b.index) {
                return a.index < b.index;
            }
            return a.generation < b.generation;
        }
    };

    std::map<fuse::u32, fuse::ecs::EntityID> m_bySim{};
    std::map<fuse::ecs::EntityID, fuse::u32, EntityOrder> m_byEntity{};
};

} // namespace fuse::compat
