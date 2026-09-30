#include <fuse/config/cvar.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fuse::config {

namespace {

char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    usize b = 0;
    usize e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

/// Names: [A-Za-z0-9_.-], starting with a letter or '_', no leading/trailing/double dots.
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    const char first = name.front();
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')) {
        return false;
    }
    char prev = 0;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-';
        if (!ok || (c == '.' && prev == '.')) {
            return false;
        }
        prev = c;
    }
    return name.back() != '.';
}

bool parse_bool(std::string_view t, bool& out) {
    if (t == "1" || iequals(t, "true") || iequals(t, "on") || iequals(t, "yes")) {
        out = true;
        return true;
    }
    if (t == "0" || iequals(t, "false") || iequals(t, "off") || iequals(t, "no")) {
        out = false;
        return true;
    }
    return false;
}

bool parse_i64(std::string_view t, std::int64_t& out) {
    if (!t.empty() && t.front() == '+') {
        t.remove_prefix(1);
    }
    if (t.empty()) {
        return false;
    }
    const auto res = std::from_chars(t.data(), t.data() + t.size(), out);
    return res.ec == std::errc() && res.ptr == t.data() + t.size();
}

bool parse_f64(std::string_view t, f64& out) {
    if (!t.empty() && t.front() == '+') {
        t.remove_prefix(1);
    }
    if (t.empty()) {
        return false;
    }
    const auto res = std::from_chars(t.data(), t.data() + t.size(), out);
    return res.ec == std::errc() && res.ptr == t.data() + t.size() && std::isfinite(out);
}

std::string format_float(f32 v) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

std::string format_int(i32 v) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

u32 word_of_int(i32 v) { return std::bit_cast<u32>(v); }
u32 word_of_float(f32 v) { return std::bit_cast<u32>(v); }

std::optional<std::string> env_value(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    usize len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    std::free(value);
    return out.empty() ? std::nullopt : std::optional<std::string>(out);
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

/// Quote a string value for the config file when it needs it (spaces, comment chars, quotes).
std::string quote_if_needed(std::string_view v) {
    bool needs = v.empty();
    for (const char c : v) {
        if (c == ' ' || c == '\t' || c == '#' || c == ';' || c == '"' || c == '\\' || c == '\n' || c == '/' ||
            c == '=') {
            needs = true;
            break;
        }
    }
    if (!needs) {
        return std::string(v);
    }
    std::string out = "\"";
    for (const char c : v) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    out += '"';
    return out;
}

/// Parses the value part of a config line: a quoted string (escapes) or bare text up to a
/// comment. Returns false on an unterminated quote / trailing garbage.
bool parse_config_value(std::string_view raw, std::string& out) {
    raw = trim(raw);
    out.clear();
    if (!raw.empty() && raw.front() == '"') {
        usize i = 1;
        for (; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == '\\' && i + 1 < raw.size()) {
                const char n = raw[++i];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
                continue;
            }
            if (c == '"') {
                break;
            }
            out += c;
        }
        if (i >= raw.size()) {
            return false;
        }
        const std::string_view rest = trim(raw.substr(i + 1));
        return rest.empty() || rest.front() == '#' || rest.front() == ';' || rest.substr(0, 2) == "//";
    }
    usize end = raw.size();
    for (usize i = 0; i < raw.size(); ++i) {
        if (raw[i] == '#' || raw[i] == ';' || (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/')) {
            end = i;
            break;
        }
    }
    out = std::string(trim(raw.substr(0, end)));
    return true;
}

} // namespace

const char* to_string(CVarResult result) {
    switch (result) {
    case CVarResult::Ok:
        return "ok";
    case CVarResult::Unchanged:
        return "unchanged";
    case CVarResult::Deferred:
        return "deferred until restart";
    case CVarResult::Overridden:
        return "overridden by a higher-precedence source";
    case CVarResult::UnknownName:
        return "unknown cvar";
    case CVarResult::ParseError:
        return "invalid value";
    case CVarResult::OutOfRange:
        return "value out of range";
    case CVarResult::ReadOnly:
        return "read-only";
    case CVarResult::CheatProtected:
        return "cheat cvar (cheats disabled)";
    case CVarResult::TypeMismatch:
        return "type mismatch";
    }
    return "?";
}

