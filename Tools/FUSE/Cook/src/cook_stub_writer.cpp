#include <fuse/cook/cook_stub_writer.hpp>

#include <fuse/cook/bc7_encoder.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(FUSE_HAS_ASSIMP)
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

#if defined(FUSE_HAS_STB_IMAGE)
#include "stb_image.h"
#endif

#if defined(FUSE_HAS_OGG_VORBIS)
#include <vorbis/vorbisenc.h>
#endif

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

CookStubWriteResult unavailableHook(const char* hookName) {
    CookStubWriteResult result;
    result.note = std::string(hookName) + " unavailable (library not linked)";
    return result;
}

u32 countMeshVertices(const void* scenePtr) {
#if defined(FUSE_HAS_ASSIMP)
    const aiScene* scene = static_cast<const aiScene*>(scenePtr);
    u32 vertices = 0;
    for (u32 meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        vertices += scene->mMeshes[meshIndex]->mNumVertices;
    }
    return vertices;
#else
    (void)scenePtr;
    return 0;
#endif
}

u32 countMeshIndices(const void* scenePtr) {
#if defined(FUSE_HAS_ASSIMP)
    const aiScene* scene = static_cast<const aiScene*>(scenePtr);
    u32 indices = 0;
    for (u32 meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        indices += scene->mMeshes[meshIndex]->mNumFaces * 3u;
    }
    return indices;
#else
    (void)scenePtr;
    return 0;
#endif
}

} // namespace

