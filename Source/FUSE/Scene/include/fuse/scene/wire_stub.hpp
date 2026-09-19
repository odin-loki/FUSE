#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::scene {

/// Parsed metadata from converter-emitted `__fuse.wire|kind|owner|ref` entity names.
struct WireStubRef {
    std::string kind;
    std::string owner;
    std::string value;
    bool valid = false;
};

[[nodiscard]] bool isWireStubEntityName(const std::string& entityName);
[[nodiscard]] WireStubRef parseWireStubEntityName(const std::string& entityName);

} // namespace fuse::scene
