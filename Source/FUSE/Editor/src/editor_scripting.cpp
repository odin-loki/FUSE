#include "editor_scripting.hpp"

#include <fuse/ecs/registry.hpp>

#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT
#include <fuse/config/cvar.hpp>
#include <fuse/script/script_console.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_host.hpp>
#include <fuse/script/script_vm.hpp>
#endif

#include <cctype>
#include <utility>

namespace fuse::editor {

namespace {

[[maybe_unused]] void appendLines(const std::string& text, fuse::log::Level level, std::vector<EditorConsoleLine>& out) {
    usize start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        out.push_back({level, text.substr(start, end - start)});
        start = end + 1u;
    }
}

} // namespace

#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT

struct EditorScripting::Impl {
    script::ScriptHost host;
    script::ScriptConsole console;
    bool ready = false;
};

EditorScripting::EditorScripting() : m_impl(std::make_unique<Impl>()) {}

EditorScripting::~EditorScripting() {
    if (m_impl->ready) {
        m_impl->console.detach();
        m_impl->host.shutdown();
    }
}

bool EditorScripting::init(ecs::Registry& registry, std::string& error) {
    if (m_impl->ready) {
        return true;
    }
    if (!m_impl->host.init()) {
        error = "script host init failed";
        return false;
    }
    script::ScriptVM& vm = m_impl->host.vm();
    if (!vm.has_lua_backend()) {
        m_impl->host.shutdown();
        error = "no Lua backend in this build (fuse_script null backend)";
        return false;
    }
    // A console line must never hang or exhaust the game thread.
    vm.set_instruction_budget(50'000'000u);
    vm.set_memory_limit(64u * 1024u * 1024u);
    script::ScriptEngineBindings bindings;
    bindings.registry = &registry;
    bindings.cvars = &config::CVarRegistry::global();
    if (!script::bind_engine_api(vm, bindings)) {
        m_impl->host.shutdown();
        error = "binding the engine script API failed";
        return false;
    }
    m_impl->console.attach(&m_impl->host);
    m_impl->ready = true;
    return true;
}

bool EditorScripting::ready() const {
    return m_impl->ready;
}

bool EditorScripting::isConsoleCommand(const std::string& line) const {
    usize begin = 0;
    while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
        ++begin;
    }
    usize end = begin;
    while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end])) == 0) {
        ++end;
    }
    if (end == begin) {
        return false;
    }
    const std::string word = line.substr(begin, end - begin);
    if (!m_impl->console.has_command(word.c_str())) {
        return false;
    }
    // `list = {}` / `history.x = 1` are Lua, not the `list` / `history` built-ins.
    usize next = end;
    while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next])) != 0) {
        ++next;
    }
    if (next < line.size()) {
        const char ch = line[next];
        if (ch == '=' || ch == '(' || ch == '.' || ch == ':' || ch == '[' || ch == '{' || ch == '"' ||
            ch == '\'') {
            return false;
        }
    }
    return true;
}

bool EditorScripting::runConsoleCommand(const std::string& line, std::vector<EditorConsoleLine>& out) {
    if (!m_impl->ready) {
        out.push_back({fuse::log::Level::Error, "scripting unavailable"});
        return false;
    }
    script::ScriptVM& vm = m_impl->host.vm();
    vm.clear_output();
    const script::ScriptConsoleCommandResult result = m_impl->console.execute(line.c_str());
    // `run` executes Lua: its prints land in the VM's output buffer.
    appendLines(vm.output(), fuse::log::Level::Info, out);
    vm.clear_output();
    if (!result.output.empty()) {
        appendLines(result.output, result.ok() ? fuse::log::Level::Info : fuse::log::Level::Error, out);
    }
    return result.ok();
}

script::ScriptVM* EditorScripting::vm() {
    return m_impl->ready ? &m_impl->host.vm() : nullptr;
}

bool EditorScripting::runLua(script::ScriptVM& vm, const std::string& source, std::vector<EditorConsoleLine>& out) {
    vm.clear_output();
    script::ScriptLoadResult result = vm.load_string(source.c_str(), "console");
    std::string error = result.ok() || result.message == nullptr ? std::string() : std::string(result.message);
    if (result.status == script::ScriptLoadStatus::ParseError) {
        // Not a statement: echo it as an expression (`1 + 1` -> 2).
        const std::string wrapped = "print(" + source + ")";
        const script::ScriptLoadResult echoed = vm.load_string(wrapped.c_str(), "console");
        if (echoed.status != script::ScriptLoadStatus::ParseError) {
            result = echoed;
            error = echoed.ok() || echoed.message == nullptr ? std::string() : std::string(echoed.message);
        }
    }
    appendLines(vm.output(), fuse::log::Level::Info, out);
    vm.clear_output();
    if (!result.ok()) {
        out.push_back({fuse::log::Level::Error, error.empty() ? std::string("lua error") : error});
        return false;
    }
    return true;
}

#else // !FUSE_EDITOR_HAS_SCRIPT

struct EditorScripting::Impl {};

EditorScripting::EditorScripting() : m_impl(std::make_unique<Impl>()) {}
EditorScripting::~EditorScripting() = default;

bool EditorScripting::init(ecs::Registry& /*registry*/, std::string& error) {
    error = "scripting unavailable (editor built without fuse_script)";
    return false;
}

bool EditorScripting::ready() const {
    return false;
}

bool EditorScripting::isConsoleCommand(const std::string& /*line*/) const {
    return false;
}

bool EditorScripting::runConsoleCommand(const std::string& /*line*/, std::vector<EditorConsoleLine>& out) {
    out.push_back({fuse::log::Level::Error, "scripting unavailable (editor built without fuse_script)"});
    return false;
}

script::ScriptVM* EditorScripting::vm() {
    return nullptr;
}

bool EditorScripting::runLua(script::ScriptVM& /*vm*/, const std::string& /*source*/, std::vector<EditorConsoleLine>& out) {
    out.push_back({fuse::log::Level::Error, "scripting unavailable (editor built without fuse_script)"});
    return false;
}

#endif

} // namespace fuse::editor