const char* to_string(CVarType type) {
    switch (type) {
    case CVarType::Bool:
        return "bool";
    case CVarType::Int:
        return "int";
    case CVarType::Float:
        return "float";
    case CVarType::String:
        return "string";
    case CVarType::Enum:
        return "enum";
    }
    return "?";
}

const char* to_string(CVarSource source) {
    switch (source) {
    case CVarSource::Default:
        return "default";
    case CVarSource::ConfigFile:
        return "config";
    case CVarSource::CommandLine:
        return "command line";
    case CVarSource::Runtime:
        return "runtime";
    }
    return "?";
}

// ---- CVarEntry --------------------------------------------------------------------------------

i32 CVarEntry::get_int() const {
    return std::bit_cast<i32>(m_word.load(std::memory_order_acquire));
}

f32 CVarEntry::get_float() const {
    return std::bit_cast<f32>(m_word.load(std::memory_order_acquire));
}

std::string CVarEntry::get_string() const {
    std::shared_ptr<const std::string> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_stringMutex);
        snapshot = m_string;
    }
    return snapshot ? *snapshot : std::string();
}

std::string CVarEntry::value_text() const {
    switch (m_type) {
    case CVarType::Bool:
        return get_bool() ? "true" : "false";
    case CVarType::Int:
        return format_int(get_int());
    case CVarType::Float:
        return format_float(get_float());
    case CVarType::String:
        return get_string();
    case CVarType::Enum: {
        const i32 index = get_enum();
        return index >= 0 && static_cast<usize>(index) < m_enumValues.size() ? m_enumValues[static_cast<usize>(index)]
                                                                              : format_int(index);
    }
    }
    return {};
}

// ---- CVarRegistry -----------------------------------------------------------------------------

CVarRegistry::CVarRegistry() = default;
CVarRegistry::~CVarRegistry() = default;

CVarRegistry& CVarRegistry::global() {
    static CVarRegistry s_registry;
    return s_registry;
}

CVarEntry* CVarRegistry::register_entry(std::unique_ptr<CVarEntry> entry) {
    CVarEntry* raw = nullptr;
    std::optional<Pending> pending;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        const auto it = m_entries.find(entry->m_name);
        if (it != m_entries.end()) {
            return it->second->m_type == entry->m_type ? it->second.get() : nullptr;
        }
        raw = entry.get();
        m_entries.emplace(entry->m_name, std::move(entry));
        const auto p = m_pending.find(raw->m_name);
        if (p != m_pending.end()) {
            pending = std::move(p->second);
            m_pending.erase(p);
        }
    }
    // Values that arrived before registration, in precedence order. Registration is not a
    // change: listeners hear about these like any other set.
    if (pending) {
        if (pending->config) {
            (void)apply(*raw, *pending->config, CVarSource::ConfigFile);
        }
        if (pending->command_line) {
            (void)apply(*raw, *pending->command_line, CVarSource::CommandLine);
        }
    }
    return raw;
}

CVarEntry* CVarRegistry::register_bool(std::string_view name, bool default_value, std::string_view description,
                                       CVarFlags flags) {
    if (!valid_name(name)) {
        return nullptr;
    }
    std::unique_ptr<CVarEntry> e(new CVarEntry());
    e->m_name = std::string(name);
    e->m_description = std::string(description);
    e->m_flags = flags;
    e->m_type = CVarType::Bool;
    e->m_defaultText = default_value ? "true" : "false";
    e->m_word.store(default_value ? 1u : 0u, std::memory_order_relaxed);
    return register_entry(std::move(e));
}

CVarEntry* CVarRegistry::register_int(std::string_view name, i32 default_value, i32 low, i32 high,
                                      std::string_view description, CVarFlags flags) {
    if (!valid_name(name) || low > high || default_value < low || default_value > high) {
        return nullptr;
    }
    std::unique_ptr<CVarEntry> e(new CVarEntry());
    e->m_name = std::string(name);
    e->m_description = std::string(description);
    e->m_flags = flags;
    e->m_type = CVarType::Int;
    e->m_hasRange = true;
    e->m_rangeLow = low;
    e->m_rangeHigh = high;
    e->m_defaultText = format_int(default_value);
    e->m_word.store(word_of_int(default_value), std::memory_order_relaxed);
    return register_entry(std::move(e));
}

