#include <fuse/cook/cook_stub_writer.hpp>

#include <fuse/cook/audio_cook.hpp>

#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/ispc_texcomp_hook.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/cook/texture_cook.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <cstring>

namespace fuse::cook {

namespace {

CookStubWriteResult writeTextStub(const std::string& output_path, const std::string& payload) {
    CookStubWriteResult result;
    result.byteCount = static_cast<u32>(payload.size());

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "unable to write stub output";
        return result;
    }

    out << payload;
    result.ok = out.good();
    result.note = result.ok ? "stub output written" : "stub output write failed";
    return result;
}

} // namespace

const char* cookFailureName(CookFailure failure) {
    switch (failure) {
    case CookFailure::None:
        return "none";
    case CookFailure::InvalidArgument:
        return "invalid_argument";
    case CookFailure::ImporterUnavailable:
        return "importer_unavailable";
    case CookFailure::MalformedSource:
        return "malformed_source";
    case CookFailure::InvalidGeometry:
        return "invalid_geometry";
    case CookFailure::CorruptImage:
        return "corrupt_image";
    case CookFailure::InvalidImageDimensions:
        return "invalid_image_dimensions";
    case CookFailure::WriteFailed:
        return "write_failed";
    }
    return "unknown";
}

CookStubWriteResult tryCookMeshAssimp(const std::string& input_path, const std::string& output_path,
                                      u32 lod_count, bool compressed) {
    // GREP-COOK-1: lod_count (levels including LOD 0) -> the W0.2 discrete LOD chain,
    // compressed -> FMSH v2 quantised positions (unorm16) and normals (oct snorm16). The lenient path
    // only asks for a LOD chain when this build can make one (else the lossless LOD 0 mesh is cooked).
    MeshCookOptions options;
    options.optimize.lods = lod_options_for_count(lod_count, options.optimize.lod) && mesh_optimizer_available();
    options.encoding.quantize_positions = compressed;
    options.encoding.quantize_normals = compressed;
    return cook_mesh_file(input_path, output_path, options);
}

CookStubWriteResult tryCookTextureBc7(const std::string& input_path, const std::string& output_path,
                                      const char* compression, bool mipmaps) {
    // W0.3: BC1 / BC4 / BC5 / BC6H / BC7 through the in-house encoders (bcn_encoder.cpp).
    TextureCookOptions options;
    options.mipmaps = mipmaps;
    if (compression != nullptr && !parse_bc_format(compression, options.format)) {
        CookStubWriteResult result;
        result.note = std::string("texture compression '") + compression + "' not supported";
        result.failure = CookFailure::InvalidArgument;
        return result;
    }
    options.normal_map = options.format == BcFormat::BC5;
    return cook_texture_file(input_path, output_path, options);
}

CookStubWriteResult tryCookAudioOgg(const std::string& input_path, const std::string& output_path,
                                    u32 sample_rate, const char* format) {
    // MP-B7.9-AUDIO-IMPORT: thin wrapper over the audio cook chain (audio_cook.cpp) with the
    // AudioImportDesc defaults: decode WAV/FLAC/Ogg, resample, trim, R128 normalise, quality VBR.
    AudioCookOptions options;
    options.target_sample_rate = sample_rate != 0 ? sample_rate : kAudioDefaultSampleRate;
    if (format != nullptr && (std::strcmp(format, "pcm_f32") == 0 || std::strcmp(format, "PCM_F32") == 0)) {
        options.format = AudioCookFormat::PcmF32;
    }
    return cook_audio_file(input_path, output_path, options);
}

CookStubWriteResult write_mesh_stub(const std::string& input_path, const std::string& output_path,
                                    u32 lod_count, bool compressed) {
    CookStubWriteResult hook{};
    if (!input_path.empty()) {
        hook = tryCookMeshAssimp(input_path, output_path, lod_count, compressed);
        if (hook.ok) {
            return hook;
        }
    } else {
        hook.note = "missing_input_path";
    }

    std::ostringstream payload;
    payload << "FUSEMESH_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "lods=" << lod_count << "\n";
    payload << "compress=" << (compressed ? "on" : "off") << "\n";
    payload << "vertices=0\n";
    payload << "indices=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult write_texture_bc7_encoded(const std::string& output_path,
                                              const std::string& source_path, bool mipmaps) {
    if (!source_path.empty()) {
        const CookStubWriteResult decoded = cook_texture_bc7_file(source_path, output_path, mipmaps);
        if (decoded.ok) {
            return decoded;
        }
    }

    // Lenient fallback for undecodable sources: a deterministic placeholder image, labelled as such
    // in the header. Strict cooks (the AssetCooker default, see ImportValidation) never reach this path.
    Bc7RgbaImage working = source_path.empty() ? Bc7RgbaImage{} : synthesize_rgba_from_source(source_path);
    if (working.rgba.empty()) {
        working.width = 4u;
        working.height = 4u;
        working.rgba.clear();
        for (u32 i = 0; i < 16u; ++i) {
            working.rgba.insert(working.rgba.end(), {200, 64, 32, 255});
        }
    }
    return write_texture_bc7_rgba(working.rgba.data(), working.width, working.height, output_path, mipmaps,
                                  "bc7_synthesized_placeholder");
}

