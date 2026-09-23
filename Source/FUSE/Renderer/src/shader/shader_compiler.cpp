#include <fuse/renderer/shader/shader_compiler.hpp>

#include <fuse/renderer/shader/shader_io.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fuse::renderer {

namespace {

void fillDescMetadata(CompiledShader& result, const ShaderDesc& desc) {
    result.stage = desc.stage;
    if (desc.sourcePath != nullptr) {
        result.sourcePath = desc.sourcePath;
    }
    result.entryPoint = (desc.entryPoint != nullptr && desc.entryPoint[0] != '\0') ? desc.entryPoint : "main";
    result.defines.clear();
    if (desc.defines != nullptr) {
        for (u32 i = 0; i < desc.defineCount; ++i) {
            if (desc.defines[i] != nullptr) {
                result.defines.emplace_back(desc.defines[i]);
            }
        }
    }
    result.defineCount = static_cast<u32>(result.defines.size());
    result.includePaths.clear();
    if (desc.includePaths != nullptr) {
        for (u32 i = 0; i < desc.includePathCount; ++i) {
            if (desc.includePaths[i] != nullptr) {
                result.includePaths.emplace_back(desc.includePaths[i]);
            }
        }
    }
    result.includePathCount = static_cast<u32>(result.includePaths.size());
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
    for (const std::string& define : result.defines) {
        result.spirvHash = hashMixDefineBytes(result.spirvHash, define.data(),
                                              static_cast<u32>(define.size()));
    }
    for (const std::string& includePath : result.includePaths) {
        result.spirvHash = hashMixDefineBytes(result.spirvHash, includePath.data(),
                                              static_cast<u32>(includePath.size()));
    }
    result.message = message;
    result.valid = true;
    return result;
}

const char* glslangStageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vert";
    case ShaderStage::Fragment:
        return "frag";
    case ShaderStage::Compute:
        return "comp";
    case ShaderStage::Mesh:
        return "mesh";
    case ShaderStage::Task:
        return "task";
    case ShaderStage::RayGen:
        return "rgen";
    case ShaderStage::RayMiss:
        return "rmiss";
    case ShaderStage::RayClosestHit:
        return "rchit";
    case ShaderStage::RayAnyHit:
        return "rahit";
    }
    return "vert";
}

bool isPrecompiledPath(const std::string& path) {
    auto endsWith = [&path](const char* suffix) {
        const std::string s(suffix);
        return path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    return endsWith(".spv") || endsWith(".fuseshader");
}

/// Run glslangValidator with `args` (args[0] = executable), output discarded. POSIX spawns the
/// process directly (no shell: ~10 ms less per hot reload and no quoting pitfalls).
bool runValidator(const std::vector<std::string>& args) {
#if defined(_WIN32)
    std::string command;
    for (const std::string& arg : args) {
        command += (command.empty() ? "\"" : " \"") + arg + "\"";
    }
    command = "\"" + command + " > NUL 2>&1\"";
    return std::system(command.c_str()) == 0;
#else
    std::vector<char*> argv;
    argv.reserve(args.size() + 1u);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return false;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

} // namespace

const char* ShaderCompiler::defaultValidatorPath() {
#if defined(FUSE_GLSLANG_VALIDATOR_PATH)
    return FUSE_GLSLANG_VALIDATOR_PATH;
#else
    return nullptr;
#endif
}

CompiledShader ShaderCompiler::compileWithValidator(const ShaderDesc& desc, const char* validatorPath) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return makeFailure(desc, "shader source path is required");
    }
    const char* validator = validatorPath != nullptr ? validatorPath : defaultValidatorPath();
    if (validator == nullptr || validator[0] == '\0') {
        return makeFailure(desc, "no glslangValidator configured");
    }
    const std::string source(desc.sourcePath);
    if (isPrecompiledPath(source)) {
        return compileOffline(desc);
    }

    const std::string spirvPath = spirvPathForSource(desc.sourcePath);
    const std::string tempPath = spirvPath + ".tmp";
    std::vector<std::string> args = {validator, "-V", "-S", glslangStageName(desc.stage)};
    if (desc.entryPoint != nullptr && desc.entryPoint[0] != '\0' && std::string(desc.entryPoint) != "main") {
        args.insert(args.end(), {"-e", desc.entryPoint, "--source-entrypoint", "main"});
    }
    for (u32 i = 0; desc.defines != nullptr && i < desc.defineCount; ++i) {
        if (desc.defines[i] != nullptr && desc.defines[i][0] != '\0') {
            args.push_back(std::string("-D") + desc.defines[i]);
        }
    }
    for (u32 i = 0; desc.includePaths != nullptr && i < desc.includePathCount; ++i) {
        if (desc.includePaths[i] != nullptr && desc.includePaths[i][0] != '\0') {
            args.push_back(std::string("-I") + desc.includePaths[i]);
        }
    }
    args.insert(args.end(), {source, "-o", tempPath});

    if (!runValidator(args)) {
        std::remove(tempPath.c_str());
        return makeFailure(desc, "glslangValidator failed for " + source);
    }

    // Atomic replace: a watcher polling the .spv never sees a half-written module.
    std::error_code error;
    std::filesystem::rename(tempPath, spirvPath, error);
    if (error) {
        std::remove(tempPath.c_str());
        return makeFailure(desc, "could not replace " + spirvPath + ": " + error.message());
    }

    CompiledShader result = compileOffline(desc);
    if (result.valid) {
        result.message = "compiled GLSL with glslangValidator";
    }
    return result;
}

