// fuse_app native backends: the parts of NativePlatform that are the same on every platform.
#include "native_internal.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace fuse::app {

namespace detail {
fuse::ui::Key keysymKey(unsigned long s) {
    using fuse::ui::Key;
    switch (s) {
    case 0xff09: case 0xfe20: return Key::Tab;
    case 0xff0d: case 0xff8d: return Key::Enter;
    case 0xff1b: return Key::Escape;
    case 0xff08: return Key::Backspace;
    case 0xffff: return Key::Delete;
    case 0xff51: return Key::Left;
    case 0xff52: return Key::Up;
    case 0xff53: return Key::Right;
    case 0xff54: return Key::Down;
    case 0xff50: return Key::Home;
    case 0xff57: return Key::End;
    case 0xff55: return Key::PageUp;
    case 0xff56: return Key::PageDown;
    case 0xffbe: return Key::F1;
    case 0xffbf: return Key::F2;
    case 0xffc0: return Key::F3;
    case 0xffc1: return Key::F4;
    case 0xffc2: return Key::F5;
    case 0xffc3: return Key::F6;
    case 0xffc4: return Key::F7;
    case 0xffc5: return Key::F8;
    case 0xffc6: return Key::F9;
    case 0xffc7: return Key::F10;
    case 0xffc8: return Key::F11;
    case 0xffc9: return Key::F12;
    default: break;
    }
    Key k = Key::Other;
    if (s < 0x80) asciiKey(k, fuse::ui::u32(s));
    return k;
}
} // namespace detail

#if defined(__ANDROID__) || defined(__APPLE__) || defined(_WIN32)
NativeBackend NativePlatform::backend() const { return NativeBackend::Auto; } // one window system on these platforms
#endif

void NativePlatform::openFileDialog(AppWindow& w, const FileDialogOptions& o, FileDialogCallback cb) {
    std::string path;
    switch (fileDialog(&w, o, path)) {
    case FileDialogStatus::Accepted:
        if (cb.done) cb.done(cb.user, w, path);
        return;
    case FileDialogStatus::Cancelled:
        if (cb.done) cb.done(cb.user, w, {});
        return;
    case FileDialogStatus::Unavailable: break;
    }
    openToolkitFileDialog(w, o, cb); // no native dialog here: the in-toolkit one, answered later from the UI
}

#if defined(__ANDROID__) || defined(__APPLE__)
// ---- platforms without tool windows, OS drag and drop, clipboard formats or window-less dialogs ----------------------
// (iOS / Android; macOS has tool windows and the file / colour dialogs): the API is there, each call reports "cannot".
// Text goes through the platform's text clipboard.
#if !(defined(__APPLE__) && TARGET_OS_OSX) // macOS: borderless child panels + posted NSEvents (native_apple.mm)
std::size_t NativePlatform::toolWindowCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::toolWindow(const AppWindow&, std::size_t) const { return 0; }
fuse::ui::Rect NativePlatform::toolWindowScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::captureToolWindow(const AppWindow&, std::size_t, fuse::ui::Image&) { return false; }
bool NativePlatform::postPointerMove(unsigned long long, fuse::ui::Vec2) { return false; }
bool NativePlatform::postPointerButton(unsigned long long, fuse::ui::Vec2, bool) { return false; }
bool NativePlatform::postClose(unsigned long long) { return false; }
#endif
bool NativePlatform::externalDragActive() const { return false; }
int NativePlatform::cursorThemeSize() const { return 0; }
bool NativePlatform::setClipboard(std::vector<ClipboardItem> items) {
    for (ClipboardItem& it : items) {
        if (it.mime != kClipboardText && it.mime != "text/plain") continue;
        if (it.provide && !it.provide(it.user, it)) return false;
        return setClipboardText(it.text);
    }
    return false; // only text has an OS format here
}
bool NativePlatform::setClipboardImage(const fuse::ui::Image&) { return false; }
bool NativePlatform::clipboardImage(fuse::ui::Image&) { return false; }
bool NativePlatform::clipboardData(std::string_view mime, std::vector<fuse::ui::u8>& out) {
    out.clear();
    std::string text;
    if ((mime != kClipboardText && mime != "text/plain") || !clipboardText(text)) return false;
    out.assign(text.begin(), text.end());
    return true;
}
std::vector<std::string> NativePlatform::clipboardFormats() {
    std::string text;
    if (clipboardText(text)) return {std::string(kClipboardText)};
    return {};
}
bool NativePlatform::clipboardOwned() const { return false; }
bool NativePlatform::setPrimarySelectionText(std::string_view) { return false; }
bool NativePlatform::primarySelectionText(std::string&) { return false; }

#if !(defined(__APPLE__) && TARGET_OS_OSX) // macOS: NSOpenPanel / NSSavePanel / NSColorPanel (native_apple.mm)
std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
std::expected<fuse::ui::Color, PlatformError> colorDialog(const ColorDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
#endif
std::expected<MessageResult, PlatformError> messageBox(std::string_view, std::string_view, MessageIcon, MessageButtons, unsigned long long) {
    return std::unexpected(PlatformError::Unavailable);
}
std::expected<void, PlatformError> revealInFileManager(std::string_view path) {
    if (path.empty()) return std::unexpected(PlatformError::InvalidArgument);
    if (!revealFile(path)) return std::unexpected(PlatformError::Unavailable);
    return {};
}
#endif

} // namespace fuse::app
