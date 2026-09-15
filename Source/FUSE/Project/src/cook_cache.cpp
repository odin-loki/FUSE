#include <fuse/project/cook_cache.hpp>

#include <fstream>
#include <sstream>

namespace fuse::project {

namespace {

std::string escapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

} // namespace

CookCacheEntry* CookCache::find_entry_(u64 content_hash) {
    for (CookCacheEntry& entry : m_entries) {
        if (entry.content_hash == content_hash) {
            return &entry;
        }
    }
    return nullptr;
}

const CookCacheEntry* CookCache::find_entry_(u64 content_hash) const {
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.content_hash == content_hash) {
            return &entry;
        }
    }
    return nullptr;
}

CookCacheLookup CookCache::lookup(u64 content_hash, CookCacheEntry* out_entry) {
    if (!is_valid_cook_cache_key(content_hash)) {
        return CookCacheLookup::Miss;
    }

    const CookCacheEntry* entry = find_entry_(content_hash);
    if (!entry) {
        ++m_stats.misses;
        return CookCacheLookup::Miss;
    }

    ++m_stats.hits;
    if (out_entry) {
        *out_entry = *entry;
    }
    return CookCacheLookup::Hit;
}

void CookCache::store(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_key(entry.content_hash) || entry.source_path.empty() || entry.output_path.empty()) {
        return;
    }

    if (CookCacheEntry* existing = find_entry_(entry.content_hash)) {
        *existing = entry;
        return;
    }
    m_entries.push_back(entry);
}

bool CookCache::invalidate(u64 content_hash) {
    if (!is_valid_cook_cache_key(content_hash)) {
        return false;
    }

    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->content_hash == content_hash) {
            m_entries.erase(it);
            ++m_stats.invalidations;
            return true;
        }
    }
    return false;
}

namespace {

const CookJob* find_job_by_id(const std::vector<CookJob>& jobs, const std::string& job_id) {
    for (const CookJob& job : jobs) {
        if (job.id == job_id) {
            return &job;
        }
    }
    return nullptr;
}

} // namespace

u32 CookCache::invalidate_downstream_of(const std::string& output_path,
                                        const std::vector<CookJobDependencyEdge>& edges,
                                        const std::vector<CookJob>& jobs) {
    u32 removed = invalidate_source(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {
            continue;
        }

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {
            continue;
        }

        removed += invalidate_source(to_job->source_path);
        removed += invalidate_downstream_of(to_job->output_path, edges, jobs);
    }

    return removed;
}

std::vector<std::string> CookCache::invalidate_stale_upstream_hashes(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) {
    std::vector<std::string> invalidated;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        const u64 current_upstream = pair.second;

        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->source_path == source_path && it->upstream_hash != current_upstream) {
                invalidated.push_back(source_path);
                it = m_entries.erase(it);
                ++m_stats.invalidations;
            } else {
                ++it;
            }
        }
    }
    return invalidated;
}

u32 CookCache::invalidate_source(const std::string& source_path) {
    u32 removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->source_path == source_path) {
            it = m_entries.erase(it);
            ++removed;
            ++m_stats.invalidations;
        } else {
            ++it;
        }
    }
    return removed;
}

void CookCache::invalidate_all() {
    const u32 removed = static_cast<u32>(m_entries.size());
    if (removed > 0) {
        m_stats.invalidations += removed;
    }
    m_entries.clear();
}

void CookCache::clear() {
    m_entries.clear();
    m_stats = {};
}

bool CookCache::save(const std::string& path) const {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"entries\": [\n";

    for (usize i = 0; i < m_entries.size(); ++i) {
        const CookCacheEntry& entry = m_entries[i];
        out << "    {\n";
        out << "      \"contentHash\": " << entry.content_hash << ",\n";
        out << "      \"upstreamHash\": " << entry.upstream_hash << ",\n";
        out << "      \"outputPath\": \"" << escapeJson(entry.output_path) << "\",\n";
        out << "      \"sourcePath\": \"" << escapeJson(entry.source_path) << "\",\n";
        out << "      \"kind\": \"" << cookAssetKindName(entry.kind) << "\"\n";
        out << "    }";
        if (i + 1 < m_entries.size()) {
            out << ',';
        }
        out << '\n';
    }

    out << "  ]\n}\n";

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << out.str();
    return static_cast<bool>(file);
}

