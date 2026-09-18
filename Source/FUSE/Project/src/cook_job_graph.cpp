#include <fuse/project/cook_job_graph.hpp>

#include <fuse/project/asset_cooker.hpp>
#include <fuse/log/logger.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <unordered_map>

namespace fuse::project {

namespace {

bool path_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

std::vector<CookStageRecord> make_pending_stages() {
    return {
        {CookStageKind::Import, CookStageStatus::Pending, {}},
        {CookStageKind::Process, CookStageStatus::Pending, {}},
        {CookStageKind::Pack, CookStageStatus::Pending, {}},
    };
}

void skip_remaining_stages(CookJob& job, std::size_t from_index, const char* reason) {
    for (std::size_t i = from_index; i < job.stages.size(); ++i) {
        if (job.stages[i].status == CookStageStatus::Pending) {
            job.stages[i].status = CookStageStatus::Skipped;
            job.stages[i].note = reason;
        }
    }
}

std::string format_stage_summary(const CookJob& job) {
    std::ostringstream out;
    for (std::size_t i = 0; i < job.stages.size(); ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << cookStageKindName(job.stages[i].kind) << ':'
            << cookStageStatusName(job.stages[i].status);
    }
    return out.str();
}

} // namespace

const char* cookStageKindName(CookStageKind kind) {
    switch (kind) {
    case CookStageKind::Import:
        return "import";
    case CookStageKind::Process:
        return "process";
    case CookStageKind::Pack:
        return "pack";
    }
    return "unknown";
}

u8 cookStageIndex(CookStageKind kind) {
    switch (kind) {
    case CookStageKind::Import:
        return 0;
    case CookStageKind::Process:
        return 1;
    case CookStageKind::Pack:
        return 2;
    }
    return 0;
}

CookStageKind nextCookStageKind(CookStageKind kind) {
    switch (kind) {
    case CookStageKind::Import:
        return CookStageKind::Process;
    case CookStageKind::Process:
        return CookStageKind::Pack;
    case CookStageKind::Pack:
        return CookStageKind::Pack;
    }
    return CookStageKind::Pack;
}

bool isTerminalCookStage(CookStageKind kind) {
    return kind == CookStageKind::Pack;
}

std::size_t pendingCookStageCount(const CookJob& job) {
    std::size_t count = 0;
    for (const CookStageRecord& stage : job.stages) {
        if (stage.status == CookStageStatus::Pending) {
            ++count;
        }
    }
    return count;
}

std::size_t firstPendingCookStageIndex(const CookJob& job) {
    for (std::size_t i = 0; i < job.stages.size(); ++i) {
        if (job.stages[i].status == CookStageStatus::Pending) {
            return i;
        }
    }
    return job.stages.size();
}

const char* cookStageStatusName(CookStageStatus status) {
    switch (status) {
    case CookStageStatus::Pending:
        return "pending";
    case CookStageStatus::Ok:
        return "ok";
    case CookStageStatus::Failed:
        return "failed";
    case CookStageStatus::Skipped:
        return "skipped";
    }
    return "unknown";
}

void CookJobGraph::clear() {
    m_jobs.clear();
    m_dep_graph.clear();
}

void CookJobGraph::add_edge_(const std::string& from_job_id, const std::string& to_job_id) {
    (void)m_dep_graph.add_edge(from_job_id, to_job_id);
}

CookJob* CookJobGraph::find_job_(const std::string& job_id) {
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [&](const CookJob& job) {
        return job.id == job_id;
    });
    return it == m_jobs.end() ? nullptr : &(*it);
}

const CookJob* CookJobGraph::find_job_(const std::string& job_id) const {
    const auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [&](const CookJob& job) {
        return job.id == job_id;
    });
    return it == m_jobs.end() ? nullptr : &(*it);
}

std::string CookJobGraph::resolve_job_id_(const std::string& path) const {
    for (const CookJob& job : m_jobs) {
        if (job.output_path == path || job.source_path == path || job.id == path) {
            return job.id;
        }
    }
    return {};
}