CVarEntry* CVarRegistry::register_float(std::string_view name, f32 default_value, f32 low, f32 high,
                                        std::string_view description, CVarFlags flags) {
    if (!valid_name(name) || !std::isfinite(default_value) || !std::isfinite(low) || !std::isfinite(high) ||
        low > high || default_value < low || default_value > high) {
        return nullptr;
    }
    std::unique_ptr<CVarEntry> e(new CVarEntry());
    e->m_name = std::string(name);
    e->m_description = std::string(description);
    e->m_flags = flags;
    e->m_type = CVarType::Float;
    e->m_hasRange = true;
    e->m_rangeLow = low;
    e->m_rangeHigh = high;
    e->m_defaultText = format_float(default_value);
    e->m_word.store(word_of_float(default_value), std::memory_order_relaxed);
    return register_entry(std::move(e));
}

CVarEntry* CVarRegistry::register_string(std::string_view name, std::string_view default_value,
                                         std::string_view description, CVarFlags flags) {
    if (!valid_name(name)) {
        return nullptr;
    }
    std::unique_ptr<CVarEntry> e(new CVarEntry());
    e->m_name = std::string(name);
    e->m_description = std::string(description);
    e->m_flags = flags;
    e->m_type = CVarType::String;
    e->m_defaultText = std::string(default_value);
    e->m_string = std::make_shared<const std::string>(default_value);
    return register_entry(std::move(e));
}

CVarEntry* CVarRegistry::register_enum(std::string_view name, std::initializer_list<std::string_view> values,
                                       i32 default_index, std::string_view description, CVarFlags flags) {
    std::vector<std::string> copy;
    copy.reserve(values.size());
    for (const std::string_view v : values) {
        copy.emplace_back(v);
    }
    return register_enum(name, copy, default_index, description, flags);
}

CVarEntry* CVarRegistry::register_enum(std::string_view name, const std::vector<std::string>& values,
                                       i32 default_index, std::string_view description, CVarFlags flags) {
    if (!valid_name(name) || values.empty() || default_index < 0 ||
        static_cast<usize>(default_index) >= values.size()) {
        return nullptr;
    }
    for (usize i = 0; i < values.size(); ++i) {
        if (values[i].empty()) {
            return nullptr;
        }
        for (usize j = 0; j < i; ++j) {
            if (iequals(values[i], values[j])) {
                return nullptr;
            }
        }
    }
    std::unique_ptr<CVarEntry> e(new CVarEntry());
    e->m_name = std::string(name);
    e->m_description = std::string(description);
    e->m_flags = flags;
    e->m_type = CVarType::Enum;
    e->m_enumValues = values;
    e->m_hasRange = true;
    e->m_rangeLow = 0.0;
    e->m_rangeHigh = static_cast<f64>(values.size() - 1);
    e->m_defaultText = values[static_cast<usize>(default_index)];
    e->m_word.store(word_of_int(default_index), std::memory_order_relaxed);
    return register_entry(std::move(e));
}

CVarEntry* CVarRegistry::find(std::string_view name) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto it = m_entries.find(name);
    return it != m_entries.end() ? it->second.get() : nullptr;
}

const CVarEntry* CVarRegistry::find(std::string_view name) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto it = m_entries.find(name);
    return it != m_entries.end() ? it->second.get() : nullptr;
}

usize CVarRegistry::count() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_entries.size();
}

