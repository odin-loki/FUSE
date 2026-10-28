#pragma once

#include <string_view>

namespace fuse::renderer {

inline bool vulkan_loader_name(std::string_view fileName) {
    const auto slash = fileName.find_last_of("/\\");
    const std::string_view base = slash == std::string_view::npos ? fileName : fileName.substr(slash + 1);
    return base == "vulkan-1.dll" || base == "libvulkan.so" || base == "libvulkan.so.1" || base == "libvulkan.dylib";
}

} // namespace fuse::renderer
