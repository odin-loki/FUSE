#pragma once

#include <fuse/renderer/shader/shader_types.hpp>
#include <fuse/renderer/shader/shader_watch.hpp>

#include <string>
#include <vector>

namespace fuse::jobs {
class JobScheduler;
}

namespace fuse::renderer {

/// Offline-first shader compiler front end (B2.4, WP-0.5).
/// CI loads checked-in `.spv` fixtures; runtime compiles dispatch on the source extension:
/// `.slang` -> slangc (pinned by cmake/FuseSlang.cmake), GLSL -> glslangValidator.
class ShaderCompiler {
public:
    /// Loads precompiled SPIR-V for `desc.sourcePath` (`.spv` sibling or explicit `.spv` path).
    static CompiledShader compileOffline(const ShaderDesc& desc);

    /// When glslang is enabled at build time, compiles GLSL/HLSL source; otherwise falls back to offline.
    static CompiledShader compile(const ShaderDesc& desc);

    /// Compile GLSL `desc.sourcePath` with an external glslangValidator into its `.spv` sibling
    /// (written to a temp file, then renamed so watchers never observe a partial module) and load
    /// it. `validatorPath` null uses the build-configured glslangValidator. Returns an invalid
    /// shader when no validator is available or compilation fails.
    static CompiledShader compileWithValidator(const ShaderDesc& desc, const char* validatorPath = nullptr);

    /// Build-configured glslangValidator path, or nullptr when none was found at configure time.
    static const char* defaultValidatorPath();

    /// Compile Slang `desc.sourcePath` with slangc (SPIR-V 1.5, same flags as the build-time
    /// fuse_add_slang_shaders) into the permutation cache directory, then load it. The output is
    /// `<cacheDir>/<stem>.<permutationKey hex>.spv`, written to a unique temp file and renamed, so
    /// concurrent permutations of one source never collide. `dependencies` comes from slangc's
    /// depfile (#include and import). `slangcPath` null uses defaultSlangcPath().
    static CompiledShader compileWithSlang(const ShaderDesc& desc, const char* slangcPath = nullptr);

    /// slangc to use: env FUSE_SLANGC when set, else the build-configured one, else nullptr.
    static const char* defaultSlangcPath();

    /// Directory for runtime-compiled permutations: env FUSE_SHADER_CACHE_DIR, else
    /// `<temp>/fuse_shader_cache`. Created on demand.
    static std::string permutationCacheDirectory();

    /// Language dispatch with the default tools: Slang -> compileWithSlang, GLSL ->
    /// compileWithValidator, precompiled -> compileOffline. Falls back to compileOffline when the
    /// tool for the language is unavailable.
    static CompiledShader compileSource(const ShaderDesc& desc);

    /// Background compile: runs compileSource for every desc on `scheduler` (parallel_for; serial
    /// when null or single-threaded). Results are in desc order. Descs (and their strings) must stay
    /// alive until the call returns; it blocks until all compiles finished.
    static std::vector<CompiledShader> compileBatch(const ShaderDesc* descs, u32 count,
                                                    jobs::JobScheduler* scheduler);

    /// Opt this compiler into runtime GLSL compilation: `watch` / `pollHotReload` then recompile
    /// changed sources with glslangValidator instead of only reloading prebuilt `.spv` files.
    /// Also enables runtime Slang when a slangc is available (defaultSlangcPath()).
    /// Returns false (and stays offline) when no validator is available.
    bool enableRuntimeCompile(const char* validatorPath = nullptr);
    bool runtimeCompileEnabled() const { return !m_validatorPath.empty(); }

    /// Opt into runtime Slang compilation for `.slang` sources (independent of GLSL). Returns false
    /// when no slangc is available.
    bool enableSlangRuntimeCompile(const char* slangcPath = nullptr);
    bool slangRuntimeCompileEnabled() const { return !m_slangcPath.empty(); }

    /// Watch `desc.sourcePath`, define strings, and include paths (copied into owned storage).
    /// Returns false if path is null/empty.
    bool watch(const ShaderDesc& desc);

    /// Polls ShaderFileWatch and recompiles only watched entries whose source or one of whose
    /// dependencies (#include / Slang import, from the last runtime compile) changed in this poll.
    /// Each entry recompiles at most once per poll. Returns number of successful recompiles.
    u32 pollHotReload();

    u32 watchedCount() const;
    const CompiledShader* lastCompiled(const char* path) const; // nullptr if unknown

private:
    struct WatchedEntry {
        std::string path;
        std::vector<std::string> defineStorage;
        std::vector<const char*> definePtrs;
        std::vector<std::string> includePathStorage;
        std::vector<const char*> includePathPtrs;
        ShaderDesc desc{};
        CompiledShader last{};
    };

    static void bindOwnedPointers(WatchedEntry& entry);
    CompiledShader compileEntry(const ShaderDesc& desc) const;
    /// Adds the dependencies of `compiled` to the file watch (idempotent).
    void watchDependencies(const CompiledShader& compiled);

    std::string m_validatorPath;
    std::string m_slangcPath;
    std::vector<std::string> m_watchedPaths;

    ShaderFileWatch m_watch;
    std::vector<WatchedEntry> m_entries;
};

} // namespace fuse::renderer
