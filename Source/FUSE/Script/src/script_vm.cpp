#include <fuse/script/script_vm.hpp>

#include <fuse/alloc/size_class_allocator.hpp>

#include "script_lua_compat.hpp"

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
#include <fuse/script/script_bind_lua.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace fuse::script {

namespace {

/// Captured `print` output is trimmed to the newest half once it grows past this size.
constexpr usize kMaxCapturedOutput = 1u << 20;

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
/// Instructions between budget-hook checks (the budget is enforced at this granularity).
constexpr int kBudgetHookGranularity = 1000;
#endif

} // namespace

struct ScriptVM::Impl {
    std::string output;
    bool echo_print = false;

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    lua_State* state = nullptr;
    /// Lua heap: every lua_Alloc request is served from this size-class pool, so a steady-state
    /// script frame (GC'd temporaries, table resizes) recycles pooled blocks instead of reaching
    /// the system heap (FUSE_MASTER_PLAN B1.8).
    alloc::SizeClassAllocator heap{alloc::SizeClassAllocatorDesc{"script.lua", 64u * 1024u, 0u, false}};
    usize bytes = 0;
    usize peak_bytes = 0;
    usize memory_limit = 0;
    u64 instruction_budget = 0;
    u64 executed = 0;
    int protected_depth = 0;
#endif
};

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
namespace {

ScriptVM::Impl* impl_from(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<ScriptVM::Impl*>(ud);
}

void* counting_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* impl = static_cast<ScriptVM::Impl*>(ud);
    const usize old_size = (ptr != nullptr) ? osize : 0u;
    if (nsize == 0) {
        impl->heap.deallocate(ptr, old_size);
        impl->bytes -= old_size;
        return nullptr;
    }
    // Only growth may fail (Lua requires shrinking reallocations to succeed), and only inside
    // protected host calls so an allocation failure never reaches the Lua panic handler.
    if (impl->memory_limit != 0 && impl->protected_depth > 0 && nsize > old_size &&
        impl->bytes - old_size + nsize > impl->memory_limit) {
        return nullptr;
    }
    void* block = impl->heap.reallocate(ptr, old_size, nsize);
    if (block == nullptr) {
        return nullptr;
    }
    impl->bytes = impl->bytes - old_size + nsize;
    if (impl->bytes > impl->peak_bytes) {
        impl->peak_bytes = impl->bytes;
    }
    return block;
}

void budget_hook(lua_State* L, lua_Debug* /*ar*/) {
    ScriptVM::Impl* impl = impl_from(L);
    if (impl == nullptr || impl->instruction_budget == 0) {
        return;
    }
    impl->executed += static_cast<u64>(kBudgetHookGranularity);
    if (impl->executed > impl->instruction_budget) {
        // lua_pushfstring has no 64-bit integer conversion before Lua 5.3: format here.
        char message[96];
        std::snprintf(message, sizeof(message), "instruction budget exceeded (%llu instructions)",
                      static_cast<unsigned long long>(impl->instruction_budget));
        luaL_error(L, "%s", message);
    }
}

void apply_budget_hook(ScriptVM::Impl& impl) {
    if (impl.state == nullptr) {
        return;
    }
    if (impl.instruction_budget == 0) {
        lua_sethook(impl.state, nullptr, 0, 0);
    } else {
        lua_sethook(impl.state, budget_hook, LUA_MASKCOUNT, kBudgetHookGranularity);
    }
}

int traceback_handler(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (message == nullptr) {
        message = "(non-string script error)";
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

struct ProtectedCall {
    ScriptVM::ProtectedFn fn = nullptr;
    void* user = nullptr;
};

int protected_trampoline(lua_State* L) {
    auto* call = static_cast<ProtectedCall*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    call->fn(L, call->user);
    return 0;
}

int captured_print(lua_State* L) {
    ScriptVM::Impl* impl = impl_from(L);
    std::string line;
    const int argc = lua_gettop(L);
    for (int i = 1; i <= argc; ++i) {
        size_t len = 0;
        const char* text = luaL_tolstring(L, i, &len);
        if (i > 1) {
            line.push_back('\t');
        }
        line.append(text, len);
        lua_pop(L, 1);
    }
    line.push_back('\n');
    if (impl != nullptr) {
        if (impl->output.size() + line.size() > kMaxCapturedOutput) {
            impl->output.erase(0, impl->output.size() / 2u);
        }
        impl->output += line;
        if (impl->echo_print) {
            std::fwrite(line.data(), 1, line.size(), stdout);
        }
    }
    return 0;
}

void set_global_nil(lua_State* L, const char* name) {
    lua_pushnil(L);
    lua_setglobal(L, name);
}

void apply_sandbox(lua_State* L) {
    static const char* const kRemovedGlobals[] = {
        "dofile", "loadfile", "load", "loadstring", "require", "module",
        "package", "io", "debug", "collectgarbage",
    };
    for (const char* name : kRemovedGlobals) {
        set_global_nil(L, name);
    }

    // os: keep only the pure time helpers.
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_createtable(L, 0, 4);
        static const char* const kKeptOs[] = {"clock", "time", "date", "difftime"};
        for (const char* name : kKeptOs) {
            lua_getfield(L, -2, name);
            lua_setfield(L, -2, name);
        }
        lua_setglobal(L, "os");
    }
    lua_pop(L, 1);

    lua_getglobal(L, "string");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
    }
    lua_pop(L, 1);
}

} // namespace
#endif

