#pragma once

#include <fuse/project/cook_dependency_graph.hpp>
#include <fuse/project/cook_manifest.hpp>

#include <string>
#include <vector>

namespace fuse::project {

class AssetCooker;

enum class CookStageKind : u8 {
    Import,
    Process,
    Pack,
};

enum class CookStageStatus : u8 {
    Pending,
    Ok,
    Failed,
    Skipped,
};

struct CookStageRecord {
    CookStageKind kind = CookStageKind::Import;
    CookStageStatus status = CookStageStatus::Pending;
    std::string note;
};

/// One manifest asset expanded into import → process → pack stages (B7.9 deepen stub).
struct CookJob {
    std::string id;
    CookAssetKind kind = CookAssetKind::Mesh;
    std::string source_path;
    std::string output_path;
    std::vector<std::string> dependency_ids;
    std::vector<CookStageRecord> stages;
    bool ok = false;
    bool skipped = false;
    bool cache_hit = false;
    u64 content_hash = 0;
    std::string skip_note;
};

struct CookJobGraphExecuteResult {
    bool ok = false;
    bool cycle_detected = false;
    std::vector<CookJob> jobs;
    std::vector<CookJobDependencyEdge> edges;
    std::vector<std::string> execution_order;
    std::string failed_job_id;
    CookStageKind failed_stage = CookStageKind::Import;
    std::string failure_note;
    std::string summary;
};

/// Offline cook job graph — stage pipeline + dependency ordering (B7.9 deepen stub).
class CookJobGraph {
public:
    void clear();

    void build_from_manifest(const CookManifest& manifest);

    [[nodiscard]] bool empty() const { return m_jobs.empty(); }
    [[nodiscard]] const std::vector<CookJob>& jobs() const { return m_jobs; }
    [[nodiscard]] const std::vector<CookJobDependencyEdge>& edges() const { return m_dep_graph.edges(); }
    [[nodiscard]] const CookDependencyGraph& dependency_graph() const { return m_dep_graph; }
    [[nodiscard]] CookJobGraphOrderResult topological_order() const;
    [[nodiscard]] CookDependencyLayerResult topological_layers() const;
    [[nodiscard]] bool has_cycle() const;
    [[nodiscard]] CookDependencyCycleResult cycle_edges() const;

    /// Jobs with no unmet predecessors — `completed_job_ids` may be empty (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> ready_job_ids(const std::vector<std::string>& completed_job_ids) const;

    /// Transitive downstream jobs for cache invalidation — guarded on empty graph / unknown seed (B7.9 deepen).
    [[nodiscard]] CookInvalidationClosureResult invalidation_closure(const std::string& from_job_id) const;

    /// Transitive upstream jobs for dependency reconciliation — guarded on empty graph / unknown seed (B7.9 deepen).
    [[nodiscard]] CookInvalidationClosureResult upstream_invalidation_closure(const std::string& to_job_id) const;

    /// Union of downstream closures for multiple seeds — guarded on empty graph / empty seeds / unknown seeds.
    [[nodiscard]] CookInvalidationClosureResult merged_invalidation_closure(
        const std::vector<std::string>& from_job_ids) const;

    CookJobGraphExecuteResult execute(AssetCooker& cooker, const CookManifest& manifest);

private:
    void add_edge_(const std::string& from_job_id, const std::string& to_job_id);

    [[nodiscard]] CookJob* find_job_(const std::string& job_id);
    [[nodiscard]] const CookJob* find_job_(const std::string& job_id) const;
    [[nodiscard]] std::string resolve_job_id_(const std::string& path) const;

    bool run_job_stages_(CookJob& job, AssetCooker& cooker, const CookManifest& manifest,
                         CookJobGraphExecuteResult& result);

    std::vector<CookJob> m_jobs;
    CookDependencyGraph m_dep_graph;
};

const char* cookStageKindName(CookStageKind kind);
const char* cookStageStatusName(CookStageStatus status);

/// Stage graph helpers — index, successor stage, and pending-stage probes (B7.9 deepen).
[[nodiscard]] u8 cookStageIndex(CookStageKind kind);
[[nodiscard]] CookStageKind nextCookStageKind(CookStageKind kind);
[[nodiscard]] bool isTerminalCookStage(CookStageKind kind);
[[nodiscard]] std::size_t pendingCookStageCount(const CookJob& job);
[[nodiscard]] std::size_t firstPendingCookStageIndex(const CookJob& job);

CookBatchResult cookBatchFromJobGraphResult(const CookJobGraphExecuteResult& graph_result);

} // namespace fuse::project
