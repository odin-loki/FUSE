// FUSE Remaster W0.2: image decode for canonical texture hashing (stb_image, public domain / MIT, vendor/stb).
// STB_IMAGE_STATIC keeps the stb symbols local to this object, so other FUSE targets that compile their own
// stb_image implementation can link next to fuse_remaster_store.
#include <fuse/remaster/store.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <climits>
#include <cstring>

namespace fuse::remaster::store {

std::optional<ImageRGBA8> decodeImage(std::span<const std::uint8_t> encoded, std::string* error) {
    if (encoded.empty() || encoded.size() > static_cast<std::size_t>(INT_MAX)) {
        if (error) {
            *error = "image: empty or too large";
        }
        return std::nullopt;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) {
        if (error) {
            *error = std::string("image: ") + (stbi_failure_reason() ? stbi_failure_reason() : "decode failed");
        }
        if (px) {
            stbi_image_free(px);
        }
        return std::nullopt;
    }
    ImageRGBA8 img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.pixels.assign(px, px + std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(px);
    return img;
}

} // namespace fuse::remaster::store
