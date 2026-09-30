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
    /// GREP-COOK-1: discrete LOD chain (W0.2 build_mesh_lods) of `lod_count` levels including LOD 0
    /// (fuse::cook::lod_options_for_count). Off by default so plain cooks stay FMSH v1; the strict cook
    /// fails (ImporterUnavailable) when meshoptimizer is not linked.
    bool generate_lods = false;
    u32 lod_count = 4;
    f32 lod_error_target = 0.01f;
    /// GREP-COOK-1: FMSH v2 quantised streams (positions unorm16, normals oct snorm16), like
    /// `quantize_vertices`. Off by default (lossless).
    bool compress = false;
    /// FMSH v2 streams (asset plan W0.1): tangents (when `generate_tangents`), uv1, colour0, skin
    /// joints/weights and material slot names, when the source has them. Off: FMSH v1 as before.
    bool fmsh_v2_streams = false;
    /// FMSH v2 quantisation: positions unorm16 in bounds, normals oct snorm16.
    bool quantize_vertices = false;
    /// RE-P1-7: FMSH v2 sections for the GPU-driven renderer. `meshlets`: WP-1.2 meshlet table
    /// (SUBM/MSHL/MVRT/MTRI); `cluster_dag`: WP-5.2 cluster DAG (implies meshlets); `cluster_pages`:
    /// also write the WP-5.3 `.fusepages` next to the output (implies cluster_dag). All fold into the
    /// cook cache key, and a cache hit is only reused when its `.fusepages` still binds to the FMSH.
    bool meshlets = false;
    bool cluster_dag = false;
    bool cluster_pages = false;
    u32 page_bytes = 64u * 1024u; ///< `.fusepages` page payload capacity (multiple of 16, >= 1024)
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

/// Shader import descriptor — offline GLSL/HLSL → SPIR-V (B7.9 stub).
struct ShaderImportDesc {
    std::string input_path;
    std::string output_path;
    enum class Stage : u8 { Vertex, Fragment, Compute } stage = Stage::Fragment;
    u32 target_version = 450;
    bool debug_info = false;
};

} // namespace fuse::project
