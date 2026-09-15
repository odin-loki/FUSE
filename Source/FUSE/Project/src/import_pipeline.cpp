#include <fuse/project/import_pipeline.hpp>

#include <fuse/log/logger.hpp>

namespace fuse::project {

void ImportPipeline::set_project_root(const std::string& root) {
    m_projectRoot = root;
}

CookBatchResult ImportPipeline::plan_from_manifest(const CookManifest& manifest) {
    m_plannedManifest = manifest;
    m_graph.clear();

    for (const CookManifestEntry& entry : manifest.assets) {
        m_graph.add_asset(entry.output_path, entry.source_path);
        for (const std::string& dependency : entry.dependencies) {
            m_graph.add_dependency(entry.output_path, dependency);
        }
    }

    CookBatchResult result;
    result.ok = true;
    result.summary = "planned " + std::to_string(manifest.assets.size()) + " cook jobs (dry-run)";

    for (const CookManifestEntry& entry : manifest.assets) {
        CookRecord record;
        record.kind = entry.kind;
        record.source_path = entry.source_path;
        record.output_path = entry.output_path;
        record.status = CookStatus::Ok;
        record.ok = true;
        record.note = "planned";
        result.records.push_back(std::move(record));
    }

    return result;
}

CookBatchResult ImportPipeline::execute(bool dry_run) {
    if (m_plannedManifest.assets.empty()) {
        CookBatchResult result;
        result.ok = false;
        result.summary = "no cook plan — call plan_from_manifest first";
        return result;
    }

    if (dry_run) {
        return plan_from_manifest(m_plannedManifest);
    }

    fuse::log::info("import_pipeline: executing cook plan for %s", m_projectRoot.c_str());
    return m_cooker.cook_manifest(m_plannedManifest);
}

CookBatchResult ImportPipeline::planForProject(const ProjectManifest& project, const std::string& project_dir) {
    ImportPipeline pipeline;
    pipeline.set_project_root(project_dir);

    CookManifest manifest = makeDefaultCookManifest(project_dir);
    if (!project.defaultWorld3D.empty()) {
        CookManifestEntry levelEntry;
        levelEntry.kind = CookAssetKind::Mesh;
        levelEntry.source_path = project.defaultWorld3D;
        levelEntry.output_path = "cooked/" + project.defaultWorld3D + ".fusecook";
        manifest.assets.push_back(levelEntry);
    }

    fuse::log::info("import_pipeline: planning cooks for project '%s'", project.name.c_str());
    return pipeline.plan_from_manifest(manifest);
}

} // namespace fuse::project
