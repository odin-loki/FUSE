#include <fuse/ai/uaisk_tree_reload.hpp>

#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>

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

const TreeFileWatchEntry* TreeFileWatchRegistry::entryFor(std::string_view path) const {
    const auto it = m_watches.find(std::string(path));
    return (it != m_watches.end()) ? &it->second : nullptr;
}

} // namespace fuse::ai::uaisk
