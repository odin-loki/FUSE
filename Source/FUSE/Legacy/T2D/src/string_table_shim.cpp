#include <fuse/legacy/t2d/api.hpp>
#include <fuse/legacy/string_intern_table.hpp>

#include <cstdint>

namespace {

fuse::legacy::StringInternTable g_stringTable;

} // namespace

extern "C" std::uint32_t fuse_t2d_StringTable_intern(const char* value) {
    return g_stringTable.intern(value);
}

namespace fuse::legacy::t2d {

u32 stringTableEntryCount() {
    return g_stringTable.entryCount();
}

u32 internString(const char* value) {
    return g_stringTable.intern(value);
}

const char* lookupString(u32 id) {
    return g_stringTable.lookup(id);
}

} // namespace fuse::legacy::t2d
