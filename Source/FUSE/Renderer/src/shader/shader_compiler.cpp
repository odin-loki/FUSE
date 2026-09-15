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

} // namespace

CompiledShader ShaderCompiler::compileOffline(const ShaderDesc& desc) {
    if (desc.sourcePath == nullptr || desc.sourcePath[0] == '\0') {
        return makeFailure(desc, "shader source path is required");
    }

    std::string error;
    const std::string spirvPath = spirvPathForSource(desc.sourcePath);
    std::vector<u32> words = loadSpirvFile(spirvPath.c_str(), &error);
    if (words.empty()) {
        return makeFailure(desc, error.empty() ? "offline SPIR-V not found" : error);
    }

    CompiledShader result;
    result.spirv = std::move(words);
    result.stage = desc.stage;
    result.sourcePath = desc.sourcePath;
    result.message = "loaded offline SPIR-V";
    result.valid = true;
    return result;
}

CompiledShader ShaderCompiler::compile(const ShaderDesc& desc) {
#if defined(FUSE_SHADER_GLSLANG)
    // Runtime glslang integration is deferred; offline fixtures keep CI green without the dependency.
    (void)desc;
    return makeFailure(desc, "runtime glslang compile not implemented — use offline .spv fixtures");
#else
    return compileOffline(desc);
#endif
}

} // namespace fuse::renderer
