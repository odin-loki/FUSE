#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

/// Tracks cooked assets and source dependencies for incremental reimport (B7.9 stub).
class AssetGraph {
public:
    void add_asset(const std::string& output_path, const std::string& source_path);
    void add_dependency(const std::string& output_path, const std::string& dependency_path);

    void scan_for_changes();
    [[nodiscard]] const std::vector<std::string>& dirty_assets() const { return m_dirty; }

    void reimport_dirty(const std::string& project_dir);
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    [[nodiscard]] usize asset_count() const { return m_assets.size(); }
    void clear();

private:
    struct AssetRecord {
        std::string output_path;
        std::string source_path;
        std::vector<std::string> dependencies;
        u64 last_import_time = 0;
    };

    [[nodiscard]] AssetRecord* find_asset(const std::string& output_path);
    [[nodiscard]] const AssetRecord* find_asset(const std::string& output_path) const;
    [[nodiscard]] u64 file_mtime_ns(const std::string& path) const;

    std::vector<AssetRecord> m_assets;
    std::vector<std::string> m_dirty;
};

} // namespace fuse::project
