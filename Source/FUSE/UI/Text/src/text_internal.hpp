// Internal helpers shared by fuse_text's sources.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fuse::text::detail {

/// String `nameId` of a name table (Windows English, any Windows, Unicode, then Mac Roman); empty when absent.
std::string fontNameString(std::span<const std::uint8_t> nameTable, std::uint16_t nameId);

/// Reads a whole file; `utf8Path` is converted to the platform's path type (wide on Windows).
bool readFile(const std::string& utf8Path, std::vector<std::uint8_t>& out);

/// A read-only memory-mapped file (empty when open fails).
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(MappedFile&& o) noexcept;
    MappedFile& operator=(MappedFile&& o) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();
    bool open(const std::string& utf8Path);
    const std::uint8_t* data() const { return m_data; }
    std::size_t size() const { return m_size; }

private:
    void reset();
    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
    void* m_handle = nullptr; // Windows: the mapping object
};

} // namespace fuse::text::detail
