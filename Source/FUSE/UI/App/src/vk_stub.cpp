// fuse_app native backends built without a Vulkan loader: no swapchain presenter (CPU raster presenters only).
#include "native_internal.hpp"

namespace fuse::app::detail {
struct VkCtx {};
struct VkWin {};
VkCtx* vkCtxCreate(SurfaceSource::Kind, std::string& err) { err = "built without Vulkan"; return nullptr; }
VkCtx* vkCtxAdopt(SurfaceSource::Kind, const VulkanDeviceHandles&, std::string& err) { err = "built without Vulkan"; return nullptr; }
void vkCtxDestroy(VkCtx*) {}
bool vkWinSetExternal(VkWin*, unsigned, const ExternalTexture&) { return false; }
const char* vkCtxDeviceName(const VkCtx*) { return ""; }
VkWin* vkWinCreate(VkCtx*, const SurfaceSource&, const NativeOptions&, std::string&) { return nullptr; }
bool vkWinPresent(VkWin*, AppWindow&, fuse::ui::Rect) { return false; }
bool vkWinGpuDraws(const VkWin*) { return false; }
void vkWinDestroy(VkWin*) {}
bool vkWinReadback(VkWin*, fuse::ui::Image&) { return false; }
} // namespace fuse::app::detail
