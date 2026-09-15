#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::project {

/// Mesh import descriptor — FBX / GLTF / OBJ → engine binary (B7.9 stub).
struct MeshImportDesc {
    std::string input_path;
    std::string output_path;
    bool generate_tangents = true;
    bool generate_normals = true;
    bool optimise_vertex_cache = true;
    bool generate_lods = true;
    u32 lod_count = 4;
    f32 lod_error_target = 0.01f;
    bool compress = true;
};

/// Texture import descriptor (B7.9 stub).
struct TextureImportDesc {
    std::string input_path;
    std::string output_path;
    enum class ColorSpace : u8 { Linear, sRGB } color_space = ColorSpace::sRGB;
    bool generate_mipmaps = true;
    enum class Compression : u8 { None, BC1, BC3, BC4, BC5, BC7 } compression = Compression::BC7;
    bool is_normal_map = false;
    bool is_hdr = false;
};

/// Audio import descriptor (B7.9 stub).
struct AudioImportDesc {
    std::string input_path;
    std::string output_path;
    u32 target_sample_rate = 48000;
    bool normalise = true;
    bool trim_silence = true;
    enum class Format : u8 { PCM_F32, OGG_VORBIS } format = Format::OGG_VORBIS;
    f32 ogg_quality = 0.6f;
};

} // namespace fuse::project
