#pragma once

#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::legacy {

/// Per-dimension string intern quarantine (U3 prep for R14).
///
/// Stores owned copies so transient `const char*` buffers cannot corrupt the table.
/// Game-thread only — mirrors legacy StringTable init constraints.
class StringInternTable {
public:
    u32 intern(const char* value);
    u32 entryCount() const;
    const char* lookup(u32 id) const;

private:
    std::vector<std::string> m_storage;
    std::unordered_map<std::string, u32> m_index;
};

} // namespace fuse::legacy
