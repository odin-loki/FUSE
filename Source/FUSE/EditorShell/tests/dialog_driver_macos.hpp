// macOS half of fuse_editor_ui_native_dialogs: answers the next application-modal AppKit dialog (NSSavePanel /
// NSOpenPanel / NSColorPanel, fuse::app's window-less dialogs) from a timer on the main run loop, which keeps firing
// inside the dialog's modal session. The Win32 half answers #32770 dialogs from a helper thread.
#pragma once

#include <memory>
#include <string>

namespace fuse::editor::test {

struct MacDialogAnswer {
    enum class Kind { Cancel, Save, Open, Folder, Colour } kind = Kind::Cancel;
    std::string folder; ///< Save / Folder: the directory to go to
    std::string name;   ///< Save: the file name typed
    std::string file;   ///< Open: the file selected
    float r = 0.f, g = 0.f, b = 0.f; ///< Colour
};
/// What the driver saw, read after macDialogFinish: `answered` 1 when it answered a dialog of the expected kind, 0
/// when no modal dialog came up, negative on a failure (`what` says which).
struct MacDialogState {
    int answered = 0;
    bool okSent = false; ///< Return / Escape went to a file panel (taken once its modal session is over)
    std::string what;
    void* timer = nullptr; ///< the armed NSTimer (retained)
};
/// Arms the driver for the next modal dialog; it gives up (answered 0) after ~60 s without one.
std::shared_ptr<MacDialogState> macAnswerNextDialog(MacDialogAnswer a);
/// Call once the dialog call returned: disarms the driver and settles `answered` (an OK that ended the panel's modal
/// session counts as answered).
void macDialogFinish(MacDialogState& st);

} // namespace fuse::editor::test
