#include <fuse/renderer/shader/shader_compiler.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/shader/shader_io.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
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
    result.tier = desc.tier;
    result.permutationKey = shaderPermutationKey(desc);
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

/// Unique per process + call: concurrent compiles (compileBatch, two build trees) never share a
/// temp file.
std::string uniqueTempSuffix() {
    static std::atomic<u32> counter{0};
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    return ".tmp." + std::to_string(pid) + "." + std::to_string(counter.fetch_add(1u));
}

std::string readTextFile(const std::string& path, usize maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > maxBytes) {
        text.resize(maxBytes);
        text += "...";
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

/// Run a tool with `args` (args[0] = executable); stdout + stderr go to `logPath` (or are
/// discarded when empty). POSIX spawns the process directly (no shell: ~10 ms less per hot reload
/// and no quoting pitfalls).
bool runTool(const std::vector<std::string>& args, const std::string& logPath) {
#if defined(_WIN32)
    std::string command;
    for (const std::string& arg : args) {
        command += (command.empty() ? "\"" : " \"") + arg + "\"";
    }
    const std::string sink = logPath.empty() ? std::string("NUL") : "\"" + logPath + "\"";
    command = "\"" + command + " > " + sink + " 2>&1\"";
    return std::system(command.c_str()) == 0;
#else
    std::vector<char*> argv;
    argv.reserve(args.size() + 1u);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const char* sink = logPath.empty() ? "/dev/null" : logPath.c_str();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, sink, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
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

/// Parses a Makefile-style depfile ("out: dep1 dep2 \\\n dep3", '\ ' escapes a space) and returns
/// the prerequisites as canonical paths, without `exclude` (the source itself) and duplicates.
std::vector<std::string> parseDepfile(const std::string& depfilePath, const std::string& exclude) {
    std::vector<std::string> deps;
    std::ifstream in(depfilePath, std::ios::binary);
    if (!in) {
        return deps;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> tokens;
    std::string current;
    bool sawColon = false;
    auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1u < text.size()) {
            const char next = text[i + 1u];
            if (next == '\n' || next == '\r') { // line continuation
                flush();
                ++i;
                continue;
            }
            if (next == ' ' || next == '#' || next == ':' || next == '\\') {
                current.push_back(next);
                ++i;
                continue;
            }
            current.push_back(c); // Windows path separator
            continue;
        }
        if (c == ':' && !sawColon && (i + 1u >= text.size() || text[i + 1u] == ' ' || text[i + 1u] == '\n' ||
                                      text[i + 1u] == '\r' || text[i + 1u] == '\t')) {
            // Target separator (a drive letter "C:\" is followed by a path character instead).
            current.clear();
            tokens.clear();
            sawColon = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
            continue;
        }
        if (c == '$' && i + 1u < text.size() && text[i + 1u] == '$') {
            current.push_back('$');
            ++i;
            continue;
        }
        current.push_back(c);
    }
    flush();

    std::error_code error;
    const std::filesystem::path excludeCanonical = std::filesystem::weakly_canonical(exclude, error);
    for (const std::string& token : tokens) {
        std::error_code canonicalError;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(token, canonicalError);
        const std::string path = canonicalError ? token : canonical.string();
        if (!excludeCanonical.empty() && canonical == excludeCanonical) {
            continue;
        }
        if (std::find(deps.begin(), deps.end(), path) == deps.end()) {
            deps.push_back(path);
        }
    }
    return deps;
}

const char* slangStageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Fragment:
        return "fragment";
    case ShaderStage::Compute:
        return "compute";
    case ShaderStage::Mesh:
        return "mesh";
    case ShaderStage::Task:
        return "amplification";
    case ShaderStage::RayGen:
        return "raygeneration";
    case ShaderStage::RayMiss:
        return "miss";
    case ShaderStage::RayClosestHit:
        return "closesthit";
    case ShaderStage::RayAnyHit:
        return "anyhit";
    }
    return "compute";
}

