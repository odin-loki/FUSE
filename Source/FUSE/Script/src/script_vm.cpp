#include <fuse/script/script_vm.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fuse::script {

bool ScriptVM::init() {
    if (m_initialized) {
        return true;
    }

    m_backend = ScriptBackendKind::Null;
    m_loadedChunks.clear();
    m_initialized = true;
    return true;
}

void ScriptVM::shutdown() {
    m_loadedChunks.clear();
    m_initialized = false;
    m_backend = ScriptBackendKind::Null;
}

ScriptLoadResult ScriptVM::load_string(const char* source, const char* chunk_name) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (source == nullptr) {
        return {ScriptLoadStatus::InvalidArgument, "source is null"};
    }

    const char* name = (chunk_name != nullptr && chunk_name[0] != '\0') ? chunk_name : "chunk";
    m_loadedChunks.emplace_back(name);
    return {ScriptLoadStatus::Ok, nullptr};
}

ScriptLoadResult ScriptVM::load_file(const char* path) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (path == nullptr || path[0] == '\0') {
        return {ScriptLoadStatus::InvalidArgument, "path is empty"};
    }

    const std::filesystem::path file_path(path);
    if (!std::filesystem::exists(file_path)) {
        return {ScriptLoadStatus::FileNotFound, path};
    }

    m_loadedChunks.emplace_back(file_path.filename().string());
    return {ScriptLoadStatus::Ok, nullptr};
}

} // namespace fuse::script
