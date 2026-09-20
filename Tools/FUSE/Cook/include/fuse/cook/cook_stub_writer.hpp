#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::cook {

struct CookStubWriteResult {
    bool ok = false;
    u32 byteCount = 0;
    std::string note;
};

/// Optional real encoder hooks — return `ok=false` when third-party libs are absent (U7 honest stubs).
CookStubWriteResult tryCookMeshAssimp(const std::string& input_path, const std::string& output_path,
                                      u32 lod_count, bool compressed);
CookStubWriteResult tryCookTextureBc7(const std::string& input_path, const std::string& output_path,
                                      const char* compression, bool mipmaps);
CookStubWriteResult tryCookTextureIspc(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps);
CookStubWriteResult tryCookAudioOgg(const std::string& input_path, const std::string& output_path,
                                    u32 sample_rate, const char* format);
CookStubWriteResult tryCookShaderSpirv(const std::string& input_path, const std::string& output_path,
                                       const char* stage, u32 target_version);
CookStubWriteResult tryCookShaderGlslang(const std::string& input_path, const std::string& output_path,
                                         const char* stage, u32 target_version);

CookStubWriteResult write_mesh_stub(const std::string& input_path, const std::string& output_path,
                                    u32 lod_count, bool compressed);
CookStubWriteResult write_texture_stub(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps);
CookStubWriteResult write_texture_bc7_encoded(const std::string& output_path,
                                              const std::string& source_path, bool mipmaps);
CookStubWriteResult write_audio_stub(const std::string& input_path, const std::string& output_path,
                                     u32 sample_rate, const char* format);
CookStubWriteResult write_shader_stub(const std::string& input_path, const std::string& output_path,
                                      const char* stage, u32 target_version);

} // namespace fuse::cook
