// fuse_app native backends: the Vulkan swapchain presenter shared by the X11, Win32 and Android window code.
#pragma once

#include "fuse/app/native.hpp"

#include <string>

namespace fuse::app::detail {

struct SurfaceSource {
    enum Kind { Xlib, Win32, Android, Wayland } kind = Xlib;
    void* display = nullptr;        ///< X11 Display*
    unsigned long xwindow = 0;      ///< X11 Window
    void* hinstance = nullptr;      ///< Win32 HINSTANCE
    void* hwnd = nullptr;           ///< Win32 HWND
    void* nativeWindow = nullptr;   ///< Android ANativeWindow*
    void* wlDisplay = nullptr;      ///< Wayland wl_display*
    void* wlSurface = nullptr;      ///< Wayland wl_surface*
};

struct VkCtx;
struct VkWin;
/// Instance with the surface extension for `kind`; device chosen on the first window. Null when Vulkan is missing.
VkCtx* vkCtxCreate(SurfaceSource::Kind kind, std::string& err);
/// Wraps an existing instance + device (NativeOptions::adoptDevice); not destroyed by vkCtxDestroy.
VkCtx* vkCtxAdopt(SurfaceSource::Kind kind, const VulkanDeviceHandles& h, std::string& err);
void vkCtxDestroy(VkCtx* c);
bool vkWinSetExternal(VkWin* w, unsigned slot, const ExternalTexture& t);
const char* vkCtxDeviceName(const VkCtx* c);
/// Chooses and creates the device before any window has a surface (Android async start: a window that is still a
/// CPU producer cannot get a VkSurfaceKHR; every Android queue family can present).
bool vkCtxCreateDevice(VkCtx* c, std::string& err);
VkWin* vkWinCreate(VkCtx* c, const SurfaceSource& s, const NativeOptions& o, std::string& err);
/// Creates the swapchain for a `width` x `height` client area ahead of the first present (async init worker).
bool vkWinPrepare(VkWin* w, unsigned width, unsigned height, std::string& err);
bool vkWinPresent(VkWin* w, AppWindow& win, fuse::ui::Rect damage);
bool vkWinGpuDraws(const VkWin* w);
void vkWinDestroy(VkWin* w);
/// Reads back the last presented frame (the canvas blitted into the swapchain).
bool vkWinReadback(VkWin* w, fuse::ui::Image& out);
/// Present timing of the window's last frame (waitGpu: waits for its fence, stamps gpuDoneAt).
PresentTiming vkWinTiming(VkWin* w, bool waitGpu);

} // namespace fuse::app::detail
