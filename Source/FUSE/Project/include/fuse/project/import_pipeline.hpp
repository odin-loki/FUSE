#pragma once

#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/manifest.hpp>

namespace fuse::project {

/// Project-scoped import/cook pipeline — plans and executes offline cooks (B7.9 stub).
class ImportPipeline {
public:
    void set_project_root(const std::string& root);
    [[nodiscard]] const std::string& project_root() const { return m_projectRoot; }

    CookBatchResult plan_from_manifest(const CookManifest& manifest);
    CookBatchResult execute(bool dry_run = false);

    AssetGraph& graph() { return m_graph; }
    const AssetGraph& graph() const { return m_graph; }
    const CookManifest& planned_manifest() const { return m_plannedManifest; }

    static CookBatchResult planForProject(const ProjectManifest& project, const std::string& project_dir);

private:
    std::string m_projectRoot;
    CookManifest m_plannedManifest;
    AssetGraph m_graph;
    AssetCooker m_cooker;
};

} // namespace fuse::project
