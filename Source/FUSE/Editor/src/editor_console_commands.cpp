#include <fuse/editor/editor_console_commands.hpp>

#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/editor_host.hpp>

#include <cctype>
#include <string>
#include <vector>

namespace fuse::editor {

namespace {

std::string lowerCopy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool usage(ConsolePanel& console, const char* text) {
    console.addLog(fuse::log::Level::Error, (std::string("usage: ") + text).c_str());
    return false;
}

} // namespace

void registerEditorConsoleCommands(ConsolePanel& console, EditorHost& host) {
    EditorHost* hostPtr = &host;

    const auto openScene = [hostPtr](const std::vector<std::string>& args, ConsolePanel& panel) {
        if (args.size() != 1u) {
            return usage(panel, "open <path.fuselevel>");
        }
        hostPtr->postFromUi(makeOpenSceneCommand(args[0]));
        return true;
    };
    console.registerCommand("open", openScene);
    console.registerCommand("load", openScene);

    console.registerCommand("save", [hostPtr](const std::vector<std::string>& args, ConsolePanel& panel) {
        if (args.size() > 1u) {
            return usage(panel, "save [path.fuselevel]");
        }
        hostPtr->postFromUi(args.empty() ? makeSaveSceneCommand() : makeSaveSceneAsCommand(args[0]));
        return true;
    });

    console.registerCommand("new", [hostPtr](const std::vector<std::string>& args, ConsolePanel& panel) {
        bool scene2D = false;
        usize nameArg = 0;
        if (!args.empty()) {
            const std::string kind = lowerCopy(args[0]);
            if (kind == "2d" || kind == "3d") {
                scene2D = kind == "2d";
                nameArg = 1;
            }
        }
        if (args.size() > nameArg + 1u) {
            return usage(panel, "new [2d|3d] [name]");
        }
        hostPtr->postFromUi(makeNewSceneCommand(scene2D, nameArg < args.size() ? args[nameArg] : std::string()));
        return true;
    });

    console.registerCommand("project.new", [hostPtr](const std::vector<std::string>& args, ConsolePanel& panel) {
        if (args.empty() || args.size() > 3u) {
            return usage(panel, "project.new <dir> [name] [3d|2d|both]");
        }
        u32 flags = kProjectEnable3D | kProjectEnable2D | kProjectEnableUI;
        if (args.size() == 3u) {
            const std::string dims = lowerCopy(args[2]);
            if (dims == "3d") {
                flags = kProjectEnable3D | kProjectEnableUI;
            } else if (dims == "2d") {
                flags = kProjectEnable2D | kProjectEnableUI;
            } else if (dims != "both") {
                return usage(panel, "project.new <dir> [name] [3d|2d|both]");
            }
        }
        hostPtr->postFromUi(makeNewProjectCommand(args[0], args.size() > 1u ? args[1] : std::string(), flags));
        return true;
    });

    console.registerCommand("project.open", [hostPtr](const std::vector<std::string>& args, ConsolePanel& panel) {
        if (args.size() != 1u) {
            return usage(panel, "project.open <dir|project.json>");
        }
        hostPtr->postFromUi(makeOpenProjectCommand(args[0]));
        return true;
    });

    const struct {
        const char* name;
        CommandKind kind;
    } transport[] = {
        {"play", CommandKind::StartPlay},
        {"stop", CommandKind::StopPlay},
        {"pause", CommandKind::PausePlay},
        {"resume", CommandKind::ResumePlay},
        {"step", CommandKind::StepPlay},
    };
    for (const auto& entry : transport) {
        const CommandKind kind = entry.kind;
        console.registerCommand(entry.name, [hostPtr, kind](const std::vector<std::string>&, ConsolePanel&) {
            hostPtr->postFromUi(makeTransportCommand(kind));
            return true;
        });
    }

    // Game-thread commands: the raw line (quotes intact, e.g. `lua print("a b")`) is parsed by
    // EditorHost::executeConsoleLine.
    for (const char* name : {"cvar", "stat", "lua"}) {
        console.registerCommand(name, [hostPtr](const std::vector<std::string>&, ConsolePanel& panel) {
            hostPtr->postFromUi(makeConsoleExecCommand(panel.lastExecutedCommand()));
            return true;
        });
    }

    console.setFallbackHandler([hostPtr](const std::string& line, ConsolePanel&) {
        hostPtr->postFromUi(makeConsoleExecCommand(line));
        return true;
    });
}

} // namespace fuse::editor