CVarResult CVarRegistry::validate(const CVarEntry& entry, std::string_view text, std::string* canonical) const {
    text = trim(text);
    std::string out;
    switch (entry.m_type) {
    case CVarType::Bool: {
        bool b = false;
        if (!parse_bool(text, b)) {
            return CVarResult::ParseError;
        }
        out = b ? "true" : "false";
        break;
    }
    case CVarType::Int: {
        std::int64_t v = 0;
        if (!parse_i64(text, v)) {
            return CVarResult::ParseError;
        }
        if (static_cast<f64>(v) < entry.m_rangeLow || static_cast<f64>(v) > entry.m_rangeHigh) {
            return CVarResult::OutOfRange;
        }
        out = format_int(static_cast<i32>(v));
        break;
    }
    case CVarType::Float: {
        f64 v = 0.0;
        if (!parse_f64(text, v)) {
            return CVarResult::ParseError;
        }
        const f32 f = static_cast<f32>(v);
        if (!std::isfinite(f) || static_cast<f64>(f) < entry.m_rangeLow || static_cast<f64>(f) > entry.m_rangeHigh) {
            return CVarResult::OutOfRange;
        }
        out = format_float(f);
        break;
    }
    case CVarType::String:
        out = std::string(text);
        break;
    case CVarType::Enum: {
        bool found = false;
        for (const std::string& v : entry.m_enumValues) {
            if (iequals(v, text)) {
                out = v;
                found = true;
                break;
            }
        }
        if (!found) {
            std::int64_t index = 0;
            if (!parse_i64(text, index)) {
                return CVarResult::ParseError;
            }
            if (index < 0 || static_cast<u64>(index) >= entry.m_enumValues.size()) {
                return CVarResult::OutOfRange;
            }
            out = entry.m_enumValues[static_cast<usize>(index)];
        }
        break;
    }
    }
    if (canonical != nullptr) {
        *canonical = std::move(out);
    }
    return CVarResult::Ok;
}

CVarResult CVarRegistry::apply(CVarEntry& entry, std::string_view text, CVarSource source) {
    // Permission checks first: a protected cvar reports its protection whatever the text.
    if (entry.has(CVarFlags::Cheat) && !m_cheats && source != CVarSource::Default) {
        return CVarResult::CheatProtected;
    }
    if (entry.has(CVarFlags::ReadOnly) && (source == CVarSource::ConfigFile || source == CVarSource::Runtime)) {
        return CVarResult::ReadOnly;
    }
    std::string canonical;
    const CVarResult parsed = validate(entry, text, &canonical);
    if (parsed != CVarResult::Ok) {
        return parsed;
    }

    // The value the user's config file should keep: config-file loads and runtime sets, even
    // when a command-line override hides it for this run.
    if (source == CVarSource::ConfigFile || source == CVarSource::Runtime) {
        if (source == CVarSource::Runtime && canonical == entry.m_defaultText) {
            entry.m_archived.reset();
        } else {
            entry.m_archived = canonical;
        }
    }

    if (source < entry.source()) {
        return CVarResult::Overridden;
    }

    if (entry.has(CVarFlags::RequiresRestart) && m_startupFinished) {
        if (canonical == entry.value_text()) {
            entry.m_pendingText.clear();
            entry.m_restartPending.store(false, std::memory_order_release);
            return CVarResult::Unchanged;
        }
        entry.m_pendingText = canonical;
        entry.m_restartPending.store(true, std::memory_order_release);
        return CVarResult::Deferred;
    }

    u32 word = 0;
    switch (entry.m_type) {
    case CVarType::Bool:
        word = canonical == "true" ? 1u : 0u;
        break;
    case CVarType::Int: {
        std::int64_t v = 0;
        (void)parse_i64(canonical, v);
        word = word_of_int(static_cast<i32>(v));
        break;
    }
    case CVarType::Float: {
        f64 v = 0.0;
        (void)parse_f64(canonical, v);
        word = word_of_float(static_cast<f32>(v));
        break;
    }
    case CVarType::String:
        break;
    case CVarType::Enum: {
        for (usize i = 0; i < entry.m_enumValues.size(); ++i) {
            if (entry.m_enumValues[i] == canonical) {
                word = word_of_int(static_cast<i32>(i));
            }
        }
        break;
    }
    }
    return apply_word(entry, word, canonical, source);
}

CVarResult CVarRegistry::apply_word(CVarEntry& entry, u32 word, const std::string& canonical, CVarSource source) {
    entry.m_source.store(static_cast<u8>(source), std::memory_order_release);
    bool changed = false;
    if (entry.m_type == CVarType::String) {
        std::lock_guard<std::mutex> lock(entry.m_stringMutex);
        if (!entry.m_string || *entry.m_string != canonical) {
            entry.m_string = std::make_shared<const std::string>(canonical);
            changed = true;
        }
    } else {
        changed = entry.m_word.exchange(word, std::memory_order_acq_rel) != word;
    }
    if (!changed) {
        return CVarResult::Unchanged;
    }
    notify(entry);
    return CVarResult::Ok;
}

