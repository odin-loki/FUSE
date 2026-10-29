#include "fuse/editor/fui/app_icon.hpp"

#include "fuse_icons_embedded.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cstring>

namespace fuse::editor::fui {

std::vector<fuse::ui::Image> applicationIcon() {
    std::vector<fuse::ui::Image> out;
    for (const EmbeddedIcon& e : kFuseIcons) {
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load_from_memory(e.png, int(e.bytes), &w, &h, &n, 4);
        if (!px) return {};
        fuse::ui::Image img;
        img.resize(w, h);
        std::memcpy(img.pixels.data(), px, std::size_t(w) * std::size_t(h) * 4); // RGBA bytes == 0xAABBGGRR LE
        stbi_image_free(px);
        out.push_back(std::move(img));
    }
    return out;
}

} // namespace fuse::editor::fui
