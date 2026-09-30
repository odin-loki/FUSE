#pragma once

// MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1: the editor console's engine commands.
//
// Registered on a ConsolePanel (UI thread). Every command posts an EditorCommand to the host's
// CommandQueue; the game thread applies it in EditorHost::gameTick and reports back through the
// host's console output queue (EditorHost::drainConsoleOutput):
//
//   open|load <path.fuselevel>          open a scene             save [path]      save / save as
//   new [2d|3d] [name]                  new empty scene
//   project.new <dir> [name] [3d|2d|both]   project.open <dir|project.json>
//   play | stop | pause | resume | step PIE transport
//   cvar <name> [value] | cvar list [prefix]    stat            lua <code>
//
// Any other line (not `help` / `clear` / a command above) is sent to the game thread's Lua REPL
// (ScriptConsole built-ins such as `echo`, `run`, `history`; otherwise Lua source, e.g.
// `print(1+1)` or a bare expression `1+1`).

namespace fuse::editor {

class ConsolePanel;
class EditorHost;

/// Registers the commands above and the Lua fallback on `console`. `host` must outlive `console`.
void registerEditorConsoleCommands(ConsolePanel& console, EditorHost& host);

} // namespace fuse::editor
