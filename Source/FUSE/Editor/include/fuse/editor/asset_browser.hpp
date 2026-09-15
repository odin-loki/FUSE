#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Headless asset browser model (B6.9 stub — Qt grid/tree deferred to U6 chrome).
class AssetBrowser {
public:
    enum class AssetType : u8 {
        Folder = 0,
        Mesh,
        Texture,
        Material,
        Scene,
        Audio,
        Script,
        Unknown,
    };

    struct AssetEntry {
        std::string path;
        std::string name;
        AssetType type = AssetType::Unknown;
        u64 lastModified = 0;
    };

    void init(const char* projectRoot);
    void setCurrentDirectory(const char* relativePath);
    void refresh();

    void setSearchFilter(const char* filter);
    const std::string& searchFilter() const { return m_searchFilter; }

    const std::vector<AssetEntry>& entries() const { return m_entries; }
    const AssetEntry* selectedEntry() const;
    bool selectEntry(const char* relativePath);

    const std::string& projectRoot() const { return m_projectRoot; }
    const std::string& currentDirectory() const { return m_currentDirectory; }

    static const char* assetTypeLabel(AssetType type);

private:
    static AssetType classifyFile(const std::string& extension);
    static bool matchesFilter(const AssetEntry& entry, const std::string& filter);

    std::string m_projectRoot;
    std::string m_currentDirectory;
    std::string m_searchFilter;
    std::vector<AssetEntry> m_entries;
    std::string m_selectedPath;
};

} // namespace fuse::editor
