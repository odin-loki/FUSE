#include <fuse/legacy/t2d/api.hpp>

#include <cstdint>
#include <unordered_map>

using fuse::u32;

namespace {

struct StringTable {
    std::unordered_map<const char*, u32> entries;
};

StringTable g_stringTable;

} // namespace

extern "C" std::uint32_t fuse_t2d_StringTable_intern(const char* value) {
    if (!value) {
        return 0;
    }
    auto it = g_stringTable.entries.find(value);
    if (it != g_stringTable.entries.end()) {
        return it->second;
    }
    const u32 id = static_cast<u32>(g_stringTable.entries.size() + 1);
    g_stringTable.entries[value] = id;
    return id;
}

namespace fuse::legacy::t2d {

u32 stringTableEntryCount() {
    return static_cast<u32>(g_stringTable.entries.size());
}

} // namespace fuse::legacy::t2d
