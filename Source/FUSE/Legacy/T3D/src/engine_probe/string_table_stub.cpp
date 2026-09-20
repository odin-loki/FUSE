#include "core/stringTable.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::vector<std::unique_ptr<std::string>> gProbeStringPool;
std::unordered_map<std::string, StringTableEntry> gProbeStringIntern;

StringTableEntry internCopy(const char* string, std::size_t len)
{
    if (string == nullptr || len == 0) {
        return "";
    }

    const std::string key(string, len);
    const auto existing = gProbeStringIntern.find(key);
    if (existing != gProbeStringIntern.end()) {
        return existing->second;
    }

    auto owned = std::make_unique<std::string>(key);
    const StringTableEntry entry = owned->c_str();
    gProbeStringIntern.emplace(*owned, entry);
    gProbeStringPool.push_back(std::move(owned));
    return entry;
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
    return existing != gProbeStringIntern.end() ? existing->second : nullptr;
}

StringTableEntry _StringTable::lookupn(const char* string, S32 len, bool /*caseSens*/)
{
    if (string == nullptr || len <= 0) {
        return nullptr;
    }
    const std::string key(string, static_cast<std::size_t>(len));
    const auto existing = gProbeStringIntern.find(key);
    return existing != gProbeStringIntern.end() ? existing->second : nullptr;
}

_StringTable gProbeStringTableInstance;
_StringTable* StringTable = &gProbeStringTableInstance;
