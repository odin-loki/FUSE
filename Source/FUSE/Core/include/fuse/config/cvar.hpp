#pragma once

// GAP-CVAR: console variables and persisted user config.
//
// One typed config system for the console, Lua, the runtime and the editor. A CVar has a name
// ("r.renderScale"), a type (bool, int, float, string, enum), flags, a description and — for
// int / float — an inclusive range. Values arrive from four sources, and a value from a lower
// source never replaces one from a higher source:
//
//     Default  <  ConfigFile  <  CommandLine  <  Runtime
//
// Reads are thread-safe and lock-free for bool / int / float / enum (one atomic word per cvar);
// string reads copy an immutable snapshot under a short per-cvar lock. Writes (set, reset,
// load, callbacks) happen on the game thread. Change callbacks run on the writing thread, once
// per change of the live value, after the new value is visible to readers.
//
// Flags:
//   Archive         saved by save_config_text/_file (only archive cvars are persisted, and only
//                   when a config file or a runtime set gave them a value — command-line
//                   overrides are never written back).
//   ReadOnly        only the command line may set it (config files and runtime sets fail).
//   Cheat           every set fails unless cheats are enabled; never persisted.
//   RequiresRestart after finish_startup() a set is stored (and persisted) but the live value
//                   stays until the next start; `restart_pending()` reports it.
//
// Names registered later still receive earlier config-file / command-line values: those are
// kept as pending and applied (config first, then command line) at registration.

#include <fuse/types.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::config {

enum class CVarType : u8 { Bool, Int, Float, String, Enum };

enum class CVarFlags : u32 {
    None = 0,
    Archive = 1u << 0,
    ReadOnly = 1u << 1,
    Cheat = 1u << 2,
    RequiresRestart = 1u << 3,
};

