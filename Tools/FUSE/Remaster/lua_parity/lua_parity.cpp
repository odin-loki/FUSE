// fuse_remaster_lua_parity (FUSE Remaster plan §4.5 "Parity harness", W4.3): runs a ported Lua behaviour in the
// engine's Lua (vendor/lua 5.2.3, the fuse_script bundled backend) under the same sandbox as ScriptVM
// (Source/FUSE/Script/src/script_vm.cpp apply_sandbox: no io / package / debug / load*, os reduced to the time
// helpers, no string.dump), replays a recorded event trace against it and prints the gameplay event log.
//
//   fuse_remaster_lua_parity <port.lua> <trace.json> [--expect <expected.log>]
//
// The behaviour sees the ScriptRuntime calling convention (global on_* functions taking `self` first) and a
// small stand-in for the engine bindings: Entity.alive / get_position / set_position, Physics.get_velocity, plus
// the port bridge Remaster.get(e, field) / Remaster.set(e, field, v) / Remaster.emit(name, fields). Entities are
// integer ids; `self` is a table holding the behaviour properties and `__entity`.
//
// Trace (fuse.remaster-parity-trace/1):
//   {"behaviour": {"entity": 1, "props": {...}},
//    "entities": {"1": {"position": [x,y,z], "velocity": [x,y,z], "alive": true, "fields": {...}}, ...},
//    "steps": [{"set": {"entity": 2, "alive": false, "fields": {...}, ...}},
//              {"call": "on_collision", "args": [{"entity": 2}, {"vec3": [0, 0, 0]}, 5.0, "text"]}]}
// Log: one line per Remaster.emit: "<step> <name> <key>=<value> ..." (keys sorted; numbers %.6f; entities #id;
// vectors (x,y,z)); a Lua error becomes "<step> error <message>" and exit code 3. With --expect the log must
// equal the file (exit 1 and a diff otherwise). Exit 2 on usage, file or trace errors.
#include "fuse/remaster/json.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace json = fuse::remaster::json;

namespace {

struct Ent {
    double pos[3] = {0, 0, 0};
    double vel[3] = {0, 0, 0};
    bool alive = true;
    std::map<std::string, json::Value> fields;
};

std::map<long long, Ent> g_ents;
std::vector<std::string> g_log;
int g_step = 0;

bool readFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v == 0.0 ? 0.0 : v); // no "-0.000000"
    std::string s = buf;
    if (s == "-0.000000") s = "0.000000";
    return s;
}

void readVec(const json::Value* v, double out[3]) {
    if (v && v->isArray() && v->size() == 3)
        for (int i = 0; i < 3; ++i) out[i] = v->asArray()[i].asDouble();
}

void applyEntity(Ent& e, const json::Value& d) {
    readVec(d.get("position"), e.pos);
    readVec(d.get("velocity"), e.vel);
    if (const auto* a = d.get("alive")) e.alive = a->asBool(true);
    if (const auto* f = d.get("fields"))
        for (const auto& [k, v] : f->asObject()) e.fields[k] = v;
}

long long entityArg(lua_State* L, int idx) {
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "__entity");
        const long long id = static_cast<long long>(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        return id;
    }
    return static_cast<long long>(luaL_checkinteger(L, idx));
}

Ent& entity(lua_State* L, int idx) {
    const long long id = entityArg(L, idx);
    auto it = g_ents.find(id);
    if (it == g_ents.end()) luaL_error(L, "unknown entity %d", static_cast<int>(id));
    return it->second;
}

void pushVec(lua_State* L, const double v[3]) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
}

void pushJson(lua_State* L, const json::Value& v) {
    switch (v.type()) {
    case json::Value::Type::Bool: lua_pushboolean(L, v.asBool()); break;
    case json::Value::Type::Int:
    case json::Value::Type::Double: lua_pushnumber(L, v.asDouble()); break;
    case json::Value::Type::String: lua_pushstring(L, v.asString().c_str()); break;
    default: lua_pushnil(L); break;
    }
}

int l_alive(lua_State* L) { lua_pushboolean(L, entity(L, 1).alive); return 1; }
int l_get_position(lua_State* L) { pushVec(L, entity(L, 1).pos); return 1; }
int l_get_velocity(lua_State* L) { pushVec(L, entity(L, 1).vel); return 1; }
int l_set_position(lua_State* L) {
    Ent& e = entity(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const char* keys[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, 2, keys[i]);
        e.pos[i] = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    return 0;
}
int l_get(lua_State* L) {
    Ent& e = entity(L, 1);
    const auto it = e.fields.find(luaL_checkstring(L, 2));
    if (it == e.fields.end()) lua_pushnil(L);
    else pushJson(L, it->second);
    return 1;
}
int l_set(lua_State* L) {
    Ent& e = entity(L, 1);
    const std::string key = luaL_checkstring(L, 2);
    if (lua_isboolean(L, 3)) e.fields[key] = json::Value(static_cast<bool>(lua_toboolean(L, 3)));
    else if (lua_type(L, 3) == LUA_TNUMBER) e.fields[key] = json::Value(lua_tonumber(L, 3));
    else e.fields[key] = json::Value(std::string(luaL_checkstring(L, 3)));
    return 0;
}

std::string luaValueText(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER: return num(lua_tonumber(L, idx));
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "true" : "false";
    case LUA_TSTRING: return lua_tostring(L, idx);
    case LUA_TTABLE: {
        lua_getfield(L, idx, "__entity");
        if (!lua_isnil(L, -1)) {
            std::string s = "#" + std::to_string(static_cast<long long>(lua_tointeger(L, -1)));
            lua_pop(L, 1);
            return s;
        }
        lua_pop(L, 1);
        double v[3] = {0, 0, 0};
        const char* keys[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            lua_getfield(L, idx, keys[i]);
            v[i] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        return "(" + num(v[0]) + "," + num(v[1]) + "," + num(v[2]) + ")";
    }
    default: return "nil";
    }
}

int l_emit(lua_State* L) {
    const std::string name = luaL_checkstring(L, 1);
    std::map<std::string, std::string> kv;
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const std::string key = lua_tostring(L, -2);
                if (lua_type(L, -1) == LUA_TNUMBER && g_ents.count(static_cast<long long>(lua_tonumber(L, -1))) &&
                    (key == "target" || key == "source" || key == "entity" || key == "other"))
                    kv[key] = "#" + std::to_string(static_cast<long long>(lua_tonumber(L, -1)));
                else
                    kv[key] = luaValueText(L, -1);
            }
            lua_pop(L, 1);
        }
    }
    std::string line = std::to_string(g_step) + " " + name;
    for (const auto& [k, v] : kv) line += " " + k + "=" + v;
    g_log.push_back(line);
    return 0;
}

