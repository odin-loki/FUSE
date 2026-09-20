#include <fuse/renderer/shader/shader_compiler.hpp>

#include <fuse/renderer/shader/shader_io.hpp>

namespace fuse::renderer {

namespace {

void fillDescMetadata(CompiledShader& result, const ShaderDesc& desc) {
    result.stage = desc.stage;
    if (desc.sourcePath != nullptr) {
        result.sourcePath = desc.sourcePath;
    }
    result.entryPoint = (desc.entryPoint != nullptr && desc.entryPoint[0] != '\0') ? desc.entryPoint : "main";
    result.defineCount = desc.defineCount;
}

CompiledShader makeFailure(const ShaderDesc& desc, const std::string& message) {
    CompiledShader result;
    fillDescMetadata(result, desc);
    result.message = message;
    result.valid = false;
    return result;
}

CompiledShader makeSuccess(const ShaderDesc& desc, std::vector<u32>&& words, const char* message) {
    CompiledShader result;
    fillDescMetadata(result, desc);
    result.spirv = std::move(words);
    result.spirvHash = hashSpirvWords(result.spirv.data(), static_cast<u32>(result.spirv.size()));
    result.message = message;
    result.valid = true;
    return result;
}

} // namespace

CompiledShader ShaderCompiler::compileOffline(const ShaderDesc& desc) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return makeFailure(desc, "shader source path is required");
    }

    std::string error;
    const std::string spirvPath = spirvPathForSource(desc.sourcePath);
    std::vector<u32> words = loadSpirvFile(spirvPath.c_str(), &error);
    if (!words.empty()) {
        return makeSuccess(desc, std::move(words), "loaded offline SPIR-V");
    }

    const std::string fuseshaderPath = fuseshaderPathForSource(desc.sourcePath);
    words = loadCookedFuseshaderSpirv(fuseshaderPath.c_str(), &error);
    if (!words.empty()) {
        return makeSuccess(desc, std::move(words), "loaded cooked fuseshader SPIR-V");
    }

    return makeFailure(desc, error.empty() ? "offline SPIR-V not found" : error);
}

CompiledShader ShaderCompiler::compile(const ShaderDesc& desc) {
#if defined(FUSE_SHADER_GLSLANG)
    // Offline-first: cooked `.fuseshader` and sibling `.spv` fixtures keep CI green.
    // Runtime glslang source compile remains deferred until validator wiring lands in fuse_rhi.
    return compileOffline(desc);
#else
    return compileOffline(desc);
#endif
}

bool ShaderCompiler::watch(const ShaderDesc& desc) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return false;
    }

    WatchedEntry entry;
    entry.path = desc.sourcePath;
    entry.desc = desc;
    entry.desc.sourcePath = entry.path.c_str();

    if (!m_watch.watch(entry.path.c_str())) {
        return false;
    }

    entry.last = compileOffline(entry.desc);
    m_entries.push_back(std::move(entry));
    m_entries.back().desc.sourcePath = m_entries.back().path.c_str();
    return true;
}

u32 ShaderCompiler::pollHotReload() {
    const u32 changed = m_watch.pollChanged();
    if (changed == 0u) {
        return 0u;
    }

    u32 successes = 0;
    const u32 changedCount = m_watch.lastChangedCount();
    if (changedCount == 0u) {
        for (WatchedEntry& entry : m_entries) {
            entry.desc.sourcePath = entry.path.c_str();
            entry.last = compileOffline(entry.desc);
            if (entry.last.valid) {
                ++successes;
            }
        }
        return successes;
    }

    for (u32 i = 0; i < changedCount; ++i) {
        const char* path = m_watch.lastChangedPath(i);
        if (path == nullptr) {
            continue;
        }
        for (WatchedEntry& entry : m_entries) {
            if (entry.path == path) {
                entry.desc.sourcePath = entry.path.c_str();
                entry.last = compileOffline(entry.desc);
                if (entry.last.valid) {
                    ++successes;
                }
            }
        }
    }
    return successes;
}

u32 ShaderCompiler::watchedCount() const {
    return static_cast<u32>(m_entries.size());
}

const CompiledShader* ShaderCompiler::lastCompiled(const char* path) const {
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }

    for (const WatchedEntry& entry : m_entries) {
        if (entry.path == path) {
            return &entry.last;
        }
    }
    return nullptr;
}

} // namespace fuse::renderer
