#include <fuse/world2d/sprite_image_map.hpp>

#include <filesystem>
#include <functional>
#include <vector>

#if defined(FUSE_WORLD2D_HAS_STB_IMAGE)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif

namespace fuse::world2d {

namespace {

std::vector<std::string> candidateImageMapPaths(const SpriteImageMapSearchRoots& roots, const std::string& imageMap) {
    std::vector<std::string> paths;
    if (imageMap.empty()) {
        return paths;
    }

    const std::filesystem::path fileName(imageMap);
    if (!roots.projectRoot.empty()) {
        paths.push_back((std::filesystem::path(roots.projectRoot) / "assets" / fileName).lexically_normal().string());
        paths.push_back((std::filesystem::path(roots.projectRoot) / "worlds" / fileName).lexically_normal().string());
    }
    if (!roots.repositoryRoot.empty()) {
        paths.push_back((std::filesystem::path(roots.repositoryRoot) / "Samples" / "unification" / "common" /
                         "sprite_maps" / fileName)
                            .lexically_normal()
                            .string());
    }
    return paths;
}

void proceduralFrameRgb(const std::string& imageMap, u32 frameIndex, u32 frameCount, u8& r, u8& g, u8& b) {
    const u32 count = frameCount > 0u ? frameCount : 1u;
    const u32 frame = frameIndex % count;
    const std::size_t hash = std::hash<std::string>{}(imageMap);
    r = static_cast<u8>(120u + ((hash >> 8) + frame * 29u) % 120u);
    g = static_cast<u8>(100u + ((hash >> 16) + frame * 17u) % 100u);
    b = static_cast<u8>(80u + ((hash >> 24) + frame * 41u) % 120u);
}

#if defined(FUSE_WORLD2D_HAS_STB_IMAGE)
bool samplePngHorizontalStrip(const std::string& path, u32 frameIndex, u32 frameCount, u8& r, u8& g, u8& b) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return false;
    }

    const u32 columns = frameCount > 0u ? frameCount : 1u;
    const u32 frame = frameIndex % columns;
    const int cellWidth = width / static_cast<int>(columns);
    if (cellWidth <= 0) {
        stbi_image_free(pixels);
        return false;
    }

    const int px = frame * cellWidth + cellWidth / 2;
    const int py = height / 2;
    const int offset = (py * width + px) * 4;
    r = pixels[offset + 0];
    g = pixels[offset + 1];
    b = pixels[offset + 2];
    stbi_image_free(pixels);
    return true;
}
#endif

} // namespace

bool sampleSpriteImageMapFrame(const SpriteImageMapSearchRoots& roots, const std::string& imageMap, u32 frameIndex,
                               u32 frameCount, u8& r, u8& g, u8& b) {
    if (imageMap.empty()) {
        return false;
    }

#if defined(FUSE_WORLD2D_HAS_STB_IMAGE)
    for (const std::string& path : candidateImageMapPaths(roots, imageMap)) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (samplePngHorizontalStrip(path, frameIndex, frameCount, r, g, b)) {
            return true;
        }
    }
#endif

    proceduralFrameRgb(imageMap, frameIndex, frameCount, r, g, b);
    return true;
}

} // namespace fuse::world2d