void CVarRegistry::notify(const CVarEntry& entry) {
    // Copies: a callback may register / remove callbacks or set other cvars.
    const std::vector<CVarEntry::Callback> own = entry.m_callbacks;
    for (const CVarEntry::Callback& cb : own) {
        cb.fn(entry);
    }
    const std::vector<CVarEntry::Callback> listeners = m_listeners;
    for (const CVarEntry::Callback& cb : listeners) {
        cb.fn(entry);
    }
}

CVarResult CVarRegistry::set(std::string_view name, std::string_view text, CVarSource source) {
    CVarEntry* entry = find(name);
    if (entry == nullptr) {
        return CVarResult::UnknownName;
    }
    return set(*entry, text, source);
}

CVarResult CVarRegistry::set(CVarEntry& entry, std::string_view text, CVarSource source) {
    return apply(entry, text, source);
}

CVarResult CVarRegistry::set_bool(CVarEntry& entry, bool value, CVarSource source) {
    if (entry.type() != CVarType::Bool) {
        return CVarResult::TypeMismatch;
    }
    return apply(entry, value ? "true" : "false", source);
}

CVarResult CVarRegistry::set_int(CVarEntry& entry, i32 value, CVarSource source) {
    if (entry.type() != CVarType::Int) {
        return CVarResult::TypeMismatch;
    }
    return apply(entry, format_int(value), source);
}

CVarResult CVarRegistry::set_float(CVarEntry& entry, f32 value, CVarSource source) {
    if (entry.type() != CVarType::Float) {
        return CVarResult::TypeMismatch;
    }
    if (!std::isfinite(value)) {
        return CVarResult::ParseError;
    }
    return apply(entry, format_float(value), source);
}

CVarResult CVarRegistry::set_string(CVarEntry& entry, std::string_view value, CVarSource source) {
    if (entry.type() != CVarType::String) {
        return CVarResult::TypeMismatch;
    }
    // Strings are not trimmed by validate() on this path: keep the caller's exact text.
    if (entry.has(CVarFlags::Cheat) && !m_cheats) {
        return CVarResult::CheatProtected;
    }
    if (entry.has(CVarFlags::ReadOnly) && (source == CVarSource::ConfigFile || source == CVarSource::Runtime)) {
        return CVarResult::ReadOnly;
    }
    const std::string text(value);
    if (source == CVarSource::ConfigFile || source == CVarSource::Runtime) {
        if (source == CVarSource::Runtime && text == entry.m_defaultText) {
            entry.m_archived.reset();
        } else {
            entry.m_archived = text;
        }
    }
    if (source < entry.source()) {
        return CVarResult::Overridden;
    }
    if (entry.has(CVarFlags::RequiresRestart) && m_startupFinished) {
        if (text == entry.get_string()) {
            entry.m_pendingText.clear();
            entry.m_restartPending.store(false, std::memory_order_release);
            return CVarResult::Unchanged;
        }
        entry.m_pendingText = text;
        entry.m_restartPending.store(true, std::memory_order_release);
        return CVarResult::Deferred;
    }
    return apply_word(entry, 0u, text, source);
}

CVarResult CVarRegistry::set_enum(CVarEntry& entry, i32 index, CVarSource source) {
    if (entry.type() != CVarType::Enum) {
        return CVarResult::TypeMismatch;
    }
    return apply(entry, format_int(index), source);
}

CVarResult CVarRegistry::reset(std::string_view name) {
    CVarEntry* entry = find(name);
    if (entry == nullptr) {
        return CVarResult::UnknownName;
    }
    entry->m_archived.reset();
    entry->m_pendingText.clear();
    entry->m_restartPending.store(false, std::memory_order_release);
    u32 word = 0;
    switch (entry->m_type) {
    case CVarType::Bool:
        word = entry->m_defaultText == "true" ? 1u : 0u;
        break;
    case CVarType::Int: {
        std::int64_t v = 0;
        (void)parse_i64(entry->m_defaultText, v);
        word = word_of_int(static_cast<i32>(v));
        break;
    }
    case CVarType::Float: {
        f64 v = 0.0;
        (void)parse_f64(entry->m_defaultText, v);
        word = word_of_float(static_cast<f32>(v));
        break;
    }
    case CVarType::String:
        break;
    case CVarType::Enum:
        for (usize i = 0; i < entry->m_enumValues.size(); ++i) {
            if (entry->m_enumValues[i] == entry->m_defaultText) {
                word = word_of_int(static_cast<i32>(i));
            }
        }
        break;
    }
    return apply_word(*entry, word, entry->m_defaultText, CVarSource::Default);
}