bool CookCache::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string contents = buffer.str();

    clear();

    std::size_t cursor = 0;
    while (cursor < contents.size()) {
        const std::size_t objectStart = contents.find('{', cursor);
        if (objectStart == std::string::npos) {
            break;
        }

        std::size_t depth = 0;
        std::size_t objectEnd = objectStart;
        for (std::size_t i = objectStart; i < contents.size(); ++i) {
            if (contents[i] == '{') {
                ++depth;
            } else if (contents[i] == '}') {
                --depth;
                if (depth == 0) {
                    objectEnd = i;
                    break;
                }
            }
        }

        const std::string_view objectBody(contents.data() + objectStart, objectEnd - objectStart + 1);
        if (objectBody.find("\"contentHash\"") == std::string_view::npos) {
            cursor = objectEnd + 1;
            continue;
        }

        CookCacheEntry entry;
        const auto readStringField = [&](std::string_view key, std::string& value) {
            const std::string needle = std::string("\"") + std::string(key) + "\":";
            const std::size_t keyPos = objectBody.find(needle);
            if (keyPos == std::string_view::npos) {
                return;
            }
            std::size_t fieldCursor = keyPos + needle.size();
            while (fieldCursor < objectBody.size() && objectBody[fieldCursor] != '"') {
                ++fieldCursor;
            }
            if (fieldCursor >= objectBody.size()) {
                return;
            }
            ++fieldCursor;
            std::size_t end = fieldCursor;
            while (end < objectBody.size() && objectBody[end] != '"') {
                ++end;
            }
            value = std::string(objectBody.substr(fieldCursor, end - fieldCursor));
        };

        const std::string hashNeedle = "\"contentHash\":";
        const std::size_t hashPos = objectBody.find(hashNeedle);
        if (hashPos != std::string_view::npos) {
            std::size_t hashCursor = hashPos + hashNeedle.size();
            while (hashCursor < objectBody.size() && !std::isdigit(static_cast<unsigned char>(objectBody[hashCursor]))) {
                ++hashCursor;
            }
            entry.content_hash = 0;
            while (hashCursor < objectBody.size() && std::isdigit(static_cast<unsigned char>(objectBody[hashCursor]))) {
                entry.content_hash = entry.content_hash * 10 +
                                     static_cast<u64>(objectBody[hashCursor] - '0');
                ++hashCursor;
            }
        }

        const std::string upstreamNeedle = "\"upstreamHash\":";
        const std::size_t upstreamPos = objectBody.find(upstreamNeedle);
        if (upstreamPos != std::string_view::npos) {
            std::size_t upstreamCursor = upstreamPos + upstreamNeedle.size();
            while (upstreamCursor < objectBody.size() &&
                   !std::isdigit(static_cast<unsigned char>(objectBody[upstreamCursor]))) {
                ++upstreamCursor;
            }
            entry.upstream_hash = 0;
            while (upstreamCursor < objectBody.size() &&
                   std::isdigit(static_cast<unsigned char>(objectBody[upstreamCursor]))) {
                entry.upstream_hash = entry.upstream_hash * 10 +
                                      static_cast<u64>(objectBody[upstreamCursor] - '0');
                ++upstreamCursor;
            }
        }

        readStringField("outputPath", entry.output_path);
        readStringField("sourcePath", entry.source_path);

        std::string kindText;
        readStringField("kind", kindText);
        if (kindText == "mesh") {
            entry.kind = CookAssetKind::Mesh;
        } else if (kindText == "texture") {
            entry.kind = CookAssetKind::Texture;
        } else if (kindText == "audio") {
            entry.kind = CookAssetKind::Audio;
        } else if (kindText == "shader") {
            entry.kind = CookAssetKind::Shader;
        }

        if (entry.content_hash != 0 && !entry.output_path.empty()) {
            m_entries.push_back(std::move(entry));
        }

        cursor = objectEnd + 1;
    }

    return !m_entries.empty();
}

} // namespace fuse::project