ScriptVM::ScriptVM() : m_impl(std::make_unique<Impl>()) {}

ScriptVM::~ScriptVM() { shutdown(); }

ScriptVM::ScriptVM(ScriptVM&& other) noexcept
    : m_initialized(other.m_initialized),
      m_backend(other.m_backend),
      m_desc(other.m_desc),
      m_loadedChunks(std::move(other.m_loadedChunks)),
      m_lastError(std::move(other.m_lastError)),
      m_errorCount(other.m_errorCount),
      m_impl(std::move(other.m_impl)) {
    other.m_initialized = false;
    other.m_backend = ScriptBackendKind::Null;
    other.m_errorCount = 0;
    other.reset_impl();
}

ScriptVM& ScriptVM::operator=(ScriptVM&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_initialized = other.m_initialized;
        m_backend = other.m_backend;
        m_desc = other.m_desc;
        m_loadedChunks = std::move(other.m_loadedChunks);
        m_lastError = std::move(other.m_lastError);
        m_errorCount = other.m_errorCount;
        m_impl = std::move(other.m_impl);
        other.m_initialized = false;
        other.m_backend = ScriptBackendKind::Null;
        other.m_errorCount = 0;
        other.reset_impl();
    }
    return *this;
}

void ScriptVM::reset_impl() {
    try {
        m_impl = std::make_unique<Impl>();
    } catch (...) {
        m_impl.reset();
    }
}

bool ScriptVM::has_lua_backend() const {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    return m_impl != nullptr && m_impl->state != nullptr;
#else
    return false;
#endif
}

lua_State* ScriptVM::lua_state() const {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    return m_impl != nullptr ? m_impl->state : nullptr;
#else
    return nullptr;
#endif
}

bool ScriptVM::init(const ScriptVMDesc& desc) {
    if (m_initialized) {
        return true;
    }
    if (m_impl == nullptr) {
        reset_impl();
        if (m_impl == nullptr) {
            return false;
        }
    }

    m_desc = desc;
    m_impl->output.clear();
    m_impl->echo_print = desc.echo_print;

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    m_impl->bytes = 0;
    m_impl->peak_bytes = 0;
    m_impl->memory_limit = 0; // limits apply after the standard libraries are open
    m_impl->instruction_budget = 0;
    m_impl->executed = 0;
    m_impl->protected_depth = 0;
    m_impl->state = lua_newstate(counting_alloc, m_impl.get());
    if (m_impl->state == nullptr) {
        m_backend = ScriptBackendKind::Null;
        return false;
    }

    lua_State* L = m_impl->state;
    luaL_openlibs(L);
    lua_pushcfunction(L, captured_print);
    lua_setglobal(L, "print");
    if (desc.sandboxed) {
        apply_sandbox(L);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);

    m_impl->memory_limit = desc.memory_limit_bytes;
    m_impl->instruction_budget = desc.instruction_budget;
    apply_budget_hook(*m_impl);
    m_backend = ScriptBackendKind::Lua;
#else
    m_backend = ScriptBackendKind::Null;
#endif

    m_loadedChunks.clear();
    m_lastError.clear();
    m_errorCount = 0;
    m_initialized = true;
    return true;
}

void ScriptVM::shutdown() {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (m_impl != nullptr && m_impl->state != nullptr) {
        m_impl->memory_limit = 0;
        lua_close(m_impl->state);
        m_impl->state = nullptr;
    }
#endif

    m_loadedChunks.clear();
    m_initialized = false;
    m_backend = ScriptBackendKind::Null;
}