void appendDefinesAndIncludes(std::vector<std::string>& args, const ShaderDesc& desc, bool separateIncludeArg) {
    for (u32 i = 0; desc.defines != nullptr && i < desc.defineCount; ++i) {
        if (desc.defines[i] != nullptr && desc.defines[i][0] != '\0') {
            args.push_back(std::string("-D") + desc.defines[i]);
        }
    }
    if (desc.tier >= 0) {
        args.push_back("-DFUSE_RENDER_TIER=" + std::to_string(desc.tier));
    }
    for (u32 i = 0; desc.includePaths != nullptr && i < desc.includePathCount; ++i) {
        if (desc.includePaths[i] != nullptr && desc.includePaths[i][0] != '\0') {
            if (separateIncludeArg) {
                args.insert(args.end(), {"-I", desc.includePaths[i]});
            } else {
                args.push_back(std::string("-I") + desc.includePaths[i]);
            }
        }
    }
}

std::string hex16(u64 value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<usize>(i)] = kDigits[value & 0xFu];
        value >>= 4u;
    }
    return text;
}

} // namespace

ShaderLanguage shaderLanguageForPath(const char* sourcePath) {
    if (sourcePath == nullptr) {
        return ShaderLanguage::Glsl;
    }
    const std::string path(sourcePath);
    if (isPrecompiledPath(path)) {
        return ShaderLanguage::Precompiled;
    }
    const std::string suffix = ".slang";
    if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return ShaderLanguage::Slang;
    }
    return ShaderLanguage::Glsl;
}

u64 shaderPermutationKey(const ShaderDesc& desc) {
    constexpr u64 kFnvOffset = 14695981039346656037ull;
    u64 hash = kFnvOffset;
    auto mix = [&hash](const std::string& text) {
        hash = hashMixDefineBytes(hash, text.data(), static_cast<u32>(text.size()));
        const char separator = '\x1f';
        hash = hashMixDefineBytes(hash, &separator, 1u);
    };
    mix(desc.sourcePath != nullptr ? desc.sourcePath : "");
    mix((desc.entryPoint != nullptr && desc.entryPoint[0] != '\0') ? desc.entryPoint : "main");
    mix(std::to_string(static_cast<u32>(desc.stage)));
    std::vector<std::string> defines;
    for (u32 i = 0; desc.defines != nullptr && i < desc.defineCount; ++i) {
        if (desc.defines[i] != nullptr && desc.defines[i][0] != '\0') {
            defines.emplace_back(desc.defines[i]);
        }
    }
    std::sort(defines.begin(), defines.end());
    mix("defines");
    for (const std::string& define : defines) {
        mix(define);
    }
    mix("includes");
    for (u32 i = 0; desc.includePaths != nullptr && i < desc.includePathCount; ++i) {
        if (desc.includePaths[i] != nullptr) {
            mix(desc.includePaths[i]);
        }
    }
    mix("tier" + std::to_string(desc.tier));
    return hash;
}

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
    const std::string tempPath = spirvPath + uniqueTempSuffix();
    const std::string depPath = tempPath + ".d";
    const std::string logPath = tempPath + ".log";
    std::vector<std::string> args = {validator, "-V", "-S", glslangStageName(desc.stage)};
    if (desc.entryPoint != nullptr && desc.entryPoint[0] != '\0' && std::string(desc.entryPoint) != "main") {
        args.insert(args.end(), {"-e", desc.entryPoint, "--source-entrypoint", "main"});
    }
    appendDefinesAndIncludes(args, desc, false);
    args.insert(args.end(), {source, "-o", tempPath, "--depfile", depPath});

    if (!runTool(args, logPath)) {
        const std::string log = readTextFile(logPath, 2048u);
        std::remove(tempPath.c_str());
        std::remove(depPath.c_str());
        std::remove(logPath.c_str());
        return makeFailure(desc, "glslangValidator failed for " + source + (log.empty() ? "" : ": " + log));
    }
    std::vector<std::string> dependencies = parseDepfile(depPath, source);
    std::remove(depPath.c_str());
    std::remove(logPath.c_str());

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
    result.dependencies = std::move(dependencies);
    return result;
}

const char* ShaderCompiler::defaultSlangcPath() {
    const char* env = std::getenv("FUSE_SLANGC");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
#if defined(FUSE_SLANGC_PATH)
    return FUSE_SLANGC_PATH;
#else
    return nullptr;
#endif
}

