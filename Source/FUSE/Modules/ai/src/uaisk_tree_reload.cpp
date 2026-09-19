#include <fuse/ai/uaisk_tree_reload.hpp>

#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>
#include <fuse/ai/uaisk_file_watch_os.hpp>

namespace fuse::ai::uaisk {

namespace {

u32 fnv1aHash(std::string_view content) {
    u32 hash = 2166136261u;
    for (unsigned char ch : content) {
        hash ^= static_cast<u32>(ch);
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

std::string TreeFileWatchRegistry::hashContent(std::string_view content) {
    return std::to_string(fnv1aHash(content));
}

void TreeFileWatchRegistry::watchProfile(std::string_view path, u32 profileId, std::string_view initialContent) {
    TreeFileWatchEntry entry;
    entry.path = std::string(path);
    entry.profileId = profileId;
    entry.contentHash = hashContent(initialContent);
    m_watches[entry.path] = entry;
    m_contents[entry.path] = std::string(initialContent);
}

void TreeFileWatchRegistry::watchProfileFromDisk(std::string_view path, u32 profileId) {
    const OsFileWatchStatus status = readOsFileWatchStatus(path);
    TreeFileWatchEntry entry;
    entry.path = std::string(path);
    entry.profileId = profileId;
    entry.osWatchEnabled = status.readable;
    entry.lastModifiedNs = status.lastModifiedNs;
    entry.contentHash = status.readable ? hashContent(status.content) : std::string{};
    m_watches[entry.path] = entry;
    if (status.readable) {
        m_contents[entry.path] = status.content;
    }
}

void TreeFileWatchRegistry::setContent(std::string_view path, std::string_view content) {
    m_contents[std::string(path)] = std::string(content);
}

bool TreeFileWatchRegistry::reloadProfile(const TreeFileWatchEntry& entry,
                                           BehaviorRuntime& runtime,
                                           std::string* errorOut) {
    const auto contentIt = m_contents.find(entry.path);
    if (contentIt == m_contents.end()) {
        if (errorOut != nullptr) {
            *errorOut = "missing watched content";
        }
        return false;
    }

    if (entry.path.size() >= 3 && entry.path.substr(entry.path.size() - 3) == ".cs") {
        return importCodegenProfile(entry.path, contentIt->second, entry.profileId, runtime, errorOut);
    }

    BehaviorTree tree;
    if (!loadTreeFromText(contentIt->second, tree, errorOut)) {
        return false;
    }

    runtime.registerTreeProfile(entry.profileId, tree);
    return true;
}

bool TreeFileWatchRegistry::applyOsStatus_(TreeFileWatchEntry& entry,
                                            std::string_view content,
                                            u64 lastModifiedNs,
                                            BehaviorRuntime& runtime,
                                            std::string* errorOut) {
    const std::string nextHash = hashContent(content);
    const bool mtimeChanged = !entry.osWatchEnabled || entry.lastModifiedNs != lastModifiedNs;
    if (!mtimeChanged && nextHash == entry.contentHash) {
        return false;
    }

    m_contents[entry.path] = std::string(content);
    if (!reloadProfile(entry, runtime, errorOut)) {
        return false;
    }

    entry.contentHash = nextHash;
    entry.lastModifiedNs = lastModifiedNs;
    entry.osWatchEnabled = true;
    ++entry.reloadCount;
    ++m_reloadCount;
    return true;
}

u32 TreeFileWatchRegistry::pollReloads(BehaviorRuntime& runtime, std::string* errorOut) {
    u32 reloaded = 0;
    for (auto& entryPair : m_watches) {
        TreeFileWatchEntry& entry = entryPair.second;
        const auto contentIt = m_contents.find(entry.path);
        if (contentIt == m_contents.end()) {
            continue;
        }

        const std::string nextHash = hashContent(contentIt->second);
        if (nextHash == entry.contentHash) {
            continue;
        }

        if (!reloadProfile(entry, runtime, errorOut)) {
            continue;
        }

        entry.contentHash = nextHash;
        ++entry.reloadCount;
        ++m_reloadCount;
        ++reloaded;
    }
    return reloaded;
}

u32 TreeFileWatchRegistry::pollOsFileChanges(BehaviorRuntime& runtime, std::string* errorOut) {
    u32 reloaded = 0;
    ++m_osPollCount;

    for (auto& entryPair : m_watches) {
        TreeFileWatchEntry& entry = entryPair.second;
        if (!entry.osWatchEnabled) {
            continue;
        }

        const OsFileWatchStatus status = readOsFileWatchStatus(entry.path);
        if (!status.readable) {
            if (errorOut != nullptr && !status.error.empty()) {
                *errorOut = status.error;
            }
            continue;
        }

        if (applyOsStatus_(entry, status.content, status.lastModifiedNs, runtime, errorOut)) {
            ++m_osReloadCount;
            ++reloaded;
        }
    }

    return reloaded;
}

const TreeFileWatchEntry* TreeFileWatchRegistry::entryFor(std::string_view path) const {
    const auto it = m_watches.find(std::string(path));
    return (it != m_watches.end()) ? &it->second : nullptr;
}

} // namespace fuse::ai::uaisk