CookStubWriteResult write_texture_stub(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps) {
    const CookStubWriteResult ispc = tryCookTextureIspc(input_path, output_path, compression, mipmaps);
    if (ispc.ok) {
        return ispc;
    }

    if (compression != nullptr && std::string(compression) == "BC7") {
        const CookStubWriteResult bc7 = tryCookTextureBc7(input_path, output_path, compression, mipmaps);
        if (bc7.ok) {
            return bc7;
        }
        const CookStubWriteResult fallback = write_texture_bc7_encoded(output_path, input_path, mipmaps);
        if (fallback.ok) {
            return fallback;
        }
    }

    CookStubWriteResult hook{};
    if (!input_path.empty()) {
        hook = tryCookTextureBc7(input_path, output_path, compression, mipmaps);
        if (hook.ok) {
            return hook;
        }
    } else {
        hook.note = "missing_input_path";
    }

    std::ostringstream payload;
    payload << "FUSETEX_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "compression=" << compression << "\n";
    payload << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    payload << "width=0\n";
    payload << "height=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult write_audio_stub(const std::string& input_path, const std::string& output_path,
                                     u32 sample_rate, const char* format) {
    const CookStubWriteResult hook = tryCookAudioOgg(input_path, output_path, sample_rate, format);
    if (hook.ok) {
        return hook;
    }

    std::ostringstream payload;
    payload << "FUSEAUDIO_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "rate=" << sample_rate << "\n";
    payload << "format=" << format << "\n";
    payload << "samples=0\n";
    return writeTextStub(output_path, payload.str());
}

CookStubWriteResult tryCookShaderGlslang(const std::string& input_path, const std::string& output_path,
                                         const char* stage, u32 target_version) {
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "missing_input_or_output_path";
        return result;
    }

    const std::filesystem::path input = std::filesystem::path(input_path);
    const std::string ext = input.extension().string();
    if (ext != ".cs" && ext != ".glsl" && ext != ".frag" && ext != ".vert" && ext != ".comp") {
        result.note = "glslang_source_required";
        return result;
    }

    const char* stageFlag = "frag";
    if (stage != nullptr) {
        if (std::strcmp(stage, "vertex") == 0) {
            stageFlag = "vert";
        } else if (std::strcmp(stage, "compute") == 0 || ext == ".cs" || ext == ".comp") {
            stageFlag = "comp";
        }
    } else if (ext == ".vert") {
        stageFlag = "vert";
    } else if (ext == ".cs" || ext == ".comp") {
        stageFlag = "comp";
    }

    const std::filesystem::path tempSpv =
        std::filesystem::temp_directory_path() / "fuse_glslang_spv.bin";
    const std::string tempSpvPath = tempSpv.string();

    std::ostringstream command;
    command << "glslangValidator -V -S " << stageFlag << " \"" << input_path << "\" -o \""
            << tempSpvPath << "\"";
    const int compileResult = std::system(command.str().c_str());
    if (compileResult != 0) {
        result.note = "glslang_unavailable_or_failed";
        std::error_code ec;
        std::filesystem::remove(tempSpv, ec);
        return result;
    }

    std::ifstream in(tempSpvPath, std::ios::binary);
    if (!in) {
        result.note = "glslang_output_unreadable";
        std::error_code ec;
        std::filesystem::remove(tempSpv, ec);
        return result;
    }

    std::vector<char> spirv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(tempSpv, ec);
    if (spirv.empty() || (spirv.size() % 4u) != 0u) {
        result.note = "glslang_invalid_spirv_size";
        return result;
    }

    std::ostringstream header;
    header << "FUSESHADER_GLSLANG\n";
    header << "stage=" << (stage != nullptr ? stage : "fragment") << "\n";
    header << "version=" << target_version << "\n";
    header << "words=" << (spirv.size() / 4u) << "\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(spirv.data(), spirv.size());

    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "glslang_output_unwritable";
        return result;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    result.ok = out.good();
    result.byteCount = static_cast<u32>(payload.size());
    result.note = result.ok ? "glslang offline compile written" : "glslang write failed";
    return result;
}

CookStubWriteResult tryCookShaderSpirv(const std::string& input_path, const std::string& output_path,
                                       const char* stage, u32 target_version) {
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "missing_input_or_output_path";
        return result;
    }

    const std::filesystem::path input = std::filesystem::path(input_path);
    const std::string ext = input.extension().string();
    if (ext != ".spv") {
        result.note = "spirv_input_required";
        return result;
    }

    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        result.note = "spirv_input_unreadable";
        return result;
    }

    std::vector<char> spirv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (spirv.empty() || (spirv.size() % 4u) != 0u) {
        result.note = "spirv_invalid_size";
        return result;
    }

    std::ostringstream header;
    header << "FUSESHADER_SPIV\n";
    header << "stage=" << (stage != nullptr ? stage : "fragment") << "\n";
    header << "version=" << target_version << "\n";
    header << "words=" << (spirv.size() / 4u) << "\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(spirv.data(), spirv.size());

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "spirv_output_unwritable";
        return result;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    result.ok = out.good();
    result.byteCount = static_cast<u32>(payload.size());
    result.note = result.ok ? "spirv passthrough written" : "spirv write failed";
    return result;
}

CookStubWriteResult write_shader_stub(const std::string& input_path, const std::string& output_path,
                                      const char* stage, u32 target_version) {
    const CookStubWriteResult glslangHook =
        tryCookShaderGlslang(input_path, output_path, stage, target_version);
    if (glslangHook.ok) {
        return glslangHook;
    }

    const CookStubWriteResult hook =
        tryCookShaderSpirv(input_path, output_path, stage, target_version);
    if (hook.ok) {
        return hook;
    }

    std::ostringstream payload;
    payload << "FUSESHADER_STUB\n";
    payload << "hook=" << hook.note << "\n";
    payload << "stage=" << (stage != nullptr ? stage : "fragment") << "\n";
    payload << "version=" << target_version << "\n";
    payload << "spirv_words=0\n";
    return writeTextStub(output_path, payload.str());
}

} // namespace fuse::cook
