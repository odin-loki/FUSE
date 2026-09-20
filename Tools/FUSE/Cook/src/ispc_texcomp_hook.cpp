#include <fuse/cook/ispc_texcomp_hook.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(FUSE_HAS_STB_IMAGE)
#include "stb_image.h"
#endif

#if defined(FUSE_HAS_ISPC_TEXCOMP)
#include <ispc_texcomp.h>
#endif

namespace fuse::cook {

CookStubWriteResult tryCookTextureIspc(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps) {
#if defined(FUSE_HAS_ISPC_TEXCOMP) && defined(FUSE_HAS_STB_IMAGE)
    if (input_path.empty() || output_path.empty()) {
        CookStubWriteResult result;
        result.note = "ispc missing input or output path";
        return result;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(input_path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        CookStubWriteResult result;
        result.note = "ispc stb_image decode failed";
        return result;
    }

    const ispc::TextureFormat format =
        (compression != nullptr && std::string(compression) == "BC5") ? ispc::TextureFormat::BC5_RG
                                                                      : ispc::TextureFormat::BC7_RGBA;
    const uint32_t blockCount = ispc::blockCountForSize(static_cast<uint32_t>(width),
                                                        static_cast<uint32_t>(height));
    std::vector<uint8_t> blocks(blockCount * 16u);
    const ispc::CompressionResult encoded =
        ispc::CompressBlocks(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                             format, blocks.data());
    stbi_image_free(pixels);
    if (!encoded.ok) {
        CookStubWriteResult result;
        result.note = encoded.note != nullptr ? encoded.note : "ispc compress failed";
        return result;
    }

    std::ostringstream header;
    header << "FUSETEX_BC7\n";
    header << "hook=ispc_texcomp\n";
    header << "compression=" << (compression != nullptr ? compression : "BC7") << "\n";
    header << "width=" << width << "\n";
    header << "height=" << height << "\n";
    header << "blocks=" << encoded.blockCount << "\n";
    header << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(reinterpret_cast<const char*>(blocks.data()), blocks.size());

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    CookStubWriteResult written;
    if (!out) {
        written.note = "unable to write ispc texture output";
        return written;
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    written.ok = out.good();
    written.byteCount = static_cast<u32>(payload.size());
    written.note = written.ok ? "ispc_texcomp encoded" : "ispc write failed";
    return written;
#else
    (void)input_path;
    (void)output_path;
    (void)compression;
    (void)mipmaps;
    CookStubWriteResult result;
    result.note = "ispc_texcomp unavailable (header not linked)";
    return result;
#endif
}

} // namespace fuse::cook