constexpr CVarFlags operator|(CVarFlags a, CVarFlags b) {
    return static_cast<CVarFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool has_flag(CVarFlags set, CVarFlags flag) {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0u;
}

/// Where a value came from, in precedence order (a higher source is never replaced by a lower).
enum class CVarSource : u8 { Default = 0, ConfigFile = 1, CommandLine = 2, Runtime = 3 };

enum class CVarResult : u8 {
    Ok,             ///< live value changed (callbacks fired)
    Unchanged,      ///< same value as before (source may have been raised; no callback)
    Deferred,       ///< RequiresRestart after startup: stored, applies on the next start
    Overridden,     ///< a higher source already set it; the live value is kept
    UnknownName,
    ParseError,     ///< text is not a value of the cvar's type
    OutOfRange,
    ReadOnly,
    CheatProtected, ///< cheat cvar while cheats are disabled
    TypeMismatch,   ///< typed setter of the wrong type
};

[[nodiscard]] const char* to_string(CVarResult result);
[[nodiscard]] const char* to_string(CVarType type);
[[nodiscard]] const char* to_string(CVarSource source);
[[nodiscard]] inline bool succeeded(CVarResult r) {
    return r == CVarResult::Ok || r == CVarResult::Unchanged || r == CVarResult::Deferred;
}

class CVarRegistry;

/// One registered cvar. Owned by its registry (stable address for the registry's lifetime).
class CVarEntry {
public:
    CVarEntry(const CVarEntry&) = delete;
    CVarEntry& operator=(const CVarEntry&) = delete;

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::string& description() const { return m_description; }
    [[nodiscard]] CVarType type() const { return m_type; }
    [[nodiscard]] CVarFlags flags() const { return m_flags; }
    [[nodiscard]] bool has(CVarFlags flag) const { return has_flag(m_flags, flag); }

    // ---- thread-safe reads ------------------------------------------------------------------
    [[nodiscard]] bool get_bool() const { return m_word.load(std::memory_order_acquire) != 0u; }
    [[nodiscard]] i32 get_int() const;
    [[nodiscard]] f32 get_float() const;
    /// Enum: index into enum_values().
    [[nodiscard]] i32 get_enum() const { return get_int(); }
    [[nodiscard]] std::string get_string() const;
    /// The live value as text (the form the config file and the console use).
    [[nodiscard]] std::string value_text() const;
    [[nodiscard]] CVarSource source() const {
        return static_cast<CVarSource>(m_source.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool restart_pending() const { return m_restartPending.load(std::memory_order_acquire); }

    // ---- immutable metadata ------------------------------------------------------------------
    [[nodiscard]] const std::string& default_text() const { return m_defaultText; }
    [[nodiscard]] bool has_range() const { return m_hasRange; }
    [[nodiscard]] f64 range_low() const { return m_rangeLow; }
    [[nodiscard]] f64 range_high() const { return m_rangeHigh; }
    [[nodiscard]] const std::vector<std::string>& enum_values() const { return m_enumValues; }

    // ---- game-thread state -------------------------------------------------------------------
    /// Text a pending RequiresRestart set will apply at the next start (empty when none).
    [[nodiscard]] const std::string& pending_text() const { return m_pendingText; }
    /// Value the user config file should hold (from a config file or a runtime set).
    [[nodiscard]] const std::optional<std::string>& archived_text() const { return m_archived; }

private:
    friend class CVarRegistry;
    CVarEntry() = default;

    std::string m_name;
    std::string m_description;
    CVarType m_type = CVarType::Bool;
    CVarFlags m_flags = CVarFlags::None;
    std::string m_defaultText;
    bool m_hasRange = false;
    f64 m_rangeLow = 0.0;
    f64 m_rangeHigh = 0.0;
    std::vector<std::string> m_enumValues;

    /// bool: 0/1; int / enum: the i32 bit pattern; float: the f32 bit pattern.
    std::atomic<u32> m_word{0};
    std::atomic<u8> m_source{static_cast<u8>(CVarSource::Default)};
    std::atomic<bool> m_restartPending{false};
    mutable std::mutex m_stringMutex;
    std::shared_ptr<const std::string> m_string;

    std::optional<std::string> m_archived;
    std::string m_pendingText;
    struct Callback {
        u64 id = 0;
        std::function<void(const CVarEntry&)> fn;
    };
    std::vector<Callback> m_callbacks;
};

using CVarCallback = std::function<void(const CVarEntry&)>;

/// A problem found while reading a config file or command line (the rest is still applied).
struct CVarIssue {
    u32 line = 0; ///< config file line (1-based) or argv index; 0 when not applicable
    std::string name;
    CVarResult result = CVarResult::Ok;
    std::string message;
};

class CVarRegistry {
public:
    CVarRegistry();
    ~CVarRegistry();
    CVarRegistry(const CVarRegistry&) = delete;
    CVarRegistry& operator=(const CVarRegistry&) = delete;

    /// Process-wide registry used by the engine cvars and static CVar<T> objects.
    static CVarRegistry& global();

    // ---- registration (game thread / static init) -------------------------------------------
    // Registering an existing name with the same type returns the existing entry; with another
    // type (or an invalid name / default) returns nullptr.
    CVarEntry* register_bool(std::string_view name, bool default_value, std::string_view description,
                             CVarFlags flags = CVarFlags::None);
    CVarEntry* register_int(std::string_view name, i32 default_value, i32 low, i32 high,
                            std::string_view description, CVarFlags flags = CVarFlags::None);
    CVarEntry* register_float(std::string_view name, f32 default_value, f32 low, f32 high,
                              std::string_view description, CVarFlags flags = CVarFlags::None);
    CVarEntry* register_string(std::string_view name, std::string_view default_value,
                               std::string_view description, CVarFlags flags = CVarFlags::None);
    CVarEntry* register_enum(std::string_view name, std::initializer_list<std::string_view> values,
                             i32 default_index, std::string_view description, CVarFlags flags = CVarFlags::None);
    CVarEntry* register_enum(std::string_view name, const std::vector<std::string>& values, i32 default_index,
                             std::string_view description, CVarFlags flags = CVarFlags::None);

    [[nodiscard]] CVarEntry* find(std::string_view name);
    [[nodiscard]] const CVarEntry* find(std::string_view name) const;
    [[nodiscard]] usize count() const;

    // ---- writes (game thread) ----------------------------------------------------------------
    CVarResult set(std::string_view name, std::string_view text, CVarSource source = CVarSource::Runtime);
    CVarResult set(CVarEntry& entry, std::string_view text, CVarSource source = CVarSource::Runtime);
    CVarResult set_bool(CVarEntry& entry, bool value, CVarSource source = CVarSource::Runtime);
    CVarResult set_int(CVarEntry& entry, i32 value, CVarSource source = CVarSource::Runtime);
    CVarResult set_float(CVarEntry& entry, f32 value, CVarSource source = CVarSource::Runtime);
    CVarResult set_string(CVarEntry& entry, std::string_view value, CVarSource source = CVarSource::Runtime);
    CVarResult set_enum(CVarEntry& entry, i32 index, CVarSource source = CVarSource::Runtime);
    /// Back to the default value and source; drops the archived and pending values.
    CVarResult reset(std::string_view name);

    /// Per-cvar change callback; returns an id for remove_callback (0 when `entry` is foreign).
    u64 add_callback(CVarEntry& entry, CVarCallback callback);
    /// Callback for every cvar (console echo, editor settings panel).
    u64 add_listener(CVarCallback callback);
    bool remove_callback(u64 id);

    void set_cheats_enabled(bool enabled) { m_cheats = enabled; }
    [[nodiscard]] bool cheats_enabled() const { return m_cheats; }
    /// From now on RequiresRestart cvars defer their sets.
    void finish_startup() { m_startupFinished = true; }
    [[nodiscard]] bool startup_finished() const { return m_startupFinished; }

    // ---- persistence -------------------------------------------------------------------------
    /// `key = value` lines; `#`, `;` and `//` start comments; `[section]` prefixes following keys
    /// with "section."; values may be "double-quoted" with \" \\ \n \t escapes. Unknown names are
    /// kept (applied when registered later, and written back by save). Returns values applied.
    usize load_config_text(std::string_view text, std::vector<CVarIssue>* issues = nullptr);
    /// False when the file cannot be read (a missing file is not an issue: returns true, 0 applied).
    bool load_config_file(const std::filesystem::path& path, std::vector<CVarIssue>* issues = nullptr);
    /// Archive, non-cheat, non-read-only cvars that hold a config-file / runtime value, sorted by
    /// name, plus config-file values of names not registered in this run.
    [[nodiscard]] std::string save_config_text() const;
    /// Writes through a temporary file + rename; creates the parent directory.
    bool save_config_file(const std::filesystem::path& path) const;

    /// `-set key=value` (also `--set`) and `+key value`. Other arguments are ignored. Returns the
    /// number of values applied.
    usize apply_command_line(int argc, const char* const* argv, std::vector<CVarIssue>* issues = nullptr);

    /// Per-user config directory: %APPDATA%\<app> (Windows), ~/Library/Application Support/<app>
    /// (macOS), $XDG_CONFIG_HOME/<app> or ~/.config/<app> (other). Empty when no home is known.
    [[nodiscard]] static std::filesystem::path user_config_dir(std::string_view app_name);
    /// user_config_dir(app) / "settings.cfg".
    [[nodiscard]] static std::filesystem::path default_config_path(std::string_view app_name);

    /// Visits every cvar in name order.
    void for_each(const std::function<void(const CVarEntry&)>& fn) const;
    /// Names starting with `prefix` (console completion), sorted.
    [[nodiscard]] std::vector<std::string> complete(std::string_view prefix) const;

    /// Parse `text` as a value of `entry` without applying it; returns the canonical text.
    [[nodiscard]] CVarResult validate(const CVarEntry& entry, std::string_view text, std::string* canonical) const;

private:
    CVarEntry* register_entry(std::unique_ptr<CVarEntry> entry);
    CVarResult apply(CVarEntry& entry, std::string_view text, CVarSource source);
    CVarResult apply_word(CVarEntry& entry, u32 word, const std::string& canonical, CVarSource source);
    void notify(const CVarEntry& entry);

    struct Pending {
        std::optional<std::string> config;
        std::optional<std::string> command_line;
    };

    mutable std::shared_mutex m_mutex; ///< guards the maps (lookups may come from any thread)
    std::map<std::string, std::unique_ptr<CVarEntry>, std::less<>> m_entries;
    std::map<std::string, Pending, std::less<>> m_pending;
    std::vector<CVarEntry::Callback> m_listeners;
    u64 m_nextCallbackId = 1;
    bool m_cheats = false;
    bool m_startupFinished = false;
};

// ---- typed handles --------------------------------------------------------------------------

/// Typed handle to a registered cvar (T = bool, i32, f32 or std::string). Declare as a static to
/// register at start-up: `static CVar<f32> s_scale("r.renderScale", 1.f, 0.25f, 2.f, "...", ...)`.
template <typename T>
class CVar;

namespace detail {
template <typename Derived>
class CVarHandleBase {
public:
    [[nodiscard]] bool valid() const { return m_entry != nullptr; }
    [[nodiscard]] CVarEntry& entry() const { return *m_entry; }
    [[nodiscard]] CVarRegistry& registry() const { return *m_registry; }
    /// Set from text at runtime precedence.
    CVarResult set_text(std::string_view text, CVarSource source = CVarSource::Runtime) {
        return m_entry != nullptr ? m_registry->set(*m_entry, text, source) : CVarResult::UnknownName;
    }
    u64 on_change(CVarCallback callback) {
        return m_entry != nullptr ? m_registry->add_callback(*m_entry, std::move(callback)) : 0u;
    }

protected:
    CVarRegistry* m_registry = nullptr;
    CVarEntry* m_entry = nullptr;
};
} // namespace detail

template <>
class CVar<bool> : public detail::CVarHandleBase<CVar<bool>> {
public:
    CVar() = default;
    CVar(std::string_view name, bool default_value, std::string_view description,
         CVarFlags flags = CVarFlags::None, CVarRegistry& registry = CVarRegistry::global()) {
        m_registry = &registry;
        m_entry = registry.register_bool(name, default_value, description, flags);
    }
    [[nodiscard]] bool get() const { return m_entry != nullptr && m_entry->get_bool(); }
    CVarResult set(bool value, CVarSource source = CVarSource::Runtime) {
        return m_entry != nullptr ? m_registry->set_bool(*m_entry, value, source) : CVarResult::UnknownName;
    }
};

template <>
class CVar<i32> : public detail::CVarHandleBase<CVar<i32>> {
public:
    CVar() = default;
    CVar(std::string_view name, i32 default_value, i32 low, i32 high, std::string_view description,
         CVarFlags flags = CVarFlags::None, CVarRegistry& registry = CVarRegistry::global()) {
        m_registry = &registry;
        m_entry = registry.register_int(name, default_value, low, high, description, flags);
    }
    [[nodiscard]] i32 get() const { return m_entry != nullptr ? m_entry->get_int() : 0; }
    CVarResult set(i32 value, CVarSource source = CVarSource::Runtime) {
        return m_entry != nullptr ? m_registry->set_int(*m_entry, value, source) : CVarResult::UnknownName;
    }
};

template <>
class CVar<f32> : public detail::CVarHandleBase<CVar<f32>> {
public:
    CVar() = default;
    CVar(std::string_view name, f32 default_value, f32 low, f32 high, std::string_view description,
         CVarFlags flags = CVarFlags::None, CVarRegistry& registry = CVarRegistry::global()) {
        m_registry = &registry;
        m_entry = registry.register_float(name, default_value, low, high, description, flags);
    }
    [[nodiscard]] f32 get() const { return m_entry != nullptr ? m_entry->get_float() : 0.f; }
    CVarResult set(f32 value, CVarSource source = CVarSource::Runtime) {
        return m_entry != nullptr ? m_registry->set_float(*m_entry, value, source) : CVarResult::UnknownName;
    }
};

template <>
class CVar<std::string> : public detail::CVarHandleBase<CVar<std::string>> {
public:
    CVar() = default;
    CVar(std::string_view name, std::string_view default_value, std::string_view description,
         CVarFlags flags = CVarFlags::None, CVarRegistry& registry = CVarRegistry::global()) {
        m_registry = &registry;
        m_entry = registry.register_string(name, default_value, description, flags);
    }
    [[nodiscard]] std::string get() const { return m_entry != nullptr ? m_entry->get_string() : std::string(); }
    CVarResult set(std::string_view value, CVarSource source = CVarSource::Runtime) {
        return m_entry != nullptr ? m_registry->set_string(*m_entry, value, source) : CVarResult::UnknownName;
    }
};

/// Enum cvar: a fixed list of names; stored and read as the index.
class CVarEnum : public detail::CVarHandleBase<CVarEnum> {
public:
    CVarEnum() = default;
    CVarEnum(std::string_view name, std::initializer_list<std::string_view> values, i32 default_index,
             std::string_view description, CVarFlags flags = CVarFlags::None,
             CVarRegistry& registry = CVarRegistry::global()) {
        m_registry = &registry;
        m_entry = registry.register_enum(name, values, default_index, description, flags);
    }
    [[nodiscard]] i32 get() const { return m_entry != nullptr ? m_entry->get_enum() : 0; }
    [[nodiscard]] std::string get_name() const { return m_entry != nullptr ? m_entry->value_text() : std::string(); }
    CVarResult set(i32 index, CVarSource source = CVarSource::Runtime) {
        return m_entry != nullptr ? m_registry->set_enum(*m_entry, index, source) : CVarResult::UnknownName;
    }
};

} // namespace fuse::config
