#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

/// Name a Vulkan object via `vkSetDebugUtilsObjectNameEXT` when `VK_EXT_debug_utils` is loaded.
/// `vkObjectType` is `VkObjectType` numeric (buffer=9, image=10, deviceMemory=8).
/// Returns false if device/name/handle is null, the proc is missing, or the backend is stub.
bool setDebugObjectName(void* vkDevice, u32 vkObjectType, u64 handle, const char* name);

} // namespace fuse::renderer
