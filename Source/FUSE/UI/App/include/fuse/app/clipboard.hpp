// fuse_app system clipboard (plan §4.13): text, images, custom MIME types and lazy provision.
//
//   fuse::app::setClipboardText("hello");
//   fuse::app::setClipboard({{.mime = "application/x-myapp-nodes", .bytes = blob},
//                            {.mime = std::string(fuse::app::kClipboardImage), .provide = [](fuse::app::ClipboardItem& it) {
//                                it.image = renderThumbnail(); return true; }}}); // made only when someone pastes it
//
//   Win32: CF_UNICODETEXT, CF_DIBV5 (Windows synthesises CF_DIB / CF_BITMAP), RegisterClipboardFormat(mime) for custom
//          types; lazy items use delayed rendering (WM_RENDERFORMAT on a hidden message window of the setting thread).
//   X11:   the CLIPBOARD selection on a private connection: TARGETS, UTF8_STRING / STRING / text/plain;charset=utf-8,
//          image/png, custom MIME atoms, INCR for large transfers. Requests are served from the NativePlatform event
//          pump, so other clients can paste only while it runs.
//   Elsewhere (Android, no display): a process-local store with the same API.
//
// UI thread only. NativePlatform::show() routes the window's UiContext clipboard (LineEdit Ctrl+C / X / V) here.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::app {

/// Format ids. Text is UTF-8; an image is a fuse::ui::Image of straight (not premultiplied) RGBA, 0xAABBGGRR.
inline constexpr std::string_view kClipboardText = "text/plain;charset=utf-8";
inline constexpr std::string_view kClipboardImage = "image/png";

struct ClipboardItem {
    std::string mime;                 ///< kClipboardText, kClipboardImage or any other MIME type
    std::string text;                 ///< kClipboardText
    fuse::ui::Image image;            ///< kClipboardImage
    std::vector<std::uint8_t> bytes;  ///< other MIME types
    /// Lazy provision: called once, when a paste first asks for this format; fills text / image / bytes.
    std::function<bool(ClipboardItem&)> provide;
};

/// Replaces the clipboard with `items` (one per format). False when the OS clipboard could not be taken.
bool setClipboard(std::vector<ClipboardItem> items);
bool setClipboardText(std::string_view utf8);
bool setClipboardImage(const fuse::ui::Image& image);

/// Reads the clipboard (whoever owns it). False when the format is not there.
bool clipboardText(std::string& out);
bool clipboardImage(fuse::ui::Image& out);
/// Raw bytes of any format (text: UTF-8; images are read with clipboardImage).
bool clipboardData(std::string_view mime, std::vector<std::uint8_t>& out);
/// MIME types offered now (kClipboardText / kClipboardImage for the OS text and image formats).
std::vector<std::string> clipboardFormats();
/// True while this process owns the clipboard (it has not been replaced by another application since).
bool clipboardOwned();

} // namespace fuse::app