std::string ShaderCompiler::permutationCacheDirectory() {
    const char* env = std::getenv("FUSE_SHADER_CACHE_DIR");
    std::error_code error;
    std::filesystem::path dir = (env != nullptr && env[0] != '\0')
                                    ? std::filesystem::path(env)
                                    : std::filesystem::temp_directory_path(error) / "fuse_shader_cache";
    std::filesystem::create_directories(dir, error);
    return dir.string();
}

CompiledShader ShaderCompiler::compileWithSlang(const ShaderDesc& desc, const char* slangcPath) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return makeFailure(desc, "shader source path is required");
    }
    const char* slangc = slangcPath != nullptr ? slangcPath : defaultSlangcPath();
    if (slangc == nullptr || slangc[0] == '\0') {
        return makeFailure(desc, "no slangc configured");
    }
    const std::string source(desc.sourcePath);
    if (shaderLanguageForPath(desc.sourcePath) == ShaderLanguage::Precompiled) {
        return compileOffline(desc);
    }

    const u64 key = shaderPermutationKey(desc);
    const std::filesystem::path outDir(permutationCacheDirectory());
    const std::string stem = std::filesystem::path(source).stem().string();
    const std::string spirvPath = (outDir / (stem + "." + hex16(key) + ".spv")).string();
    const std::string tempPath = spirvPath + uniqueTempSuffix();
    const std::string depPath = tempPath + ".d";
    const std::string logPath = tempPath + ".log";

    // Keep in sync with FUSE_SLANG_SPIRV_FLAGS (cmake/FuseSlang.cmake).
    std::vector<std::string> args = {slangc, source, "-target", "spirv", "-profile", "spirv_1_5", "-entry",
                                     (desc.entryPoint != nullptr && desc.entryPoint[0] != '\0') ? desc.entryPoint
                                                                                               : "main",
                                     "-stage", slangStageName(desc.stage)};
    appendDefinesAndIncludes(args, desc, true);
    args.insert(args.end(), {"-o", tempPath, "-depfile", depPath});

    if (!runTool(args, logPath)) {
        const std::string log = readTextFile(logPath, 2048u);
        std::remove(tempPath.c_str());
        std::remove(depPath.c_str());
        std::remove(logPath.c_str());
        return makeFailure(desc, "slangc failed for " + source + (log.empty() ? "" : ": " + log));
    }
    std::vector<std::string> dependencies = parseDepfile(depPath, source);
    std::remove(depPath.c_str());
    std::remove(logPath.c_str());

    std::error_code error;
    std::filesystem::rename(tempPath, spirvPath, error);
    if (error) {
        std::remove(tempPath.c_str());
        return makeFailure(desc, "could not replace " + spirvPath + ": " + error.message());
    }

    std::string loadError;
    std::vector<u32> words = loadSpirvFile(spirvPath.c_str(), &loadError);
    if (words.empty()) {
        return makeFailure(desc, loadError.empty() ? "slangc produced no SPIR-V" : loadError);
    }
    CompiledShader result = makeSuccess(desc, std::move(words), "compiled Slang with slangc");
    result.spirvPath = spirvPath;
    result.dependencies = std::move(dependencies);
    return result;
}

CompiledShader ShaderCompiler::compileSource(const ShaderDesc& desc) {
    switch (shaderLanguageForPath(desc.sourcePath)) {
    case ShaderLanguage::Slang:
        if (defaultSlangcPath() != nullptr) {
            return compileWithSlang(desc);
        }
        break;
    case ShaderLanguage::Glsl:
        if (defaultValidatorPath() != nullptr) {
            return compileWithValidator(desc);
        }
        break;
    case ShaderLanguage::Precompiled:
        break;
    }
    return compileOffline(desc);
}

std::vector<CompiledShader> ShaderCompiler::compileBatch(const ShaderDesc* descs, u32 count,
                                                         jobs::JobScheduler* scheduler) {
    std::vector<CompiledShader> results(descs != nullptr ? count : 0u);
    if (descs == nullptr || count == 0u) {
        return results;
    }
    auto body = [descs, &results](u32 i) { results[i] = compileSource(descs[i]); };
    if (scheduler != nullptr && scheduler->isInitialized()) {
        scheduler->parallel_for(0u, count, 1u, body);
    } else {
        for (u32 i = 0; i < count; ++i) {
            body(i);
        }
    }
    return results;
}

