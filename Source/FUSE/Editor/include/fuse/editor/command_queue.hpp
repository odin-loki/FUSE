#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::editor {

enum class CommandKind {
    SetProperty,
    DeleteObject,
    ReparentObject,
    SelectEntity,
    StartPlay,
    StopPlay,
    PausePlay,
    ResumePlay,
    Undo,
    Redo,
    // UNI-U6-FILE-1 / MP-B6-QT-SCENE-FILES: project + scene files (applied by EditorHost on the
    // game thread; the outcome is `EditorHost::lastFileResult()`). Build them with the make*Command
    // helpers below.
    NewProject,  ///< propertyValue = directory, propertyName = project name, flags = kProject* bits
    OpenProject, ///< propertyValue = directory (or its project.json)
    NewScene,    ///< flags = kScene2D for a 2D world, propertyName = scene name (optional)
    OpenScene,   ///< propertyValue = .fuselevel path
    SaveScene,   ///< save to the current scene path
    SaveSceneAs, ///< propertyValue = .fuselevel path
    // MP-B6-EDITOR-SCRIPT-PIE: PIE single step while paused, and console lines for the game thread.
    StepPlay,
    ConsoleExec, ///< propertyValue = console line (engine command or Lua), see EditorHost::executeConsoleLine
};

/// UI-thread command envelope — applied on the game thread via CommandQueue::drain().
/// Full Qt editor panes land in U6; this header stays Qt-free.
struct EditorCommand {
    CommandKind kind = CommandKind::SetProperty;
    Handle<Object> target = Handle<Object>::invalid();
    Handle<Object> parent = Handle<Object>::invalid();
    std::string propertyName;
    std::string propertyValue;
    /// First value in a coalesced drag group — preserved when later edits merge (B6.2 deepen).
    std::string propertyValueBefore;
    /// Command-specific option bits (NewProject dimensions, NewScene dimension).
    u32 flags = 0;
};

/// `EditorCommand::flags` bits.
inline constexpr u32 kProjectEnable3D = 1u << 0;
inline constexpr u32 kProjectEnable2D = 1u << 1;
inline constexpr u32 kProjectEnableUI = 1u << 2;
inline constexpr u32 kScene2D = 1u << 0;

EditorCommand makeNewProjectCommand(std::string directory, std::string name,
                                    u32 dimensionFlags = kProjectEnable3D | kProjectEnable2D | kProjectEnableUI);
EditorCommand makeOpenProjectCommand(std::string directory);
EditorCommand makeNewSceneCommand(bool scene2D = false, std::string name = {});
EditorCommand makeOpenSceneCommand(std::string path);
EditorCommand makeSaveSceneCommand();
EditorCommand makeSaveSceneAsCommand(std::string path);
EditorCommand makeTransportCommand(CommandKind kind); ///< StartPlay / StopPlay / PausePlay / ResumePlay / StepPlay
EditorCommand makeConsoleExecCommand(std::string line);

/// Formats a float for `EditorCommand::propertyValue` so parsing it back yields the identical
/// value (round-trip precision; `std::to_string` keeps only six decimals).
std::string formatPropertyFloat(f32 value);

/// Thread-safe queue: post from UI thread, drain on game thread.
class CommandQueue {
public:
    void post(EditorCommand command);
    void drain();
    u32 pendingCount() const;
    u32 appliedCount() const { return m_applied; }
    u32 coalescedPostCount() const { return m_coalescedPosts; }

    /// Commands moved out of the pending queue by the most recent drain() call.
    const std::vector<EditorCommand>& lastDrainedBatch() const { return m_lastDrained; }

private:
    mutable std::mutex m_mutex;
    std::deque<EditorCommand> m_pendingDeque;
    std::vector<EditorCommand> m_lastDrained;
    u32 m_pending = 0;
    u32 m_applied = 0;
    u32 m_coalescedPosts = 0;
};

} // namespace fuse::editor