void CookJobGraph::build_from_manifest(const CookManifest& manifest) {
    clear();

    for (const CookManifestEntry& entry : manifest.assets) {
        CookJob job;
        job.id = entry.output_path;
        job.kind = entry.kind;
        job.source_path = entry.source_path;
        job.output_path = entry.output_path;
        job.stages = make_pending_stages();
        m_dep_graph.add_node(job.id);
        m_jobs.push_back(std::move(job));
    }

    for (const CookManifestEntry& entry : manifest.assets) {
        CookJob* job = find_job_(entry.output_path);
        if (!job) {
            continue;
        }

        for (const std::string& dependency : entry.dependencies) {
            const std::string from_id = resolve_job_id_(dependency);
            if (!from_id.empty() && from_id != job->id) {
                add_edge_(from_id, job->id);
                if (std::find(job->dependency_ids.begin(), job->dependency_ids.end(), from_id) ==
                    job->dependency_ids.end()) {
                    job->dependency_ids.push_back(from_id);
                }
            }
        }

        for (const CookJob& producer : m_jobs) {
            if (producer.id != job->id && producer.output_path == job->source_path) {
                add_edge_(producer.id, job->id);
                if (std::find(job->dependency_ids.begin(), job->dependency_ids.end(), producer.id) ==
                    job->dependency_ids.end()) {
                    job->dependency_ids.push_back(producer.id);
                }
            }
        }
    }
}

CookJobGraphOrderResult CookJobGraph::topological_order() const {
    return m_dep_graph.topological_order();
}

CookDependencyLayerResult CookJobGraph::topological_layers() const {
    return m_dep_graph.topological_layers();
}

bool CookJobGraph::has_cycle() const {
    return m_dep_graph.has_cycle();
}

CookDependencyCycleResult CookJobGraph::cycle_edges() const {
    return m_dep_graph.detect_cycle_edges();
}

std::vector<std::string> CookJobGraph::ready_job_ids(const std::vector<std::string>& completed_job_ids) const {
    if (m_jobs.empty()) {
        return {};
    }

    std::unordered_map<std::string, bool> completed;
    for (const std::string& job_id : completed_job_ids) {
        completed[job_id] = true;
    }

    std::vector<std::string> ready;
    for (const CookJob& job : m_jobs) {
        if (completed.find(job.id) != completed.end()) {
            continue;
        }

        bool all_deps_met = true;
        for (const std::string& dependency_id : job.dependency_ids) {
            if (completed.find(dependency_id) == completed.end()) {
                all_deps_met = false;
                break;
            }
        }

        if (all_deps_met) {
            ready.push_back(job.id);
        }
    }

    std::sort(ready.begin(), ready.end());
    return ready;
}

CookInvalidationClosureResult CookJobGraph::invalidation_closure(const std::string& from_job_id) const {
    if (m_jobs.empty()) {
        CookInvalidationClosureResult result;
        result.ok = false;
        return result;
    }
    return m_dep_graph.transitive_successors(from_job_id);
}

CookInvalidationClosureResult CookJobGraph::upstream_invalidation_closure(const std::string& to_job_id) const {
    if (m_jobs.empty()) {
        CookInvalidationClosureResult result;
        result.ok = false;
        return result;
    }
    return m_dep_graph.transitive_predecessors(to_job_id);
}

CookInvalidationClosureResult CookJobGraph::merged_invalidation_closure(
    const std::vector<std::string>& from_job_ids) const {
    if (m_jobs.empty()) {
        CookInvalidationClosureResult result;
        result.ok = false;
        return result;
    }
    return m_dep_graph.merged_invalidation_closure(from_job_ids);
}

bool CookJobGraph::run_job_stages_(CookJob& job, AssetCooker& cooker, const CookManifest& manifest,
                                   CookJobGraphExecuteResult& result) {
    CookStageRecord& import_stage = job.stages[0];
    if (job.source_path.empty() || job.output_path.empty()) {
        import_stage.status = CookStageStatus::Failed;
        import_stage.note = "missing input or output path";
        skip_remaining_stages(job, 1, "import failed");
        result.failed_job_id = job.id;
        result.failed_stage = CookStageKind::Import;
        result.failure_note = import_stage.note;
        job.ok = false;
        return false;
    }

    if (!path_exists(job.source_path)) {
        import_stage.status = CookStageStatus::Failed;
        import_stage.note = "source file not found";
        skip_remaining_stages(job, 1, "import failed");
        result.failed_job_id = job.id;
        result.failed_stage = CookStageKind::Import;
        result.failure_note = import_stage.note;
        job.ok = false;
        return false;
    }

    import_stage.status = CookStageStatus::Ok;
    import_stage.note = "source validated";

    CookStageRecord& process_stage = job.stages[1];
    process_stage.status = CookStageStatus::Ok;
    process_stage.note = std::string("stub ") + cookAssetKindName(job.kind) + " process";

    CookManifestEntry entry;
    entry.kind = job.kind;
    entry.source_path = job.source_path;
    entry.output_path = job.output_path;
    entry.dependencies = job.dependency_ids;

    CookStageRecord& pack_stage = job.stages[2];
    const CookRecord packed = cooker.cook_entry(entry, manifest);
    if (!packed.ok) {
        pack_stage.status = CookStageStatus::Failed;
        pack_stage.note = packed.note.empty() ? cookStatusName(packed.status) : packed.note;
        result.failed_job_id = job.id;
        result.failed_stage = CookStageKind::Pack;
        result.failure_note = pack_stage.note;
        job.ok = false;
        return false;
    }

    pack_stage.status = CookStageStatus::Ok;
    pack_stage.note = packed.note.empty() ? "packed" : packed.note;
    job.content_hash = packed.content_hash;
    job.cache_hit = packed.cache_hit;
    job.ok = true;
    return true;
}

