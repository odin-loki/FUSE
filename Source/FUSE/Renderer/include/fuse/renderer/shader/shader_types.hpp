#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer {

enum class ShaderStage : u8 {
    Vertex,
    Fragment,
    Compute,
    Mesh,
    Task,
    RayGen,
    RayMiss,
    RayClosestHit,
    RayAnyHit,
};

struct ShaderDesc {
    const char* sourcePath = nullptr;
    const char* entryPoint = "main";
    ShaderStage stage = ShaderStage::Vertex;
    const char** defines = nullptr;
    u32 defineCount = 0;
};

struct CompiledShader {
    std::vector<u32> spirv;
    ShaderStage stage = ShaderStage::Vertex;
    std::string sourcePath;
    std::string message;
    bool valid = false;
};

} // namespace fuse::renderer
