#pragma once
// UI1 text services: system clipboard (UTF-8 text) and the text-input session (IME on / off, caret rectangle for the
// candidate and composition windows). Committed text and pre-edit arrive through EventPump as TextInput /
// TextComposition events.
//
// Backends: Win32 = CF_UNICODETEXT clipboard + IMM32. Elsewhere (X11, headless, GLFW) the clipboard is process-local
// (copy / paste inside the app works; the X11 CLIPBOARD selection protocol is not wired yet) and the session calls are
// recorded but have no OS effect.

#include <fuse/types.hpp>

#include <string>
#include <string_view>

namespace fuse::platform {

class Window;

/// Replaces the clipboard contents with `utf8`. False when the OS clipboard could not be opened.
bool setClipboardText(std::string_view utf8);
/// Reads the clipboard as UTF-8 into `out` (cleared first). False when it holds no text.
bool clipboardText(std::string& out);

/// Starts / stops a text-input session for `window` (Win32: associates / disassociates the IME context, so
/// game keys do not open the IME outside text fields).
void beginTextInput(Window& window);
void endTextInput(Window& window);
bool textInputActive(const Window& window);
/// Client-pixel caret rectangle of the focused text field: places the IME composition and candidate windows.
void setTextInputArea(Window& window, i32 x, i32 y, i32 width, i32 height);

struct TextInputArea {
    i32 x = 0, y = 0, width = 0, height = 0;
};
/// Last area given to setTextInputArea (any window; diagnostics and tests).
TextInputArea lastTextInputArea();

} // namespace fuse::platform
