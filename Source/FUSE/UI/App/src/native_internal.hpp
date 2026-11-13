// fuse_app native backends: the Vulkan swapchain presenter shared by the window code of every platform (X11 and
// Win32 in native.cpp, Android in native_android.cpp, macOS / iOS in native_apple.mm) and the key helpers they share.
#pragma once

#include "fuse/app/native.hpp"

#include <string>

namespace fuse::app::detail {

struct SurfaceSource {
    enum Kind { Xlib, Win32, Android, Metal, Wayland } kind = Xlib;
    void* display = nullptr;        ///< X11 Display*
    unsigned long xwindow = 0;      ///< X11 Window
    void* hinstance = nullptr;      ///< Win32 HINSTANCE
    void* hwnd = nullptr;           ///< Win32 HWND
    void* nativeWindow = nullptr;   ///< Android ANativeWindow*
    void* metalLayer = nullptr;     ///< macOS / iOS CAMetalLayer* (VK_EXT_metal_surface, MoltenVK)
    void* wlDisplay = nullptr;      ///< Wayland wl_display* (VK_KHR_wayland_surface)
    void* wlSurface = nullptr;      ///< Wayland wl_surface*
};

/// X11 / xkbcommon keysym -> UI key (the two share the values); Key::Other when the key set has no such key. The
/// Wayland backend's table; native.cpp keeps its own x11::mapKey with the same content.
fuse::ui::Key keysymKey(unsigned long sym);

/// Printable ASCII -> UI key (letters either case, digits, space, brackets); 0 when the key set has no such key.
inline fuse::ui::u8 asciiKey(fuse::ui::Key& k, fuse::ui::u32 c) {
    using fuse::ui::Key;
    static constexpr Key kLetters[26] = {Key::A, Key::B, Key::C, Key::D, Key::E, Key::F, Key::G, Key::H, Key::I,
                                         Key::J, Key::K, Key::L, Key::M, Key::N, Key::O, Key::P, Key::Q, Key::R,
                                         Key::S, Key::T, Key::U, Key::V, Key::W, Key::X, Key::Y, Key::Z};
    static constexpr Key kDigits[10] = {Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4,
                                        Key::Num5, Key::Num6, Key::Num7, Key::Num8, Key::Num9};
    if (c >= 'a' && c <= 'z') { k = kLetters[c - 'a']; return 1; }
    if (c >= 'A' && c <= 'Z') { k = kLetters[c - 'A']; return 1; }
    if (c >= '0' && c <= '9') { k = kDigits[c - '0']; return 1; }
    switch (c) {
    case ' ': k = Key::Space; return 1;
    case '[': k = Key::BracketLeft; return 1;
    case ']': k = Key::BracketRight; return 1;
    default: return 0;
    }
}
inline void appendUtf8(std::string& s, fuse::ui::u32 cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}

struct VkCtx;
struct VkWin;
/// Instance with the surface extension for `kind`; device chosen on the first window. Null when Vulkan is missing.
VkCtx* vkCtxCreate(SurfaceSource::Kind kind, std::string& err);
/// Wraps an existing instance + device (NativeOptions::adoptDevice); not destroyed by vkCtxDestroy.
VkCtx* vkCtxAdopt(SurfaceSource::Kind kind, const VulkanDeviceHandles& h, std::string& err);
void vkCtxDestroy(VkCtx* c);
bool vkWinSetExternal(VkWin* w, unsigned slot, const ExternalTexture& t);
const char* vkCtxDeviceName(const VkCtx* c);
VkWin* vkWinCreate(VkCtx* c, const SurfaceSource& s, const NativeOptions& o, std::string& err);
bool vkWinPresent(VkWin* w, AppWindow& win, fuse::ui::Rect damage);
bool vkWinGpuDraws(const VkWin* w);
void vkWinDestroy(VkWin* w);
/// Reads back the last presented frame (the canvas blitted into the swapchain).
bool vkWinReadback(VkWin* w, fuse::ui::Image& out);

} // namespace fuse::app::detail
