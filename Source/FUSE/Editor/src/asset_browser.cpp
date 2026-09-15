#include <fuse/editor/asset_browser.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace fuse::editor {

namespace {

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string joinPath(const std::string& root, const std::string& relative) {
    if (relative.empty()) {
        return root;
    }
    return (std::filesystem::path(root) / relative).lexically_normal().string();
}

} // namespace

void AssetBrowser::init(const char* projectRoot) {
    m_projectRoot = projectRoot != nullptr ? projectRoot : "";
    m_currentDirectory.clear();
    m_searchFilter.clear();
    m_selectedPath.clear();
    m_entries.clear();
    refresh();
}

void AssetBrowser::setCurrentDirectory(const char* relativePath) {
    m_currentDirectory = relativePath != nullptr ? relativePath : "";
    m_selectedPath.clear();
    refresh();
}

void AssetBrowser::refresh() {
    m_entries.clear();

    if (m_projectRoot.empty()) {
        return;
    }

    const std::filesystem::path rootPath = joinPath(m_projectRoot, m_currentDirectory);
    std::error_code ec;
    if (!std::filesystem::exists(rootPath, ec) || !std::filesystem::is_directory(rootPath, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(rootPath, ec)) {
        if (ec) {
            break;
        }

        AssetEntry asset;
        asset.path = entry.path().lexically_relative(std::filesystem::path(m_projectRoot)).string();
        asset.name = entry.path().filename().string();

        if (entry.is_directory(ec)) {
            asset.type = AssetType::Folder;
        } else if (entry.is_regular_file(ec)) {
            asset.type = classifyFile(entry.path().extension().string());
        } else {
            asset.type = AssetType::Unknown;
        }

        const auto modified = entry.last_write_time(ec);
        if (!ec) {
            const auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
                modified - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            asset.lastModified = static_cast<u64>(sctp.time_since_epoch().count());
        }

        if (matchesFilter(asset, m_searchFilter)) {
            m_entries.push_back(std::move(asset));
        }
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const AssetEntry& lhs, const AssetEntry& rhs) {
        if (lhs.type == AssetType::Folder && rhs.type != AssetType::Folder) {
            return true;
        }
        if (lhs.type != AssetType::Folder && rhs.type == AssetType::Folder) {
            return false;
        }
        return lhs.name < rhs.name;
    });
}

void AssetBrowser::setSearchFilter(const char* filter) {
    m_searchFilter = filter != nullptr ? filter : "";
    refresh();
}

const AssetBrowser::AssetEntry* AssetBrowser::selectedEntry() const {
    if (m_selectedPath.empty()) {
        return nullptr;
    }

    for (const AssetEntry& entry : m_entries) {
        if (entry.path == m_selectedPath) {
            return &entry;
        }
    }
    return nullptr;
}

bool AssetBrowser::selectEntry(const char* relativePath) {
    if (relativePath == nullptr || relativePath[0] == '\0') {
        m_selectedPath.clear();
        return true;
    }

    for (const AssetEntry& entry : m_entries) {
        if (entry.path == relativePath) {
            m_selectedPath = relativePath;
            return true;
        }
    }
    return false;
}

const char* AssetBrowser::assetTypeLabel(AssetType type) {
    switch (type) {
    case AssetType::Folder:
        return "Folder";
    case AssetType::Mesh:
        return "Mesh";
    case AssetType::Texture:
        return "Texture";
    case AssetType::Material:
        return "Material";
    case AssetType::Scene:
        return "Scene";
    case AssetType::Audio:
        return "Audio";
    case AssetType::Script:
        return "Script";
    case AssetType::Unknown:
    default:
        return "Unknown";
    }
}

AssetBrowser::AssetType AssetBrowser::classifyFile(const std::string& extension) {
    const std::string ext = toLower(extension);

    if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".dae") {
        return AssetType::Mesh;
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds" ||
        ext == ".bmp" || ext == ".hdr") {
        return AssetType::Texture;
    }
    if (ext == ".mat" || ext == ".material" || ext == ".fusemat") {
        return AssetType::Material;
    }
    if (ext == ".scene" || ext == ".fuse" || ext == ".mis") {
        return AssetType::Scene;
    }
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
        return AssetType::Audio;
    }
    if (ext == ".lua" || ext == ".ts" || ext == ".tscript" || ext == ".js") {
        return AssetType::Script;
    }
    return AssetType::Unknown;
}

bool AssetBrowser::matchesFilter(const AssetEntry& entry, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }

    const std::string needle = toLower(filter);
    return toLower(entry.name).find(needle) != std::string::npos ||
           toLower(entry.path).find(needle) != std::string::npos;
}

} // namespace fuse::editor
