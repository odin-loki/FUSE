// UI1 clipboard + text-input session: see include/fuse/platform/text_services.hpp.
#include <fuse/platform/text_services.hpp>
#include <fuse/platform/window.hpp>

#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#endif
#if defined(FUSE_PLATFORM_WINDOW_X11)
#include "x11_window.hpp"
#endif

namespace fuse::platform {

namespace {
TextInputArea g_area{};
const Window* g_textWindow = nullptr;
#if !defined(_WIN32)
std::string& localClipboard() {
    static std::string text;
    return text;
}
#endif
} // namespace

#if defined(_WIN32)
bool setClipboardText(std::string_view utf8) {
    const int wide = utf8.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wide) + 1u) * sizeof(wchar_t))) {
        auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
        if (wide > 0) {
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), dst, wide);
        }
        dst[wide] = L'\0';
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

bool clipboardText(std::string& out) {
    out.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    if (HANDLE mem = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* src = static_cast<const wchar_t*>(GlobalLock(mem))) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                out.resize(static_cast<usize>(bytes) - 1u);
                WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), bytes, nullptr, nullptr);
            }
            ok = true;
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
    return ok;
}
#else
bool setClipboardText(std::string_view utf8) {
    localClipboard().assign(utf8);
#if defined(FUSE_PLATFORM_WINDOW_X11)
    if (x11::ensureDisplay()) {
        return x11::setClipboard(utf8.data(), utf8.size()); // CLIPBOARD selection owner
    }
#endif
    return true;
}

bool clipboardText(std::string& out) {
#if defined(FUSE_PLATFORM_WINDOW_X11)
    if (x11::ensureDisplay()) {
        thread_local std::vector<char> bytes;
        const bool ok = x11::clipboard(bytes);
        out.assign(bytes.data(), bytes.size());
        return ok;
    }
#endif
    out = localClipboard();
    return !out.empty();
}
#endif

void beginTextInput(Window& window) {
    g_textWindow = &window;
#if defined(_WIN32)
    if (HWND hwnd = static_cast<HWND>(window.nativeHandle().value)) {
        ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
    }
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    x11::setTextInputFocus(window.nativeHandle().value, true);
#endif
}

void endTextInput(Window& window) {
    if (g_textWindow == &window) {
        g_textWindow = nullptr;
    }
#if defined(_WIN32)
    if (HWND hwnd = static_cast<HWND>(window.nativeHandle().value)) {
        ImmAssociateContextEx(hwnd, nullptr, 0); // detach: no IME while no text field is focused
    }
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    x11::setTextInputFocus(window.nativeHandle().value, false);
#endif
}

bool textInputActive(const Window& window) { return g_textWindow == &window; }

void setTextInputArea(Window& window, i32 x, i32 y, i32 width, i32 height) {
    g_area = {x, y, width, height};
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(window.nativeHandle().value);
    if (hwnd == nullptr) {
        return;
    }
    if (HIMC imc = ImmGetContext(hwnd)) {
        COMPOSITIONFORM comp{};
        comp.dwStyle = CFS_POINT;
        comp.ptCurrentPos = {x, y};
        ImmSetCompositionWindow(imc, &comp);
        CANDIDATEFORM cand{};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos = {x, y + height};
        cand.rcArea = {x, y, x + width, y + height};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(hwnd, imc);
    }
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    x11::setTextInputSpot(window.nativeHandle().value, x, y + height);
#else
    (void)window;
#endif
}

TextInputArea lastTextInputArea() { return g_area; }

} // namespace fuse::platform