CookJobGraphExecuteResult CookJobGraph::execute(AssetCooker& cooker, const CookManifest& manifest) {
    CookJobGraphExecuteResult result;
    result.edges = m_dep_graph.edges();
    result.jobs = m_jobs;

    if (m_jobs.empty()) {
        result.ok = true;
        result.summary = "cooked 0/0 jobs via stage graph (stub)";
        return result;
    }

    const CookJobGraphOrderResult order = topological_order();
    result.execution_order = order.order;

    if (order.cycle_detected) {
        result.cycle_detected = true;
        result.ok = false;
        result.failure_note = "dependency cycle detected";
        result.summary = "cook job graph rejected: dependency cycle";
        return result;
    }

    usize ok_count = 0;
    for (const std::string& job_id : result.execution_order) {
        CookJob* job = find_job_(job_id);
        if (!job) {
            continue;
        }

        bool dependency_failed = false;
        for (const std::string& dependency_id : job->dependency_ids) {
            const CookJob* dependency = find_job_(dependency_id);
            if (dependency != nullptr && !dependency->ok) {
                dependency_failed = true;
                job->skip_note = "dependency failed: " + dependency_id;
                break;
            }
        }

        if (dependency_failed) {
            job->skipped = true;
            job->ok = false;
            for (CookStageRecord& stage : job->stages) {
                stage.status = CookStageStatus::Skipped;
                stage.note = job->skip_note;
            }
            if (result.failed_job_id.empty()) {
                result.failed_job_id = job->id;
                result.failed_stage = CookStageKind::Import;
                result.failure_note = job->skip_note;
            }
            continue;
        }

        if (run_job_stages_(*job, cooker, manifest, result)) {
            ++ok_count;
        }
    }

    result.jobs = m_jobs;
    result.ok = ok_count == m_jobs.size();
    std::ostringstream summary;
    summary << "cooked " << ok_count << "/" << m_jobs.size() << " jobs via stage graph (stub)";
    if (!result.ok && !result.failed_job_id.empty()) {
        summary << " — failed at " << result.failed_job_id << ' '
                << cookStageKindName(result.failed_stage) << ": " << result.failure_note;
    }
    result.summary = summary.str();
    return result;
}

CookBatchResult cookBatchFromJobGraphResult(const CookJobGraphExecuteResult& graph_result) {
    CookBatchResult batch;
    batch.ok = graph_result.ok;
    batch.summary = graph_result.summary;
    batch.records.reserve(graph_result.jobs.size());

    for (const CookJob& job : graph_result.jobs) {
        CookRecord record;
        record.kind = job.kind;
        record.source_path = job.source_path;
        record.output_path = job.output_path;
        record.ok = job.ok;
        record.content_hash = job.content_hash;
        record.cache_hit = job.cache_hit;
        record.note = job.skipped ? job.skip_note : format_stage_summary(job);

        if (job.skipped) {
            record.status = CookStatus::InvalidInput;
        } else if (job.ok) {
            record.status = CookStatus::Ok;
        } else {
            const auto failed_stage = std::find_if(job.stages.begin(), job.stages.end(), [](const CookStageRecord& stage) {
                return stage.status == CookStageStatus::Failed;
            });
            if (failed_stage != job.stages.end() && failed_stage->kind == CookStageKind::Import &&
                failed_stage->note.find("not found") != std::string::npos) {
                record.status = CookStatus::SourceMissing;
            } else if (job.kind == CookAssetKind::Shader) {
                record.status = CookStatus::UnsupportedKind;
            } else {
                record.status = CookStatus::OutputError;
            }
        }

        batch.records.push_back(std::move(record));
    }

    return batch;
}

} // namespace fuse::project
