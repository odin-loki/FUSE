#include <fuse/renderer/shader/shader_compiler.hpp>

#include <fuse/renderer/shader/shader_io.hpp>

namespace fuse::renderer {

namespace {

CompiledShader makeFailure(const ShaderDesc& desc, const std::string& message) {
    CompiledShader result;
    result.stage = desc.stage;
    if (desc.sourcePath != nullptr) {
        result.sourcePath = desc.sourcePath;
    }
    result.message = message;
    result.valid = false;
    return result;
}

CompiledShader makeSuccess(const ShaderDesc& desc, std::vector<u32>&& words, const char* message) {
    CompiledShader result;
    result.spirv = std::move(words);
    result.stage = desc.stage;
    result.sourcePath = desc.sourcePath;
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

} // namespace fuse::renderer