void reg(lua_State* L, const char* table, const luaL_Reg* fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, table);
}

// Mirrors apply_sandbox() in Source/FUSE/Script/src/script_vm.cpp.
void sandbox(lua_State* L) {
    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "require", "module", "package", "io", "debug",
                             "collectgarbage"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        lua_createtable(L, 0, 4);
        for (const char* name : {"clock", "time", "date", "difftime"}) {
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

void pushArg(lua_State* L, const json::Value& a) {
    if (const auto* e = a.get("entity")) {
        lua_pushinteger(L, static_cast<lua_Integer>(e->asInt()));
    } else if (const auto* v = a.get("vec3")) {
        double t[3] = {0, 0, 0};
        readVec(v, t);
        pushVec(L, t);
    } else {
        pushJson(L, a);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && !(argc == 5 && std::string(argv[3]) == "--expect")) {
        std::fprintf(stderr, "usage: %s <port.lua> <trace.json> [--expect <expected.log>]\n", argv[0]);
        return 2;
    }
    std::string lua, traceText, expected;
    if (!readFile(argv[1], lua) || !readFile(argv[2], traceText) || (argc == 5 && !readFile(argv[4], expected))) {
        std::fprintf(stderr, "lua_parity: cannot read the inputs\n");
        return 2;
    }
    json::ParseError perr;
    const auto trace = json::parse(traceText, &perr);
    if (!trace || !trace->isObject()) {
        std::fprintf(stderr, "lua_parity: bad trace JSON: %s at %zu\n", perr.message.c_str(), perr.offset);
        return 2;
    }
    static const json::Value kNull;
    const json::Value* ents = trace->get("entities");
    const json::Value* steps = trace->get("steps");
    for (const auto& [id, d] : (ents ? *ents : kNull).asObject()) applyEntity(g_ents[std::stoll(id)], d);
    const json::Value* beh = trace->get("behaviour");
    if (!beh || !beh->get("entity")) {
        std::fprintf(stderr, "lua_parity: trace has no behaviour.entity\n");
        return 2;
    }
    const long long selfId = beh->get("entity")->asInt();

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    sandbox(L);
    static const luaL_Reg kEntity[] = {{"alive", l_alive}, {"get_position", l_get_position},
                                       {"set_position", l_set_position}, {nullptr, nullptr}};
    static const luaL_Reg kPhysics[] = {{"get_velocity", l_get_velocity}, {nullptr, nullptr}};
    static const luaL_Reg kRemaster[] = {{"get", l_get}, {"set", l_set}, {"emit", l_emit}, {nullptr, nullptr}};
    reg(L, "Entity", kEntity);
    reg(L, "Physics", kPhysics);
    reg(L, "Remaster", kRemaster);

    int rc = 0;
    if (luaL_loadbuffer(L, lua.data(), lua.size(), "port.lua") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        g_log.push_back("0 error " + std::string(lua_tostring(L, -1)));
        lua_pop(L, 1);
        rc = 3;
    }
    // self: the behaviour instance table (properties + __entity)
    lua_newtable(L);
    if (const auto* props = beh->get("props"))
        for (const auto& [k, v] : props->asObject()) {
            pushJson(L, v);
            lua_setfield(L, -2, k.c_str());
        }
    lua_pushinteger(L, static_cast<lua_Integer>(selfId));
    lua_setfield(L, -2, "__entity");
    const int selfIdx = lua_gettop(L);

    if (rc == 0) {
        for (const auto& step : (steps ? *steps : kNull).asArray()) {
            ++g_step;
            if (const auto* s = step.get("set")) {
                applyEntity(g_ents[s->get("entity") ? s->get("entity")->asInt() : selfId], *s);
                continue;
            }
            const json::Value* call = step.get("call");
            if (!call) continue;
            lua_getglobal(L, call->asString().c_str());
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                g_log.push_back(std::to_string(g_step) + " error no handler " + call->asString());
                rc = 3;
                continue;
            }
            lua_pushvalue(L, selfIdx);
            int nargs = 1;
            if (const auto* args = step.get("args"))
                for (const auto& a : args->asArray()) {
                    pushArg(L, a);
                    ++nargs;
                }
            if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
                g_log.push_back(std::to_string(g_step) + " error " + std::string(lua_tostring(L, -1)));
                lua_pop(L, 1);
                rc = 3;
            }
        }
    }
    lua_close(L);

    std::string out;
    for (const auto& l : g_log) out += l + "\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    if (argc == 5) {
        std::string want;
        for (char c : expected)
            if (c != '\r') want += c;
        if (want != out) {
            std::fprintf(stderr, "lua_parity: event log differs from %s\n--- expected\n%s--- got\n%s", argv[4],
                         want.c_str(), out.c_str());
            return rc ? rc : 1;
        }
    }
    return rc;
}
