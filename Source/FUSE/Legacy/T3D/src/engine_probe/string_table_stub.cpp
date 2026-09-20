#include "core/stringTable.h"

#include <cstring>
#include <string>
#include <unordered_set>

namespace {

// Store interned strings in set nodes; c_str() stays valid until process exit (no rehash invalidation).
std::unordered_set<std::string> gProbeStringIntern;

StringTableEntry internCopy(const char* string, std::size_t len)
{
    if (string == nullptr || len == 0) {
        return "";
    }

    const std::string key(string, len);
    const auto [it, inserted] = gProbeStringIntern.emplace(key);
    (void)inserted;
    return it->c_str();
}

} // namespace

StringTableEntry _StringTable::insert(const char* string, bool /*caseSens*/)
{
    if (string == nullptr) {
        return _EmptyString;
    }
    if (string[0] == '\0') {
        return _EmptyString;
    }
    return internCopy(string, std::strlen(string));
}

StringTableEntry _StringTable::insertn(const char* string, S32 len, bool /*caseSens*/)
{
    if (string == nullptr || len <= 0) {
        return _EmptyString;
    }
    return internCopy(string, static_cast<std::size_t>(len));
}

StringTableEntry _StringTable::lookup(const char* string, bool /*caseSens*/)
{
    if (string == nullptr) {
        return nullptr;
    }
    const auto existing = gProbeStringIntern.find(string);
    return existing != gProbeStringIntern.end() ? existing->c_str() : nullptr;
}

StringTableEntry _StringTable::lookupn(const char* string, S32 len, bool /*caseSens*/)
{
    if (string == nullptr || len <= 0) {
        return nullptr;
    }
    const std::string key(string, static_cast<std::size_t>(len));
    const auto existing = gProbeStringIntern.find(key);
    return existing != gProbeStringIntern.end() ? existing->c_str() : nullptr;
}

_StringTable gProbeStringTableInstance;
_StringTable* StringTable = &gProbeStringTableInstance;
