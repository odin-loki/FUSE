#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::cook {

/// Why a real importer/encoder refused a source (strict cook validation). `None` when `ok`, or when
/// the failure has no more specific class than "bad argument".
enum class CookFailure : u8 {
    None = 0,
    InvalidArgument,        ///< empty input/output path, null pixel buffer, ...
    ImporterUnavailable,    ///< assimp / stb_image not linked into this build
    MalformedSource,        ///< file could not be parsed at all (truncated, corrupt, wrong format)
    InvalidGeometry,        ///< parsed, but geometry is unusable (no triangles, out-of-range indices, NaN/Inf)
    CorruptImage,           ///< image bytes could not be decoded (truncated, corrupt, not an image)
    InvalidImageDimensions, ///< zero-size or larger than `kMaxCookTextureDimension`
    WriteFailed,            ///< the cooked output could not be written
};

[[nodiscard]] const char* cookFailureName(CookFailure failure);

struct CookStubWriteResult {
    bool ok = false;
    u32 byteCount = 0;
    std::string note;
    CookFailure failure = CookFailure::None;
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
