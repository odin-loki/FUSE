#pragma once

#include <string>

namespace fuse::renderer {

/// VK_KHR_android_surface: a VkSurfaceKHR for an ANativeWindow (NativeActivity / SurfaceView window). The instance
/// must have VK_KHR_surface + VK_KHR_android_surface (VulkanInstance requests them on Android through
/// fuse::platform::requiredVulkanInstanceExtensions). Android connects the window to Vulkan here: a window that is
/// still an ANativeWindow_lock (CPU) producer is refused. Pass the result as SurfaceDesc{External, surface}.
/// Returns false (with `err`) off Android or when the call fails.
bool createAndroidSurface(void* vkInstance, void* nativeWindow, void** outSurface, std::string* err = nullptr);

/// vkDestroySurfaceKHR for a surface from createAndroidSurface (no-op for null).
void destroyAndroidSurface(void* vkInstance, void* surface);

} // namespace fuse::renderer