u64 CVarRegistry::add_callback(CVarEntry& entry, CVarCallback callback) {
    if (!callback || find(entry.name()) != &entry) {
        return 0;
    }
    const u64 id = m_nextCallbackId++;
    entry.m_callbacks.push_back({id, std::move(callback)});
    return id;
}

u64 CVarRegistry::add_listener(CVarCallback callback) {
    if (!callback) {
        return 0;
    }
    const u64 id = m_nextCallbackId++;
    m_listeners.push_back({id, std::move(callback)});
    return id;
}

bool CVarRegistry::remove_callback(u64 id) {
    if (id == 0) {
        return false;
    }
    const auto erase_from = [id](std::vector<CVarEntry::Callback>& list) {
        const auto it = std::find_if(list.begin(), list.end(), [id](const CVarEntry::Callback& c) { return c.id == id; });
        if (it == list.end()) {
            return false;
        }
        list.erase(it);
        return true;
    };
    if (erase_from(m_listeners)) {
        return true;
    }
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto& [name, entry] : m_entries) {
        (void)name;
        if (erase_from(entry->m_callbacks)) {
            return true;
        }
    }
    return false;
}

// ---- persistence ------------------------------------------------------------------------------

usize CVarRegistry::load_config_text(std::string_view text, std::vector<CVarIssue>* issues) {
    usize applied = 0;
    u32 line_no = 0;
    std::string section;
    const auto report = [&](std::string name, CVarResult result, std::string message) {
        if (issues != nullptr) {
            issues->push_back({line_no, std::move(name), result, std::move(message)});
        }
    };
    while (!text.empty()) {
        ++line_no;
        const usize nl = text.find('\n');
        std::string_view line = nl == std::string_view::npos ? text : text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        line = trim(line);
        if (line_no == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
            line = trim(line.substr(3)); // UTF-8 BOM
        }
        if (line.empty() || line.front() == '#' || line.front() == ';' || line.substr(0, 2) == "//") {
            continue;
        }
        if (line.front() == '[') {
            const usize close = line.find(']');
            if (close == std::string_view::npos) {
                report({}, CVarResult::ParseError, "unterminated [section]");
                continue;
            }
            section = std::string(trim(line.substr(1, close - 1)));
            continue;
        }
        const usize eq = line.find('=');
        if (eq == std::string_view::npos) {
            report(std::string(line), CVarResult::ParseError, "expected 'name = value'");
            continue;
        }
        std::string name = std::string(trim(line.substr(0, eq)));
        if (!section.empty()) {
            name = section + "." + name;
        }
        std::string value;
        if (!parse_config_value(line.substr(eq + 1), value)) {
            report(name, CVarResult::ParseError, "unterminated quoted value");
            continue;
        }
        if (!valid_name(name)) {
            report(name, CVarResult::ParseError, "invalid cvar name");
            continue;
        }
        CVarEntry* entry = find(name);
        if (entry == nullptr) {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_pending[name].config = value;
            continue;
        }
        const CVarResult r = apply(*entry, value, CVarSource::ConfigFile);
        if (succeeded(r)) {
            ++applied;
        } else if (r != CVarResult::Overridden) {
            report(name, r, std::string(to_string(r)) + ": '" + value + "'");
        }
    }
    return applied;
}

bool CVarRegistry::load_config_file(const std::filesystem::path& path, std::vector<CVarIssue>* issues) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    (void)load_config_text(ss.str(), issues);
    return true;
}

std::string CVarRegistry::save_config_text() const {
    std::map<std::string, std::string> lines;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        for (const auto& [name, entry] : m_entries) {
            if (!entry->has(CVarFlags::Archive) || entry->has(CVarFlags::Cheat) || entry->has(CVarFlags::ReadOnly)) {
                continue;
            }
            if (!entry->m_archived) {
                continue;
            }
            lines[name] = *entry->m_archived;
        }
        for (const auto& [name, pending] : m_pending) {
            if (pending.config) {
                lines.emplace(name, *pending.config);
            }
        }
    }
    std::string out = "# FUSE user settings (fuse::config::CVarRegistry). Only archived cvars are saved.\n";
    for (const auto& [name, value] : lines) {
        out += name;
        out += " = ";
        out += quote_if_needed(value);
        out += '\n';
    }
    return out;
}