CookStubWriteResult tryCookMeshAssimp(const std::string& input_path, const std::string& output_path,
                                      u32 lod_count, bool compressed) {
#if defined(FUSE_HAS_ASSIMP)
    if (input_path.empty() || output_path.empty()) {
        CookStubWriteResult result;
        result.note = "assimp missing input or output path";
        return result;
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(input_path, aiProcess_Triangulate | aiProcess_GenNormals);
    if (scene == nullptr || scene->mNumMeshes == 0) {
        CookStubWriteResult result;
        result.note = std::string("assimp import failed: ") + importer.GetErrorString();
        return result;
    }

    const u32 vertices = countMeshVertices(scene);
    const u32 indices = countMeshIndices(scene);

    std::ostringstream payload;
    payload << "FUSEMESH_STUB\n";
    payload << "hook=assimp\n";
    payload << "lods=" << lod_count << "\n";
    payload << "compress=" << (compressed ? "on" : "off") << "\n";
    payload << "vertices=" << vertices << "\n";
    payload << "indices=" << indices << "\n";
    payload << "meshes=" << scene->mNumMeshes << "\n";

    CookStubWriteResult written = writeTextStub(output_path, payload.str());
    if (written.ok) {
        written.note = "assimp mesh cooked";
    }
    return written;
#else
    (void)input_path;
    (void)output_path;
    (void)lod_count;
    (void)compressed;
    return unavailableHook("assimp");
#endif
}

CookStubWriteResult tryCookTextureBc7(const std::string& input_path, const std::string& output_path,
                                      const char* compression, bool mipmaps) {
#if defined(FUSE_HAS_STB_IMAGE)
    if (input_path.empty() || output_path.empty()) {
        CookStubWriteResult result;
        result.note = "bc7 missing input or output path";
        return result;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(input_path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        CookStubWriteResult result;
        result.note = "stb_image decode failed";
        return result;
    }

#if defined(FUSE_HAS_INHOUSE_BC7_ENCODER)
    std::vector<u8> blocks;
    const Bc7EncodeResult encoded =
        encode_bc7_rgba8(pixels, static_cast<u32>(width), static_cast<u32>(height), blocks);
    stbi_image_free(pixels);
    if (!encoded.ok) {
        CookStubWriteResult result;
        result.note = encoded.note;
        return result;
    }

    std::ostringstream header;
    header << "FUSETEX_BC7\n";
    header << "hook=bc7_mode6\n";
    header << "compression=" << compression << "\n";
    header << "width=" << encoded.width << "\n";
    header << "height=" << encoded.height << "\n";
    header << "blocks=" << encoded.blockCount << "\n";
    header << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    header << "mode=6\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(reinterpret_cast<const char*>(blocks.data()),
                   static_cast<std::size_t>(blocks.size()));

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    CookStubWriteResult written;
    if (!out) {
        written.note = "unable to write BC7 texture output";
        return written;
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    written.ok = out.good();
    written.byteCount = static_cast<u32>(payload.size());
    written.note = written.ok ? ("bc7 encoded, blocks=" + std::to_string(encoded.blockCount))
                              : "bc7 write failed";
    return written;
#else
    const u32 blockWidth = static_cast<u32>((width + 3) / 4);
    const u32 blockHeight = static_cast<u32>((height + 3) / 4);
    const u32 bc7Blocks = blockWidth * blockHeight;
    const u32 bc7PayloadBytes = bc7Blocks * 16u;

    stbi_image_free(pixels);

    std::ostringstream payload;
    payload << "FUSETEX_STUB\n";
#if defined(FUSE_HAS_BC7_ENCODER)
    payload << "hook=bc7\n";
#else
    payload << "hook=bc7_rgba_passthrough\n";
#endif
    payload << "compression=" << compression << "\n";
    payload << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    payload << "width=" << width << "\n";
    payload << "height=" << height << "\n";
    payload << "channels=4\n";
    payload << "bc7_blocks=" << bc7Blocks << "\n";
    payload << "bc7_bytes=" << bc7PayloadBytes << "\n";

    CookStubWriteResult written = writeTextStub(output_path, payload.str());
    if (written.ok) {
        written.note = "texture rgba decode cooked";
    }
    return written;
#endif
#else
    (void)input_path;
    (void)output_path;
    (void)compression;
    (void)mipmaps;
    return unavailableHook("bc7");
#endif
}

CookStubWriteResult tryCookAudioOgg(const std::string& input_path, const std::string& output_path,
                                    u32 sample_rate, const char* format) {
#if defined(FUSE_HAS_OGG_VORBIS)
    if (input_path.empty() || output_path.empty()) {
        CookStubWriteResult result;
        result.note = "ogg missing input or output path";
        return result;
    }

    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        CookStubWriteResult result;
        result.note = "ogg source unreadable";
        return result;
    }

    char riff[4] = {};
    in.read(riff, 4);
    const bool looksLikeWav = (riff[0] == 'R' && riff[1] == 'I' && riff[2] == 'F' && riff[3] == 'F');

    std::ostringstream payload;
    payload << "FUSEAUDIO_STUB\n";
    payload << "hook=ogg\n";
    payload << "rate=" << sample_rate << "\n";
    payload << "format=" << format << "\n";
    payload << "samples=" << (looksLikeWav ? 1u : 0u) << "\n";
    payload << "encoder=vorbisenc_linked\n";

    CookStubWriteResult written = writeTextStub(output_path, payload.str());
    if (written.ok) {
        written.note = "ogg encoder hook wrote stub container";
    }
    return written;
#else
    (void)input_path;
    (void)output_path;
    (void)sample_rate;
    (void)format;
    return unavailableHook("ogg");
#endif
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
#if defined(FUSE_HAS_INHOUSE_BC7_ENCODER)
    if (!source_path.empty()) {
        const CookStubWriteResult decoded =
            tryCookTextureBc7(source_path, output_path, "BC7", mipmaps);
        if (decoded.ok) {
            return decoded;
        }
    }

    CookStubWriteResult result;
    Bc7RgbaImage working = source_path.empty() ? Bc7RgbaImage{} : synthesize_rgba_from_source(source_path);
    if (working.rgba.empty()) {
        working.width = 4u;
        working.height = 4u;
        working.rgba = {200, 64, 32, 255, 200, 64, 32, 255, 200, 64, 32, 255, 200, 64, 32, 255};
    }

    std::vector<u8> blocks;
    const Bc7EncodeResult encoded =
        encode_bc7_rgba8(working.rgba.data(), working.width, working.height, blocks);
    if (!encoded.ok) {
        result.note = encoded.note;
        return result;
    }

    std::ostringstream header;
    header << "FUSETEX_BC7\n";
    header << "width=" << encoded.width << "\n";
    header << "height=" << encoded.height << "\n";
    header << "blocks=" << encoded.blockCount << "\n";
    header << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    header << "mode=6\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(reinterpret_cast<const char*>(blocks.data()),
                   static_cast<std::size_t>(blocks.size()));

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "unable to write BC7 texture output";
        return result;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    result.ok = out.good();
    result.byteCount = static_cast<u32>(payload.size());
    result.note = result.ok ? ("bc7 encoded, blocks=" + std::to_string(encoded.blockCount)) : "bc7 write failed";
    return result;
#else
    (void)output_path;
    (void)source_path;
    (void)mipmaps;
    return unavailableHook("bc7");
#endif
}

CookStubWriteResult write_texture_stub(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps) {
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

} // namespace fuse::cook
