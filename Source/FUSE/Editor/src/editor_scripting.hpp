#pragma once

// MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1 (private to fuse_editor_api): the editor console's Lua REPL.
//
// A ScriptHost (its own sandboxed VM, instruction budget + memory limit armed) with the engine API
// (Entity / Scene / SDF / CVar ...) bound to the editor registry, and a ScriptConsole attached to it
// for the REPL built-ins (echo, run, load, history, ...). Lua lines run in this VM outside PIE and in
// the PIE session's VM while playing (EditorHost picks). Built without fuse_script
// (FUSE_EDITOR_HAS_SCRIPT undefined) every call fails with "scripting unavailable".

#include <fuse/editor/editor_host.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::ecs {
class Registry;
} // namespace fuse::ecs

namespace fuse::script {
class ScriptVM;
} // namespace fuse::script

namespace fuse::editor {

class EditorScripting {
public:
    EditorScripting();
    ~EditorScripting();
    EditorScripting(const EditorScripting&) = delete;
    EditorScripting& operator=(const EditorScripting&) = delete;

    /// Creates the VM and binds the engine API to `registry` (which must outlive this object).
    bool init(ecs::Registry& registry, std::string& error);
    [[nodiscard]] bool ready() const;

    /// True when the first word of `line` names a ScriptConsole command.
    [[nodiscard]] bool isConsoleCommand(const std::string& line) const;
    /// Runs `line` through the ScriptConsole; its output lines go to `out`.
    bool runConsoleCommand(const std::string& line, std::vector<EditorConsoleLine>& out);

    /// The REPL VM (nullptr before init / without scripting).
    [[nodiscard]] script::ScriptVM* vm();

    /// Runs `source` in `vm` as a Lua chunk; when it does not parse as a statement it is retried as
    /// `print(<source>)` so a bare expression echoes its value. Captured `print` output becomes
    /// Info lines, errors an Error line.
    static bool runLua(script::ScriptVM& vm, const std::string& source, std::vector<EditorConsoleLine>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fuse::editor
