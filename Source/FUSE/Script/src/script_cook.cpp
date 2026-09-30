#include <fuse/script/script_cook.hpp>

#include "script_lua_compat.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fuse::script {

namespace {

constexpr usize kHeaderBytes = 4u + 4u + 4u + 4u + 4u + 8u;

u64 fnv1a64(const u8* data, usize size) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool ends_with_icase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(tail[i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

template <typename T>
void put(std::vector<u8>& out, T value) {
    const auto* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
T get(const u8* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

ScriptCookResult failed(ScriptCookFailure failure, std::string message) {
    ScriptCookResult result;
    result.ok = false;
    result.failure = failure;
    result.message = std::move(message);
    return result;
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
int dump_writer(lua_State* /*L*/, const void* p, size_t size, void* user) {
    auto* out = static_cast<std::vector<u8>*>(user);
    const auto* bytes = static_cast<const u8*>(p);
    out->insert(out->end(), bytes, bytes + size);
    return 0;
}
#endif

} // namespace

bool script_compiler_available() {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    return true;
#else
    return false;
#endif
}

ScriptCookResult compile_lua_source(std::string_view source, const char* chunk_name, std::vector<u8>& bytecode) {
    bytecode.clear();
    if (chunk_name == nullptr || chunk_name[0] == '\0') {
        return failed(ScriptCookFailure::InvalidArgument, "chunk name is empty");
    }
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    // A bare state (no libraries): compiling never runs the chunk.
    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        return failed(ScriptCookFailure::CompilerUnavailable, "cannot create a Lua state");
    }
    const int status = luaL_loadbuffer(L, source.data(), source.size(), chunk_name);
    if (status != LUA_OK) {
        const char* reason = lua_tostring(L, -1);
        ScriptCookResult result =
            failed(status == LUA_ERRSYNTAX ? ScriptCookFailure::SyntaxError : ScriptCookFailure::InvalidArgument,
                   std::string("Lua syntax error: ") + (reason != nullptr ? reason : "unknown parse error"));
        lua_close(L);
        return result;
    }
#if LUA_VERSION_NUM >= 503
    const int dumped = lua_dump(L, dump_writer, &bytecode, 0);
#else
    const int dumped = lua_dump(L, dump_writer, &bytecode);
#endif
    lua_close(L);
    if (dumped != 0 || bytecode.empty()) {
        bytecode.clear();
        return failed(ScriptCookFailure::WriteFailed, "lua_dump failed");
    }
    ScriptCookResult result;
    result.ok = true;
    result.failure = ScriptCookFailure::None;
    result.kind = CookedScriptKind::LuaBytecode;
    result.byte_count = static_cast<u32>(bytecode.size());
    result.chunk_name = chunk_name;
    return result;
#else
    (void)source;
    return failed(ScriptCookFailure::CompilerUnavailable,
                  "Lua compiler not linked into this build (FUSE_SCRIPT_ENABLE_LUA=OFF)");
#endif
}

bool is_legacy_script_path(std::string_view path) {
    return ends_with_icase(path, ".cs") || ends_with_icase(path, ".tscript");
}

bool is_cooked_script_path(std::string_view path) { return ends_with_icase(path, kCookedScriptExtension); }

std::vector<u8> encode_cooked_script(const CookedScript& script) {
    std::vector<u8> out;
    out.reserve(kHeaderBytes + script.chunk_name.size() + script.payload.size());
    put(out, kCookedScriptMagic);
    put(out, kCookedScriptVersion);
    put(out, static_cast<u8>(script.kind));
    put(out, static_cast<u8>(0));
    put(out, static_cast<u8>(0));
    put(out, static_cast<u8>(0));
    put(out, static_cast<u32>(script.chunk_name.size()));
    put(out, static_cast<u32>(script.payload.size()));
    put(out, fnv1a64(script.payload.data(), script.payload.size()));
    out.insert(out.end(), script.chunk_name.begin(), script.chunk_name.end());
    out.insert(out.end(), script.payload.begin(), script.payload.end());
    return out;
}

bool decode_cooked_script(const u8* data, usize size, CookedScript& out, std::string* error) {
    const auto reject = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (data == nullptr || size < kHeaderBytes) {
        return reject("truncated .fusescript header");
    }
    if (get<u32>(data) != kCookedScriptMagic) {
        return reject("not a .fusescript file (bad magic)");
    }
    if (get<u32>(data + 4) != kCookedScriptVersion) {
        return reject("unsupported .fusescript version");
    }
    const u8 kind = data[8];
    if (kind != static_cast<u8>(CookedScriptKind::LuaBytecode) &&
        kind != static_cast<u8>(CookedScriptKind::LegacyTorqueScript)) {
        return reject("unknown .fusescript payload kind");
    }
    const u32 name_bytes = get<u32>(data + 12);
    const u32 payload_bytes = get<u32>(data + 16);
    const u64 payload_hash = get<u64>(data + 20);
    if (static_cast<u64>(kHeaderBytes) + name_bytes + payload_bytes != size) {
        return reject("truncated or oversized .fusescript body");
    }
    const u8* name = data + kHeaderBytes;
    const u8* payload = name + name_bytes;
    if (fnv1a64(payload, payload_bytes) != payload_hash) {
        return reject(".fusescript payload hash mismatch (corrupt file)");
    }
    out.kind = static_cast<CookedScriptKind>(kind);
    out.chunk_name.assign(reinterpret_cast<const char*>(name), name_bytes);
    out.payload.assign(payload, payload + payload_bytes);
    return true;
}

bool load_cooked_script(const std::string& path, CookedScript& out, std::string* error) {
    std::string bytes;
    if (!read_file(path, bytes)) {
        if (error != nullptr) {
            *error = "cannot read " + path;
        }
        return false;
    }
    return decode_cooked_script(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), out, error);
}

ScriptCookResult cook_script_file(const std::string& input_path, const std::string& output_path) {
    if (input_path.empty() || output_path.empty()) {
        return failed(ScriptCookFailure::InvalidArgument, "missing input or output path");
    }
    std::string source;
    if (!read_file(input_path, source)) {
        return failed(ScriptCookFailure::SourceUnreadable, "cannot read script source " + input_path);
    }

    CookedScript cooked;
    if (is_legacy_script_path(input_path)) {
        // Legacy TorqueScript: no FUSE-side compile; the Compat VM parses it when the chunk runs.
        cooked.kind = CookedScriptKind::LegacyTorqueScript;
        cooked.chunk_name = "t3d:" + std::filesystem::path(input_path).stem().string();
        cooked.payload.assign(source.begin(), source.end());
    } else {
        cooked.kind = CookedScriptKind::LuaBytecode;
        cooked.chunk_name = "@" + input_path; // Lua reports errors as "<input_path>:<line>: ..."
        const ScriptCookResult compiled = compile_lua_source(source, cooked.chunk_name.c_str(), cooked.payload);
        if (!compiled.ok) {
            return compiled;
        }
    }

    const std::vector<u8> encoded = encode_cooked_script(cooked);
    std::error_code ec;
    const std::filesystem::path out_path(output_path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return failed(ScriptCookFailure::WriteFailed, "cannot open for write: " + output_path);
    }
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    out.close();
    if (!out) {
        std::filesystem::remove(out_path, ec);
        return failed(ScriptCookFailure::WriteFailed, "write failed: " + output_path);
    }

    ScriptCookResult result;
    result.ok = true;
    result.failure = ScriptCookFailure::None;
    result.kind = cooked.kind;
    result.byte_count = static_cast<u32>(encoded.size());
    result.chunk_name = cooked.chunk_name;
    result.message = cooked.kind == CookedScriptKind::LuaBytecode ? "lua bytecode" : "legacy t3d passthrough";
    return result;
}

} // namespace fuse::script
