#pragma once

#include <fuse/script/script_bind.hpp>
#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace fuse::script {

enum class ScriptBackendKind : u8 { Null, Lua };

/// VM creation options. Limits apply to the Lua backend only.
struct ScriptVMDesc {
    /// Strip host-escaping libraries: io, os (except clock/time/date/difftime), package/require,
    /// debug, dofile/loadfile/load/loadstring and string.dump. Scripts reach the engine only
    /// through the bound FUSE API tables.
    bool sandboxed = true;
    /// Hard cap on Lua heap bytes (0 = unlimited). Allocations past the cap fail with a Lua
    /// memory error inside the offending protected call; the VM stays usable.
    usize memory_limit_bytes = 0;
    /// Maximum VM instructions per protected call (0 = unlimited). Runaway scripts are aborted
    /// with a Lua error instead of hanging the game thread.
    u64 instruction_budget = 0;
    /// Forward `print` output to stdout in addition to the captured output buffer.
    bool echo_print = false;
};

/// Script VM — Lua when `FUSE_SCRIPT_LUA=1`, otherwise a null backend that records chunk names.
/// Every Lua entry point runs under `lua_pcall`: script errors are returned, never thrown.
class ScriptVM {
public:
    ScriptVM();
    ~ScriptVM();
    ScriptVM(const ScriptVM&) = delete;
    ScriptVM& operator=(const ScriptVM&) = delete;
    ScriptVM(ScriptVM&&) noexcept;
    ScriptVM& operator=(ScriptVM&&) noexcept;

    bool init(const ScriptVMDesc& desc = {});
    void shutdown();

    [[nodiscard]] bool is_initialized() const { return m_initialized; }
    [[nodiscard]] ScriptBackendKind backend_kind() const { return m_backend; }
    [[nodiscard]] const ScriptVMDesc& desc() const { return m_desc; }

    ScriptLoadResult load_string(const char* source, const char* chunk_name = "chunk");
    ScriptLoadResult load_file(const char* path);

    /// Record a non-Lua data chunk (e.g. UAISK behaviour-tree text) without executing it.
    void record_data_chunk(const char* chunk_name);

    [[nodiscard]] usize loaded_chunk_count() const { return m_loadedChunks.size(); }
    [[nodiscard]] const std::vector<std::string>& loaded_chunks() const { return m_loadedChunks; }

    /// True when `FUSE_SCRIPT_LUA=1` and the VM initialized a Lua state.
    [[nodiscard]] bool has_lua_backend() const;

    /// Non-owning Lua state (nullptr on the null backend). Owned and closed by this VM.
    [[nodiscard]] lua_State* lua_state() const;

    /// Call global function `name` with tagged args; first result is written to `out`.
    ScriptLoadResult call_global(const char* name,
                                 const bind::ScriptValue* args = nullptr,
                                 usize argc = 0,
                                 bind::ScriptValue* out = nullptr);
    [[nodiscard]] bool has_global_function(const char* name) const;

    /// Host code that touches the Lua stack. It runs inside `lua_pcall`, so any Lua error it
    /// raises (including from `lua_call` into script code) is caught. Keep objects with
    /// non-trivial destructors out of scope across calls that may raise.
    using ProtectedFn = void (*)(lua_State* L, void* user);

    /// Run `fn` protected, with a traceback handler, the per-call instruction budget and the
    /// memory limit armed. The Lua stack is restored afterwards; on error the message is kept
    /// in `last_error()` and `RuntimeError` is returned.
    ScriptLoadResult run_protected(ProtectedFn fn, void* user);

    [[nodiscard]] const std::string& last_error() const { return m_lastError; }
    [[nodiscard]] usize error_count() const { return m_errorCount; }

    /// Captured `print` output (newline-terminated lines).
    [[nodiscard]] const std::string& output() const;
    void clear_output();

    /// Current / peak Lua heap bytes (0 on the null backend).
    [[nodiscard]] usize memory_bytes() const;
    [[nodiscard]] usize peak_memory_bytes() const;
    void set_memory_limit(usize bytes);
    void set_instruction_budget(u64 instructions);
    /// Full garbage-collection cycle.
    void collect_garbage();

    struct Impl;

private:
    void reset_impl();

    ScriptLoadResult fail(ScriptLoadStatus status, const char* message);

    bool m_initialized = false;
    ScriptBackendKind m_backend = ScriptBackendKind::Null;
    ScriptVMDesc m_desc{};
    std::vector<std::string> m_loadedChunks;
    std::string m_lastError;
    usize m_errorCount = 0;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fuse::script