ScriptLoadResult ScriptVM::fail(ScriptLoadStatus status, const char* message) {
    m_lastError = (message != nullptr) ? message : "script error";
    ++m_errorCount;
    return {status, m_lastError.c_str()};
}

ScriptLoadResult ScriptVM::run_protected(ProtectedFn fn, void* user) {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (!has_lua_backend()) {
        return fail(ScriptLoadStatus::BackendUnavailable, "lua backend unavailable");
    }
    if (fn == nullptr) {
        return fail(ScriptLoadStatus::InvalidArgument, "run_protected: null function");
    }
    lua_State* L = m_impl->state;
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, 3)) {
        return fail(ScriptLoadStatus::RuntimeError, "lua stack overflow");
    }

    ProtectedCall call{fn, user};
    // Light C functions and light userdata do not allocate: nothing here can raise unprotected.
    lua_pushcfunction(L, traceback_handler);
    lua_pushcfunction(L, protected_trampoline);
    lua_pushlightuserdata(L, &call);
    if (m_impl->protected_depth == 0) {
        m_impl->executed = 0;
    }
    ++m_impl->protected_depth;
    const int status = lua_pcall(L, 1, 0, base + 1);
    --m_impl->protected_depth;

    if (status != LUA_OK) {
        const char* error = lua_tostring(L, -1);
        m_lastError = (error != nullptr) ? error
                      : (status == LUA_ERRMEM ? "not enough memory" : "(non-string script error)");
        ++m_errorCount;
        lua_settop(L, base);
        return {ScriptLoadStatus::RuntimeError, m_lastError.c_str()};
    }
    lua_settop(L, base);
    return {ScriptLoadStatus::Ok, nullptr};
#else
    (void)fn;
    (void)user;
    return fail(ScriptLoadStatus::BackendUnavailable, "lua backend unavailable");
#endif
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
namespace {

struct ChunkLoad {
    const char* source = nullptr; // buffer source, or nullptr for a file
    const char* name = nullptr;   // chunk name, or file path
    int load_status = LUA_OK;
    std::string* parse_error = nullptr;
};

void load_and_run_chunk(lua_State* L, void* user) {
    auto* load = static_cast<ChunkLoad*>(user);
    load->load_status = (load->source != nullptr)
                            ? luaL_loadbuffer(L, load->source, std::strlen(load->source), load->name)
                            : luaL_loadfile(L, load->name);
    if (load->load_status != LUA_OK) {
        const char* error = lua_tostring(L, -1);
        load->parse_error->assign(error != nullptr ? error : "lua parse error");
        return;
    }
    lua_call(L, 0, 0);
}

} // namespace
#endif

ScriptLoadResult ScriptVM::load_string(const char* source, const char* chunk_name) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (source == nullptr) {
        return {ScriptLoadStatus::InvalidArgument, "source is null"};
    }

    const char* name = (chunk_name != nullptr && chunk_name[0] != '\0') ? chunk_name : "chunk";

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (has_lua_backend()) {
        std::string parse_error;
        ChunkLoad load{source, name, LUA_OK, &parse_error};
        const ScriptLoadResult run_result = run_protected(load_and_run_chunk, &load);
        if (load.load_status != LUA_OK) {
            return fail(load.load_status == LUA_ERRSYNTAX ? ScriptLoadStatus::ParseError
                                                          : ScriptLoadStatus::RuntimeError,
                        parse_error.c_str());
        }
        if (!run_result.ok()) {
            return run_result;
        }

        m_loadedChunks.emplace_back(name);
        return {ScriptLoadStatus::Ok, nullptr};
    }
#endif

    m_loadedChunks.emplace_back(name);
    return {ScriptLoadStatus::Ok, nullptr};
}

ScriptLoadResult ScriptVM::load_file(const char* path) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (path == nullptr || path[0] == '\0') {
        return {ScriptLoadStatus::InvalidArgument, "path is empty"};
    }

    const std::filesystem::path file_path(path);
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec)) {
        return fail(ScriptLoadStatus::FileNotFound, path);
    }

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (has_lua_backend()) {
        std::string parse_error;
        ChunkLoad load{nullptr, path, LUA_OK, &parse_error};
        const ScriptLoadResult run_result = run_protected(load_and_run_chunk, &load);
        if (load.load_status != LUA_OK) {
            return fail(load.load_status == LUA_ERRSYNTAX ? ScriptLoadStatus::ParseError
                                                          : ScriptLoadStatus::RuntimeError,
                        parse_error.c_str());
        }
        if (!run_result.ok()) {
            return run_result;
        }

        m_loadedChunks.emplace_back(file_path.filename().string());
        return {ScriptLoadStatus::Ok, nullptr};
    }
