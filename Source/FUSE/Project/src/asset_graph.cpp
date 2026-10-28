#include <fuse/project/asset_graph.hpp>

#include <fuse/io/file_time.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

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

std::string lower_extension(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t base = slash == std::string::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < base || dot + 1 >= path.size()) {
        return {};
    }
    std::string ext = path.substr(dot);
    for (char& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext;
}

bool kind_from_extension(const std::string& ext, bool output_extension, CookAssetKind& out) {
    if (output_extension) {
        if (ext == ".fusemesh") {
            out = CookAssetKind::Mesh;
            return true;
        }
        if (ext == ".fusetex") {
            out = CookAssetKind::Texture;
            return true;
        }
        if (ext == ".fuseaudio") {
            out = CookAssetKind::Audio;
            return true;
        }
        if (ext == ".fuseshader") {
            out = CookAssetKind::Shader;
            return true;
        }
        if (ext == ".fusescript") {
            out = CookAssetKind::Script;
            return true;
        }
        return false;
    }

    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".dae") {
        out = CookAssetKind::Mesh;
        return true;
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr" ||
        ext == ".exr" || ext == ".psd") {
        out = CookAssetKind::Texture;
        return true;
    }
    if (ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3") {
        out = CookAssetKind::Audio;
        return true;
    }
    if (ext == ".glsl" || ext == ".hlsl" || ext == ".vert" || ext == ".frag" || ext == ".comp") {
        out = CookAssetKind::Shader;
        return true;
    }
    if (ext == ".lua" || ext == ".tscript") {
        out = CookAssetKind::Script;
        return true;
    }
    return false;
}

bool cook_kind_for_asset(const std::string& output_path, const std::string& source_path, CookAssetKind& out) {
    if (kind_from_extension(lower_extension(output_path), true, out)) {
        return true;
    }
    return kind_from_extension(lower_extension(source_path), false, out);
}

bool source_file_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

CookRecord cook_asset(AssetCooker& cooker,
                      CookAssetKind kind,
                      const std::string& source_path,
                      const std::string& output_path,
                      const std::vector<std::string>& dependencies) {
    switch (kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = source_path;
        desc.output_path = output_path;
        return cooker.cook_mesh(desc);
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = source_path;
        desc.output_path = output_path;
        return cooker.cook_texture(desc);
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = source_path;
        desc.output_path = output_path;
        return cooker.cook_audio(desc);
    }
    case CookAssetKind::Shader: {
        ShaderImportDesc desc;
        desc.input_path = source_path;
        desc.output_path = output_path;
        return cooker.cook_shader(desc);
    }
    case CookAssetKind::Script: {
        CookManifestEntry entry;
        entry.kind = CookAssetKind::Script;
        entry.source_path = source_path;
        entry.output_path = output_path;
        entry.dependencies = dependencies;
        return cooker.cook_entry(entry);
    }
    default:
        break;
    }

    CookRecord failed;
    failed.kind = kind;
    failed.source_path = source_path;
    failed.output_path = output_path;
    failed.status = CookStatus::UnsupportedKind;
    failed.ok = false;
    failed.note = "unknown cook kind";
    return failed;
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
    return io::fileWriteTimeNs(path);
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

void AssetGraph::reimport_dirty(const std::string& project_dir, AssetCooker* cooker) {
    if (m_dirty.empty()) {
        return;
    }

    AssetCooker fallback;
    AssetCooker& active = cooker != nullptr ? *cooker : fallback;

    const std::vector<std::string> pending = m_dirty;
    std::vector<std::string> still_dirty;
    still_dirty.reserve(pending.size());

    for (const std::string& output_path : pending) {
        AssetRecord* record = find_asset(output_path);
        if (record == nullptr) {
            fuse::log::warn("asset_graph: reimport failed for %s (project=%s, missing record)",
                            output_path.c_str(),
                            project_dir.c_str());
            still_dirty.push_back(output_path);
            continue;
        }

        if (!source_file_exists(record->source_path)) {
            fuse::log::warn("asset_graph: reimport failed for %s (project=%s, missing source)",
                            output_path.c_str(),
                            project_dir.c_str());
            still_dirty.push_back(output_path);
            continue;
        }

        CookAssetKind kind = CookAssetKind::Mesh;
        if (!cook_kind_for_asset(record->output_path, record->source_path, kind)) {
            fuse::log::warn("asset_graph: reimport failed for %s (project=%s, unknown kind)",
                            output_path.c_str(),
                            project_dir.c_str());
            still_dirty.push_back(output_path);
            continue;
        }

        // A dirty dependency must not be served from a cache hit of the previous source bytes.
        active.cache().invalidate_source(record->source_path);
        const CookRecord cooked =
            cook_asset(active, kind, record->source_path, record->output_path, record->dependencies);
        if (!cooked.ok || cooked.status != CookStatus::Ok) {
            fuse::log::warn("asset_graph: reimport failed for %s (project=%s, %s)",
                            output_path.c_str(),
                            project_dir.c_str(),
                            cooked.note.c_str());
            still_dirty.push_back(output_path);
            continue;
        }

        u64 stamped = file_mtime_ns(record->source_path);
        for (const std::string& dependency : record->dependencies) {
            const u64 dependency_mtime = file_mtime_ns(dependency);
            if (dependency_mtime > stamped) {
                stamped = dependency_mtime;
            }
        }
        record->last_import_time = stamped;
        fuse::log::info("asset_graph: reimported %s (project=%s, %s)",
                        output_path.c_str(),
                        project_dir.c_str(),
                        cooked.note.c_str());
    }

    m_dirty = std::move(still_dirty);
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

const std::string* AssetGraph::source_path_for(const std::string& output_path) const {
    const AssetRecord* record = find_asset(output_path);
    if (!record) {
        return nullptr;
    }
    return &record->source_path;
}

} // namespace fuse::project
