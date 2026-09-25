// FUSE Relight RL-6.1: scripted input for the developer overlay (tests, automation; relight.overlay.script).
//
// A text file, one command per line: `<frame> <command> [arguments]` ('#' starts a comment). At the Present of tap
// frame <frame> the tap turns each command into the window messages a user would cause and sends them through the
// window hook (WindowHook::send: SendMessage into the subclassed WndProc), one command at a time, running the menu on
// the events after each, so `clickw` sees the layout the previous command produced.
//
//   toggle              Alt+X (WM_SYSKEYDOWN 'X' + WM_SYSCHAR 'x' + WM_SYSKEYUP 'X', ALT context bit)
//   click X Y           WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP at back-buffer pixel (X, Y)
//   clickw ID           the same at the centre of widget ID (ui.hpp widgetRect; fails when it is not laid out)
//   move X Y            WM_MOUSEMOVE
//   type TEXT           WM_CHAR for each character of the rest of the line
//   key VK              WM_KEYDOWN + WM_KEYUP; VK a number or back, enter, esc, up, down, pgup, pgdn, home
//   wheel N             WM_MOUSEWHEEL of N notches (negative: towards the user)
//   game N              N messages a game consumes (alternating WM_KEYDOWN / WM_KEYUP 'W', WM_MOUSEMOVE, WM_CHAR 'w')
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::overlay {

struct ScriptCommand {
    enum class Kind : std::uint8_t { Toggle, Click, ClickWidget, Move, Type, Key, Wheel, Game };
    std::uint64_t frame = 0;
    Kind kind = Kind::Toggle;
    std::int32_t x = 0, y = 0, value = 0;
    std::string text; ///< ClickWidget: the id; Type: the characters
    std::uint32_t line = 0;
};

struct WindowMessage {
    std::uint32_t msg = 0;
    std::uint64_t wParam = 0;
    std::int64_t lParam = 0;
};

class Script {
public:
    /// Parses `text`; false with error() on the first bad line.
    bool parse(std::string_view text);
    bool load(const std::string& path);
    const std::vector<ScriptCommand>& commands() const { return m_commands; }
    const std::string& error() const { return m_error; }
    /// Commands of `frame`, in file order: [begin, end) indices into commands().
    void range(std::uint64_t frame, std::size_t& begin, std::size_t& end) const;

    /// The window messages of a command. (x, y) are client pixels for the mouse commands (the caller maps
    /// back-buffer pixels, or a widget's centre, to the client area).
    static void messages(const ScriptCommand& c, std::int32_t clientX, std::int32_t clientY,
                         std::vector<WindowMessage>& out);

private:
    std::vector<ScriptCommand> m_commands; ///< sorted by frame (stable)
    std::string m_error;
};

} // namespace fuse::relight::overlay
