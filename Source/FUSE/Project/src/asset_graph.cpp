#include <fuse/project/asset_graph.hpp>

#include <fuse/log/logger.hpp>

#include <chrono>
#include <fstream>
#include <sstream>

#include <filesystem>

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

AssetGraph::AssetRecord* AssetGraph::find_asset(const std::string& output_path) {
    for (AssetRecord& record : m_assets) {
        if (record.output_path == output_path) {
            return &record;
        }
    }
    return nullptr;
}

const AssetGraph::AssetRecord* AssetGraph::find_asset(const std::string& output_path) const {
    for (const AssetRecord& record : m_assets) {
        if (record.output_path == output_path) {
            return &record;
        }
    }
    return nullptr;
}

u64 AssetGraph::file_mtime_ns(const std::string& path) const {
    std::error_code ec;
    const std::filesystem::path file_path(path);
    if (!std::filesystem::exists(file_path, ec)) {
        return 0;
    }

    const auto ftime = std::filesystem::last_write_time(file_path, ec);
    if (ec) {
        return 0;
    }

    const auto sctp = std::chrono::time_point_cast<std::chrono::nanoseconds>(ftime);
    return static_cast<u64>(sctp.time_since_epoch().count());
}

void AssetGraph::add_asset(const std::string& output_path, const std::string& source_path) {
    if (AssetRecord* existing = find_asset(output_path)) {
        existing->source_path = source_path;
        return;
    }

    AssetRecord record;
    record.output_path = output_path;
    record.source_path = source_path;
    record.last_import_time = file_mtime_ns(source_path);
    m_assets.push_back(std::move(record));
}

void AssetGraph::add_dependency(const std::string& output_path, const std::string& dependency_path) {
    AssetRecord* record = find_asset(output_path);
    if (!record) {
        add_asset(output_path, dependency_path);
        record = find_asset(output_path);
    }

    if (!record) {
        return;
    }

    for (const std::string& dep : record->dependencies) {
        if (dep == dependency_path) {
            return;
        }
    }
    record->dependencies.push_back(dependency_path);
}

void AssetGraph::scan_for_changes() {
    m_dirty.clear();

    for (const AssetRecord& record : m_assets) {
        const u64 source_mtime = file_mtime_ns(record.source_path);
        bool dirty = source_mtime > record.last_import_time;

        for (const std::string& dep : record.dependencies) {
            const u64 dep_mtime = file_mtime_ns(dep);
            if (dep_mtime > record.last_import_time) {
                dirty = true;
                break;
            }
        }

        if (dirty) {
            m_dirty.push_back(record.output_path);
        }
    }
}

void AssetGraph::reimport_dirty(const std::string& project_dir) {
    for (const std::string& output_path : m_dirty) {
        AssetRecord* record = find_asset(output_path);
        if (!record) {
            continue;
        }

        fuse::log::info("asset_graph: reimport stub for %s (project=%s)",
                        output_path.c_str(),
                        project_dir.c_str());
        record->last_import_time = file_mtime_ns(record->source_path);
    }

    m_dirty.clear();
}

bool AssetGraph::save(const std::string& path) const {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"assets\": [\n";

    for (usize i = 0; i < m_assets.size(); ++i) {
        const AssetRecord& record = m_assets[i];
        out << "    {\n";
        out << "      \"outputPath\": \"" << escapeJson(record.output_path) << "\",\n";
        out << "      \"sourcePath\": \"" << escapeJson(record.source_path) << "\",\n";
        out << "      \"lastImportTime\": " << record.last_import_time << ",\n";
        out << "      \"dependencies\": [";
        for (usize depIndex = 0; depIndex < record.dependencies.size(); ++depIndex) {
            if (depIndex > 0) {
                out << ", ";
            }
            out << "\"" << escapeJson(record.dependencies[depIndex]) << "\"";
        }
        out << "]\n";
        out << "    }";
        if (i + 1 < m_assets.size()) {
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

bool AssetGraph::load(const std::string& path) {
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
        if (objectBody.find("\"outputPath\"") == std::string_view::npos) {
            cursor = objectEnd + 1;
            continue;
        }

        AssetRecord record;
        const auto readField = [&](std::string_view key, std::string& value) {
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

        readField("outputPath", record.output_path);
        readField("sourcePath", record.source_path);

        const std::string timeNeedle = "\"lastImportTime\":";
        const std::size_t timePos = objectBody.find(timeNeedle);
        if (timePos != std::string_view::npos) {
            std::size_t timeCursor = timePos + timeNeedle.size();
            while (timeCursor < objectBody.size() && !std::isdigit(static_cast<unsigned char>(objectBody[timeCursor]))) {
                ++timeCursor;
            }
            record.last_import_time = 0;
            while (timeCursor < objectBody.size() && std::isdigit(static_cast<unsigned char>(objectBody[timeCursor]))) {
                record.last_import_time = record.last_import_time * 10 +
                                          static_cast<u64>(objectBody[timeCursor] - '0');
                ++timeCursor;
            }
        }

        if (!record.output_path.empty()) {
            m_assets.push_back(std::move(record));
        }

        cursor = objectEnd + 1;
    }

    return !m_assets.empty();
}

void AssetGraph::clear() {
    m_assets.clear();
    m_dirty.clear();
}

} // namespace fuse::project
