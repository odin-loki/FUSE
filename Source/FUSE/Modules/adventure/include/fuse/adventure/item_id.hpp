#pragma once

#include <string>

namespace fuse::adventure {

/// Datablock-style item identifier (maps to 3DAAK `inv[%data.getName()]` keys).
struct ItemId {
    std::string name;

    ItemId() = default;
    explicit ItemId(std::string name) : name(std::move(name)) {}

    bool isValid() const { return !name.empty(); }

    bool operator==(const ItemId& other) const { return name == other.name; }
    bool operator!=(const ItemId& other) const { return !(*this == other); }
};

} // namespace fuse::adventure