bool ShaderCompiler::enableRuntimeCompile(const char* validatorPath) {
    const char* validator = validatorPath != nullptr ? validatorPath : defaultValidatorPath();
    if (validator == nullptr || validator[0] == '\0') {
        m_validatorPath.clear();
        return false;
    }
    m_validatorPath = validator;
    enableSlangRuntimeCompile();
    return true;
}

bool ShaderCompiler::enableSlangRuntimeCompile(const char* slangcPath) {
    const char* slangc = slangcPath != nullptr ? slangcPath : defaultSlangcPath();
    if (slangc == nullptr || slangc[0] == '\0') {
        m_slangcPath.clear();
        return false;
    }
    m_slangcPath = slangc;
    return true;
}

CompiledShader ShaderCompiler::compileEntry(const ShaderDesc& desc) const {
    const ShaderLanguage language = shaderLanguageForPath(desc.sourcePath);
    if (language == ShaderLanguage::Slang) {
        if (!m_slangcPath.empty()) {
            return compileWithSlang(desc, m_slangcPath.c_str());
        }
        return compileOffline(desc);
    }
    if (!m_validatorPath.empty()) {
        return compileWithValidator(desc, m_validatorPath.c_str());
    }
    return compileOffline(desc);
}

void ShaderCompiler::watchDependencies(const CompiledShader& compiled) {
    for (const std::string& dependency : compiled.dependencies) {
        if (std::find(m_watchedPaths.begin(), m_watchedPaths.end(), dependency) != m_watchedPaths.end()) {
            continue;
        }
        if (m_watch.watch(dependency.c_str())) {
            m_watchedPaths.push_back(dependency);
        }
    }
}

CompiledShader ShaderCompiler::compileOffline(const ShaderDesc& desc) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return makeFailure(desc, "shader source path is required");
    }

    std::string error;
    const std::string spirvPath = spirvPathForSource(desc.sourcePath);
    std::vector<u32> words = loadSpirvFile(spirvPath.c_str(), &error);
    if (!words.empty()) {
        CompiledShader result = makeSuccess(desc, std::move(words), "loaded offline SPIR-V");
        result.spirvPath = spirvPath;
        return result;
    }

    const std::string fuseshaderPath = fuseshaderPathForSource(desc.sourcePath);
    words = loadCookedFuseshaderSpirv(fuseshaderPath.c_str(), &error);
    if (!words.empty()) {
        CompiledShader result = makeSuccess(desc, std::move(words), "loaded cooked fuseshader SPIR-V");
        result.spirvPath = fuseshaderPath;
        return result;
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

    if (std::find(m_watchedPaths.begin(), m_watchedPaths.end(), entry.path) == m_watchedPaths.end()) {
        if (!m_watch.watch(entry.path.c_str())) {
            return false;
        }
        m_watchedPaths.push_back(entry.path);
    }

    entry.last = compileEntry(entry.desc);
    watchDependencies(entry.last);
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

    std::vector<std::string> changedPaths;
    changedPaths.reserve(changedCount);
    for (u32 i = 0; i < changedCount; ++i) {
        const char* path = m_watch.lastChangedPath(i);
        if (path != nullptr) {
            changedPaths.emplace_back(path);
        }
    }
    auto isChanged = [&changedPaths](const std::string& path) {
        return std::find(changedPaths.begin(), changedPaths.end(), path) != changedPaths.end();
    };

    std::vector<const CompiledShader*> recompiled;
    for (WatchedEntry& entry : m_entries) {
        bool affected = isChanged(entry.path);
        for (usize d = 0; !affected && d < entry.last.dependencies.size(); ++d) {
            affected = isChanged(entry.last.dependencies[d]);
        }
        if (!affected) {
            continue;
        }
        bindOwnedPointers(entry);
        CompiledShader next = compileEntry(entry.desc);
        if (next.valid) {
            ++successes;
        } else if (next.dependencies.empty()) {
            // A failed compile has no depfile: keep watching what the last good build pulled in.
            next.dependencies = entry.last.dependencies;
        }
        entry.last = std::move(next);
        recompiled.push_back(&entry.last);
    }
    for (const CompiledShader* compiled : recompiled) {
        watchDependencies(*compiled);
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
