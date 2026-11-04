// The one stb_image (PNG only) / stb_image_write instance of the UI libraries (fuse/text/png_codec.hpp). Static
// functions: no stbi_* symbols escape, so applications and tests may still compile their own stb.
#include "fuse/text/png_codec.hpp"

#include <cstdio>
#include <cstring>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4245 4267 4456 4457 4505 4701 4702 4996 6001 6011 6262 6385 6386)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
#endif
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

namespace fuse::text::png {

bool info(std::span<const std::uint8_t> bytes, int& w, int& h) {
    w = h = 0;
    int comp = 0;
    if (bytes.empty() || bytes.size() > 0x7fffffffu) return false;
    return stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp) != 0;
}

bool decodeRgba(std::span<const std::uint8_t> bytes, int maxDim, int& w, int& h, std::vector<std::uint8_t>& rgba) {
    if (!info(bytes, w, h) || w <= 0 || h <= 0 || w > maxDim || h > maxDim) return false;
    int dw = 0, dh = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &dw, &dh, &comp, 4);
    if (!px) return false;
    const bool ok = dw == w && dh == h;
    if (ok) rgba.assign(px, px + std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(px);
    return ok;
}

bool encodeRgba(const std::uint8_t* rgba, int w, int h, int stride, std::vector<std::uint8_t>& out) {
    if (!rgba || w <= 0 || h <= 0) return false;
    return stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
        v->insert(v->end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
    }, &out, w, h, 4, rgba, stride) != 0;
}

bool writeRgba(const std::string& path, const std::uint8_t* rgba, int w, int h, int stride) {
    std::vector<std::uint8_t> bytes;
    if (!encodeRgba(rgba, w, h, stride, bytes)) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace fuse::text::png