bool CVarRegistry::save_config_file(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        const std::string text = save_config_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows cannot rename over an existing file on every filesystem: replace explicitly.
        std::error_code rm;
        std::filesystem::remove(path, rm);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

usize CVarRegistry::apply_command_line(int argc, const char* const* argv, std::vector<CVarIssue>* issues) {
    usize applied = 0;
    const auto handle = [&](u32 index, std::string_view name, std::string_view value) {
        name = trim(name);
        if (!valid_name(name)) {
            if (issues != nullptr) {
                issues->push_back({index, std::string(name), CVarResult::ParseError, "invalid cvar name"});
            }
            return;
        }
        CVarEntry* entry = find(name);
        if (entry == nullptr) {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_pending[std::string(name)].command_line = std::string(value);
            return;
        }
        const CVarResult r = apply(*entry, value, CVarSource::CommandLine);
        if (succeeded(r)) {
            ++applied;
        } else if (issues != nullptr) {
            issues->push_back({index, std::string(name), r, std::string(to_string(r)) + ": '" + std::string(value) + "'"});
        }
    };
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string_view arg = argv[i];
        if (arg == "-set" || arg == "--set") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                if (issues != nullptr) {
                    issues->push_back({static_cast<u32>(i), {}, CVarResult::ParseError, "-set needs key=value"});
                }
                continue;
            }
            const std::string_view kv = argv[++i];
            const usize eq = kv.find('=');
            if (eq == std::string_view::npos) {
                if (issues != nullptr) {
                    issues->push_back({static_cast<u32>(i), std::string(kv), CVarResult::ParseError, "-set needs key=value"});
                }
                continue;
            }
            handle(static_cast<u32>(i), kv.substr(0, eq), kv.substr(eq + 1));
        } else if (arg.size() > 1 && arg.front() == '+') {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                if (issues != nullptr) {
                    issues->push_back({static_cast<u32>(i), std::string(arg.substr(1)), CVarResult::ParseError,
                                       "+name needs a value"});
                }
                continue;
            }
            const std::string_view value = argv[i + 1];
            handle(static_cast<u32>(i), arg.substr(1), value);
            ++i;
        }
    }
    return applied;
}

std::filesystem::path CVarRegistry::user_config_dir(std::string_view app_name) {
    std::filesystem::path base;
#if defined(_WIN32)
    if (auto appdata = env_value("APPDATA")) {
        base = *appdata;
    } else if (auto profile = env_value("USERPROFILE")) {
        base = std::filesystem::path(*profile) / "AppData" / "Roaming";
    }
#elif defined(__APPLE__)
    if (auto home = env_value("HOME")) {
        base = std::filesystem::path(*home) / "Library" / "Application Support";
    }
#else
    if (auto xdg = env_value("XDG_CONFIG_HOME")) {
        base = *xdg;
    } else if (auto home = env_value("HOME")) {
        base = std::filesystem::path(*home) / ".config";
    }
#endif
    if (base.empty()) {
        return {};
    }
    return base / std::filesystem::path(std::string(app_name));
}

std::filesystem::path CVarRegistry::default_config_path(std::string_view app_name) {
    const std::filesystem::path dir = user_config_dir(app_name);
    return dir.empty() ? std::filesystem::path() : dir / "settings.cfg";
}

void CVarRegistry::for_each(const std::function<void(const CVarEntry&)>& fn) const {
    std::vector<const CVarEntry*> entries;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        entries.reserve(m_entries.size());
        for (const auto& [name, entry] : m_entries) {
            (void)name;
            entries.push_back(entry.get());
        }
    }
    for (const CVarEntry* e : entries) {
        fn(*e);
    }
}

std::vector<std::string> CVarRegistry::complete(std::string_view prefix) const {
    std::vector<std::string> out;
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto it = m_entries.lower_bound(prefix); it != m_entries.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) {
            break;
        }
        out.push_back(it->first);
    }
    return out;
}

} // namespace fuse::config
