// fuse_app desktop platform services (plan §4.13): displays, native dialogs and shell. Displays, DPI, cursors, touch
// and the system theme of real windows live on NativePlatform (native.hpp); the functions here need no window.
//
//   Win32: IFileOpenDialog / IFileSaveDialog (FOS_PICKFOLDERS for folders), MessageBoxW, ShellExecuteW (open URL),
//          SHOpenFolderAndSelectItems (reveal in Explorer).
//   X11 / other POSIX: zenity for dialogs and message boxes, xdg-open for URLs and folders. Unavailable when the
//          tool is missing.
//
// Dialogs are modal and block the calling (UI) thread until the user answers; `owner` (NativePlatform::nativeWindow)
// keeps them on top of that window.
#pragma once

#include "fuse/ui/types.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::app {

/// One monitor. Rects are in virtual-screen device pixels.
struct DisplayInfo {
    std::string name;          ///< OS device name (\\.\DISPLAY1, X screen)
    fuse::ui::Rect bounds;     ///< whole monitor
    fuse::ui::Rect work;       ///< minus task bar / docks
    float scale = 1.f;         ///< DPI scale (dpi / 96)
    float refreshHz = 0.f;     ///< 0 = unknown
    bool primary = false;
};

enum class PlatformError : unsigned char {
    Cancelled,       ///< the user dismissed the dialog
    Unavailable,     ///< no backend on this platform / tool missing
    InvalidArgument, ///< e.g. a URL scheme other than http, https, mailto, file
    Failed,          ///< the OS call failed (missing path, ...)
};
const char* platformErrorName(PlatformError e);

struct FileFilter {
    std::string_view name;     ///< "Images"
    std::string_view patterns; ///< "*.png;*.jpg"
};
struct FileDialogDesc {
    std::string_view title{};
    std::string_view folder{};   ///< initial folder (empty = OS default)
    std::string_view fileName{}; ///< save: suggested file name
    std::span<const FileFilter> filters{};
    bool multiple = false;     ///< open: allow several files
    unsigned long long owner = 0; ///< native window the dialog is modal to (0 = none)
};
/// Paths are UTF-8.
std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc& d);
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc& d);
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc& d);

/// Colour picker (Win32 ChooseColor, full RGB / HSL panel and the session's custom colours; POSIX zenity
/// --color-selection). Alpha is passed through unchanged.
struct ColorDialogDesc {
    std::string_view title{};
    fuse::ui::Color initial{1.f, 1.f, 1.f, 1.f};
    unsigned long long owner = 0; ///< native window the dialog is modal to (0 = none)
};
std::expected<fuse::ui::Color, PlatformError> colorDialog(const ColorDialogDesc& d);

enum class MessageIcon : unsigned char { Info, Warning, Error, Question };
enum class MessageButtons : unsigned char { Ok, OkCancel, YesNo, YesNoCancel };
enum class MessageResult : unsigned char { Ok, Cancel, Yes, No };
std::expected<MessageResult, PlatformError> messageBox(std::string_view title, std::string_view text,
                                                       MessageIcon icon = MessageIcon::Info,
                                                       MessageButtons buttons = MessageButtons::Ok,
                                                       unsigned long long owner = 0);

/// Opens `url` in the default handler. Only http://, https://, mailto: and file: URLs are accepted.
std::expected<void, PlatformError> openUrl(std::string_view url);
/// Shows `path` selected in the file manager (Explorer; on POSIX its folder is opened). Failed when it is missing.
std::expected<void, PlatformError> revealInFileManager(std::string_view path);
/// The scheme check openUrl applies (exposed for tests).
bool isOpenableUrl(std::string_view url);

} // namespace fuse::app
