// Static auto-initialisation of the Vulkan loader (volk) for fuse_rhi. See vk/loader.hpp,
// "Loader initialisation".
//
// This translation unit is its own archive member of fuse_rhi, and nothing in fuse_rhi references
// it. It is linked into an image only when the link line forces its anchor symbol
// (fuse_rhi_volk_auto_init) as undefined. cmake/FuseVolk.cmake adds that `-u` / `/INCLUDE:` to
// fuse_rhi's INTERFACE_LINK_OPTIONS, unless the consuming target sets the FUSE_RHI_VOLK_NO_AUTO_INIT
// property or the tree is configured with FUSE_RHI_VOLK_AUTO_INIT=OFF. When it is not linked, fuse_rhi
// runs no loader code during static initialisation, so an image that links it (d3d9.dll) does not
// load vulkan-1.dll from DllMain. The loader is then opened by the first explicit
// vkloader::initialize() / initializeWithProcAddr(), or lazily by the first VulkanInstance::create or
// VulkanDevice::adopt.
#include <fuse/renderer/vk/loader.hpp>

namespace fuse::renderer::vkloader::detail {
bool runAutoInit();
} // namespace fuse::renderer::vkloader::detail

extern "C" {
// The anchor the link line forces. Its dynamic initialiser loads the loader's global entry points
// (vkCreateInstance, vkEnumerateInstance*) so that code may call them before any fuse_rhi object
// exists (tests probe layers first).
extern int fuse_rhi_volk_auto_init;
#if defined(FUSE_RHI_VOLK_NO_AUTO_INIT)
int fuse_rhi_volk_auto_init = 0;
#else
int fuse_rhi_volk_auto_init = fuse::renderer::vkloader::detail::runAutoInit() ? 1 : 0;
#endif
}
