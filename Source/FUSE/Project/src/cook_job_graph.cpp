#include <fuse/project/cook_job_graph.hpp>

#include <fuse/project/asset_cooker.hpp>
#include <fuse/log/logger.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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
    m_edges.clear();
}

void CookJobGraph::add_edge_(const std::string& from_job_id, const std::string& to_job_id) {
    if (from_job_id.empty() || to_job_id.empty() || from_job_id == to_job_id) {
        return;
    }

    const auto duplicate = std::find_if(m_edges.begin(), m_edges.end(), [&](const CookJobDependencyEdge& edge) {
        return edge.from_job_id == from_job_id && edge.to_job_id == to_job_id;
    });
    if (duplicate != m_edges.end()) {
        return;
    }

    m_edges.push_back({from_job_id, to_job_id});
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

bool CookJobGraph::has_cycle() const {
    return topological_order_().empty() && !m_jobs.empty();
}

std::vector<std::string> CookJobGraph::topological_order_() const {
    std::unordered_map<std::string, u32> indegree;
    std::unordered_map<std::string, std::vector<std::string>> adjacency;

    for (const CookJob& job : m_jobs) {
        indegree[job.id] = 0;
        adjacency[job.id] = {};
    }

    for (const CookJobDependencyEdge& edge : m_edges) {
        if (indegree.find(edge.from_job_id) == indegree.end() ||
            indegree.find(edge.to_job_id) == indegree.end()) {
            continue;
        }
        adjacency[edge.from_job_id].push_back(edge.to_job_id);
        ++indegree[edge.to_job_id];
    }

    std::vector<std::string> queue;
    for (const auto& pair : indegree) {
        if (pair.second == 0) {
            queue.push_back(pair.first);
        }
    }
    std::sort(queue.begin(), queue.end());

    std::vector<std::string> order;
    while (!queue.empty()) {
        const std::string current = queue.front();
        queue.erase(queue.begin());
        order.push_back(current);

        for (const std::string& next : adjacency[current]) {
            auto it = indegree.find(next);
            if (it == indegree.end()) {
                continue;
            }
            if (--it->second == 0) {
                queue.push_back(next);
                std::sort(queue.begin(), queue.end());
            }
        }
    }

    if (order.size() != m_jobs.size()) {
        order.clear();
    }

    return order;
}

bool CookJobGraph::run_job_stages_(CookJob& job, AssetCooker& cooker, CookJobGraphExecuteResult& result) {
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

    CookStageRecord& pack_stage = job.stages[2];
    const CookRecord packed = cooker.cook_entry(entry);
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

CookJobGraphExecuteResult CookJobGraph::execute(AssetCooker& cooker) {
    CookJobGraphExecuteResult result;
    result.edges = m_edges;
    result.jobs = m_jobs;
    result.execution_order = topological_order_();

    if (result.execution_order.empty() && !m_jobs.empty()) {
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

        if (run_job_stages_(*job, cooker, result)) {
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
