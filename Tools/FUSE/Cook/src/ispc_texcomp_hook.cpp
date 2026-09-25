#include <fuse/cook/ispc_texcomp_hook.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
    if (encoded.ok && encoded.note != nullptr && std::string(encoded.note).find("stub") != std::string::npos) {
        // third_party/ispc_texcomp/ispc_texcomp.h is an API stub that fills blocks with placeholder
        // bytes. Never emit those: report the hook as unavailable so callers use the in-house BCn
        // encoders (bcn_encoder.cpp, asset plan W0.3).
        stbi_image_free(pixels);
        CookStubWriteResult result;
        result.note = "ispc_texcomp unavailable (stub header only)";
        return result;
    }
    if (!encoded.ok) {
        stbi_image_free(pixels);
        CookStubWriteResult result;
        result.note = encoded.note != nullptr ? encoded.note : "ispc compress failed";
        return result;
    }

    u32 mipLevelCount = 1u;
    std::vector<uint8_t> mipBlocks;
    if (mipmaps && width > 1 && height > 1) {
        int mipWidth = width;
        int mipHeight = height;
        while (mipWidth > 1 || mipHeight > 1) {
            mipWidth = std::max(1, mipWidth / 2);
            mipHeight = std::max(1, mipHeight / 2);
            std::vector<uint8_t> rgba(static_cast<std::size_t>(mipWidth * mipHeight * 4u));
            for (int y = 0; y < mipHeight; ++y) {
                for (int x = 0; x < mipWidth; ++x) {
                    const int srcX = std::min(x * 2, width - 1);
                    const int srcY = std::min(y * 2, height - 1);
                    const int srcIndex = (srcY * width + srcX) * 4;
                    const int dstIndex = (y * mipWidth + x) * 4;
                    rgba[static_cast<std::size_t>(dstIndex + 0)] = pixels[srcIndex + 0];
                    rgba[static_cast<std::size_t>(dstIndex + 1)] = pixels[srcIndex + 1];
                    rgba[static_cast<std::size_t>(dstIndex + 2)] = pixels[srcIndex + 2];
                    rgba[static_cast<std::size_t>(dstIndex + 3)] = pixels[srcIndex + 3];
                }
            }
            const uint32_t mipBlockCount =
                ispc::blockCountForSize(static_cast<uint32_t>(mipWidth), static_cast<uint32_t>(mipHeight));
            const std::size_t mipOffset = mipBlocks.size();
            mipBlocks.resize(mipOffset + mipBlockCount * 16u);
            const ispc::CompressionResult mipEncoded = ispc::CompressBlocks(
                rgba.data(), static_cast<uint32_t>(mipWidth), static_cast<uint32_t>(mipHeight), format,
                mipBlocks.data() + mipOffset);
            if (!mipEncoded.ok) {
                break;
            }
            ++mipLevelCount;
        }
    }
    stbi_image_free(pixels);

    std::ostringstream header;
    header << "FUSETEX_BC7\n";
    header << "hook=" << (encoded.note != nullptr && std::string(encoded.note).find("stub") != std::string::npos
                              ? "ispc_texcomp_stub"
                              : "ispc_texcomp")
           << "\n";
    header << "compression=" << (compression != nullptr ? compression : "BC7") << "\n";
    header << "width=" << width << "\n";
    header << "height=" << height << "\n";
    header << "blocks=" << encoded.blockCount << "\n";
    header << "mipmaps=" << (mipmaps ? "on" : "off") << "\n";
    header << "mip_levels=" << mipLevelCount << "\n";
    header << "DATA\n";

    std::string payload = header.str();
    payload.append(reinterpret_cast<const char*>(blocks.data()), blocks.size());
    if (!mipBlocks.empty()) {
        payload.append(reinterpret_cast<const char*>(mipBlocks.data()), mipBlocks.size());
    }

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
