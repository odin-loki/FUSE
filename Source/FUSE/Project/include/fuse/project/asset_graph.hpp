#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

class AssetCooker;

/// Tracks cooked assets and source dependencies for incremental reimport.
class AssetGraph {
public:
    void add_asset(const std::string& output_path, const std::string& source_path);
    void add_dependency(const std::string& output_path, const std::string& dependency_path);

    void scan_for_changes();
    [[nodiscard]] const std::vector<std::string>& dirty_assets() const { return m_dirty; }

    /// Cook dirty assets through `AssetCooker`. A null `cooker` uses a default one.
    /// `last_import_time` is stamped and that dirty entry removed only when the cook succeeds.
    void reimport_dirty(const std::string& project_dir, AssetCooker* cooker = nullptr);
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    [[nodiscard]] usize asset_count() const { return m_assets.size(); }
    [[nodiscard]] const std::string* source_path_for(const std::string& output_path) const;
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
