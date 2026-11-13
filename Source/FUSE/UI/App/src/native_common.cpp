// fuse_app native backends: the parts of NativePlatform that are the same on every platform.
#include "fuse/app/native.hpp"

namespace fuse::app {

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
// (macOS / iOS / Android): the API is there, each call reports "cannot". Text goes through the platform's text clipboard.
std::size_t NativePlatform::toolWindowCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::toolWindow(const AppWindow&, std::size_t) const { return 0; }
fuse::ui::Rect NativePlatform::toolWindowScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::captureToolWindow(const AppWindow&, std::size_t, fuse::ui::Image&) { return false; }
bool NativePlatform::postPointerMove(unsigned long long, fuse::ui::Vec2) { return false; }
bool NativePlatform::postPointerButton(unsigned long long, fuse::ui::Vec2, bool) { return false; }
bool NativePlatform::postClose(unsigned long long) { return false; }
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

std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
std::expected<fuse::ui::Color, PlatformError> colorDialog(const ColorDialogDesc&) { return std::unexpected(PlatformError::Unavailable); }
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