bool ShaderCompiler::enableRuntimeCompile(const char* validatorPath) {
    const char* validator = validatorPath != nullptr ? validatorPath : defaultValidatorPath();
    if (validator == nullptr || validator[0] == '\0') {
        m_validatorPath.clear();
        return false;
    }
    m_validatorPath = validator;
    return true;
}

CompiledShader ShaderCompiler::compileEntry(const ShaderDesc& desc) const {
    if (!m_validatorPath.empty()) {
        return compileWithValidator(desc, m_validatorPath.c_str());
    }
    return compileOffline(desc);
}

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

void ShaderCompiler::bindOwnedPointers(WatchedEntry& entry) {
    entry.desc.sourcePath = entry.path.c_str();
    entry.definePtrs.clear();
    entry.definePtrs.reserve(entry.defineStorage.size());
    for (const std::string& define : entry.defineStorage) {
        entry.definePtrs.push_back(define.c_str());
    }
    if (entry.definePtrs.empty()) {
        entry.desc.defines = nullptr;
        entry.desc.defineCount = 0;
    } else {
        entry.desc.defines = entry.definePtrs.data();
        entry.desc.defineCount = static_cast<u32>(entry.definePtrs.size());
    }
    entry.includePathPtrs.clear();
    entry.includePathPtrs.reserve(entry.includePathStorage.size());
    for (const std::string& includePath : entry.includePathStorage) {
        entry.includePathPtrs.push_back(includePath.c_str());
    }
    if (entry.includePathPtrs.empty()) {
        entry.desc.includePaths = nullptr;
        entry.desc.includePathCount = 0;
    } else {
        entry.desc.includePaths = entry.includePathPtrs.data();
        entry.desc.includePathCount = static_cast<u32>(entry.includePathPtrs.size());
    }
}

bool ShaderCompiler::watch(const ShaderDesc& desc) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return false;
    }

    WatchedEntry entry;
    entry.path = desc.sourcePath;
    entry.desc = desc;
    entry.defineStorage.clear();
    if (desc.defines != nullptr) {
        for (u32 i = 0; i < desc.defineCount; ++i) {
            if (desc.defines[i] != nullptr) {
                entry.defineStorage.emplace_back(desc.defines[i]);
            }
        }
    }
    entry.includePathStorage.clear();
    if (desc.includePaths != nullptr) {
        for (u32 i = 0; i < desc.includePathCount; ++i) {
            if (desc.includePaths[i] != nullptr) {
                entry.includePathStorage.emplace_back(desc.includePaths[i]);
            }
        }
    }
    bindOwnedPointers(entry);

    if (!m_watch.watch(entry.path.c_str())) {
        return false;
    }

    entry.last = compileEntry(entry.desc);
    m_entries.push_back(std::move(entry));
    // Vector growth / string SSO moves invalidate c_str, definePtrs, and includePathPtrs; rebuild all.
    for (WatchedEntry& stored : m_entries) {
        bindOwnedPointers(stored);
    }
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
            bindOwnedPointers(entry);
            entry.last = compileEntry(entry.desc);
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
                bindOwnedPointers(entry);
                entry.last = compileEntry(entry.desc);
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