#endif

    m_loadedChunks.emplace_back(file_path.filename().string());
    return {ScriptLoadStatus::Ok, nullptr};
}

void ScriptVM::record_data_chunk(const char* chunk_name) {
    if (!m_initialized) {
        return;
    }
    m_loadedChunks.emplace_back((chunk_name != nullptr && chunk_name[0] != '\0') ? chunk_name : "chunk");
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
namespace {

struct GlobalProbe {
    const char* name = nullptr;
    bool is_function = false;
};

void probe_global(lua_State* L, void* user) {
    auto* probe = static_cast<GlobalProbe*>(user);
    lua_getglobal(L, probe->name);
    probe->is_function = lua_isfunction(L, -1);
}

struct GlobalCall {
    const char* name = nullptr;
    const bind::ScriptValue* args = nullptr;
    usize argc = 0;
    bind::ScriptValue* out = nullptr;
    bool found = false;
};

void call_global_fn(lua_State* L, void* user) {
    auto* call = static_cast<GlobalCall*>(user);
    luaL_checkstack(L, static_cast<int>(call->argc) + 2, "call_global arguments");
    lua_getglobal(L, call->name);
    if (!lua_isfunction(L, -1)) {
        return;
    }
    call->found = true;
    for (usize i = 0; i < call->argc; ++i) {
        bind::lua::push_to_stack(L, call->args[i]);
    }
    lua_call(L, static_cast<int>(call->argc), 1);
    if (call->out != nullptr) {
        *call->out = bind::lua::read_from_stack(L, -1);
    }
}

void full_gc(lua_State* L, void* /*user*/) { lua_gc(L, LUA_GCCOLLECT, 0); }

} // namespace
#endif

bool ScriptVM::has_global_function(const char* name) const {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (!has_lua_backend() || name == nullptr) {
        return false;
    }
    GlobalProbe probe{name, false};
    // Probing is logically const; errors (e.g. a raising __index on _G) read as "not a function".
    const_cast<ScriptVM*>(this)->run_protected(probe_global, &probe);
    return probe.is_function;
#else
    (void)name;
    return false;
#endif
}

ScriptLoadResult ScriptVM::call_global(const char* name,
                                       const bind::ScriptValue* args,
                                       usize argc,
                                       bind::ScriptValue* out) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (name == nullptr || name[0] == '\0') {
        return {ScriptLoadStatus::InvalidArgument, "function name is empty"};
    }
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (!has_lua_backend()) {
        return fail(ScriptLoadStatus::BackendUnavailable, "lua backend unavailable");
    }
    GlobalCall call{name, args, argc, out, false};
    const ScriptLoadResult result = run_protected(call_global_fn, &call);
    if (!result.ok()) {
        return result;
    }
    if (!call.found) {
        return fail(ScriptLoadStatus::InvalidArgument,
                    (std::string("global function not found: ") + name).c_str());
    }
    return {ScriptLoadStatus::Ok, nullptr};
#else
    (void)args;
    (void)argc;
    (void)out;
    return fail(ScriptLoadStatus::BackendUnavailable, "lua backend unavailable");
#endif
}

const std::string& ScriptVM::output() const {
    static const std::string kEmpty;
    return m_impl != nullptr ? m_impl->output : kEmpty;
}

void ScriptVM::clear_output() {
    if (m_impl != nullptr) {
        m_impl->output.clear();
    }
}

usize ScriptVM::memory_bytes() const {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    return has_lua_backend() ? m_impl->bytes : 0u;
#else
    return 0u;
#endif
}

usize ScriptVM::peak_memory_bytes() const {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    return has_lua_backend() ? m_impl->peak_bytes : 0u;
#else
    return 0u;
#endif
}

void ScriptVM::set_memory_limit(usize bytes) {
    m_desc.memory_limit_bytes = bytes;
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (m_impl != nullptr) {
        m_impl->memory_limit = bytes;
    }
#endif
}

void ScriptVM::set_instruction_budget(u64 instructions) {
    m_desc.instruction_budget = instructions;
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (m_impl != nullptr) {
        m_impl->instruction_budget = instructions;
        apply_budget_hook(*m_impl);
    }
#endif
}

void ScriptVM::collect_garbage() {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (has_lua_backend()) {
        run_protected(full_gc, nullptr);
    }
#endif
}

} // namespace fuse::script
