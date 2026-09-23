#include <fuse/script/script_runtime.hpp>

#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_vm.hpp>

#include "script_lua_compat.hpp"

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
#include <fuse/script/script_bind_lua.hpp>
#endif

#include <cstring>
#include <filesystem>
#include <utility>

namespace fuse::script {

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
namespace {

struct ModuleLoad {
    const char* source = nullptr;     // buffer source, or nullptr to read `path`
    const char* chunk_name = nullptr; // chunk name for buffer sources
    const char* path = nullptr;
    int old_class_ref = LUA_NOREF;
    int load_status = LUA_OK;
    std::string* parse_error = nullptr;
    int class_ref = LUA_NOREF; // out
    int meta_ref = LUA_NOREF;  // out (first load only)
};

/// Stack: 1 chunk, 2 env, 3 result. Patches the old module table in place on reload.
void load_module_fn(lua_State* L, void* user) {
    auto* load = static_cast<ModuleLoad*>(user);
    load->load_status = (load->source != nullptr)
                            ? luaL_loadbuffer(L, load->source, std::strlen(load->source), load->chunk_name)
                            : luaL_loadfile(L, load->path);
    if (load->load_status != LUA_OK) {
        const char* error = lua_tostring(L, -1);
        load->parse_error->assign(error != nullptr ? error : "lua parse error");
        return;
    }

    // Private environment falling back to the globals table.
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, 2);
    lua_pushvalue(L, 2);
    if (lua_setupvalue(L, 1, 1) == nullptr) {
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 1);
    lua_call(L, 0, 1);
    const int class_index = lua_istable(L, 3) ? 3 : 2;

    if (load->old_class_ref == LUA_NOREF) {
        lua_pushvalue(L, class_index);
        load->class_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_createtable(L, 0, 1);
        lua_pushvalue(L, class_index);
        lua_setfield(L, -2, "__index");
        load->meta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, load->old_class_ref);
    const int old_index = lua_gettop(L);
    // Drop keys the new version no longer defines.
    lua_pushnil(L);
    while (lua_next(L, old_index) != 0) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_rawget(L, class_index);
        const bool stale = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (stale) {
            lua_pushvalue(L, -1);
            lua_pushnil(L);
            lua_rawset(L, old_index);
        }
    }
    // Copy the new definitions over the live module table.
    lua_pushnil(L);
    while (lua_next(L, class_index) != 0) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, old_index);
    }
    if (lua_getmetatable(L, class_index) != 0) {
        lua_setmetatable(L, old_index);
    }
    load->class_ref = load->old_class_ref;
}

struct NewInstance {
    int meta_ref = LUA_NOREF;
    f64 entity = 0.0;
    int self_ref = LUA_NOREF; // out
};

void new_instance_fn(lua_State* L, void* user) {
    auto* request = static_cast<NewInstance*>(user);
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, static_cast<lua_Number>(request->entity));
    lua_setfield(L, -2, "entity");
    lua_rawgeti(L, LUA_REGISTRYINDEX, request->meta_ref);
    lua_setmetatable(L, -2);
    request->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

struct Unref {
    int refs[2] = {LUA_NOREF, LUA_NOREF};
};

void unref_fn(lua_State* L, void* user) {
    auto* unref = static_cast<Unref*>(user);
    for (int ref : unref->refs) {
        if (ref != LUA_NOREF && ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }
    }
}

struct Invoke {
    int self_ref = LUA_NOREF;
    const char* method = nullptr;
    int extra_args = 0; // 0: (self) 1: (self, a) 2: (self, a, point)
    f64 arg = 0.0;
    ecs::vec3 point{};
    bool found = false;
};

void invoke_fn(lua_State* L, void* user) {
    auto* call = static_cast<Invoke*>(user);
    lua_rawgeti(L, LUA_REGISTRYINDEX, call->self_ref);
    lua_getfield(L, 1, call->method);
    if (!lua_isfunction(L, 2)) {
        return;
    }
    call->found = true;
    lua_pushvalue(L, 1);
    if (call->extra_args >= 1) {
        lua_pushnumber(L, static_cast<lua_Number>(call->arg));
    }
    if (call->extra_args >= 2) {
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, call->point.x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, call->point.y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, call->point.z);
        lua_setfield(L, -2, "z");
    }
    lua_call(L, 1 + call->extra_args, 0);
}

struct FieldRead {
    int self_ref = LUA_NOREF;
    const char* field = nullptr;
    bind::ScriptValue* out = nullptr;
};

void field_read_fn(lua_State* L, void* user) {
    auto* read = static_cast<FieldRead*>(user);
    lua_rawgeti(L, LUA_REGISTRYINDEX, read->self_ref);
    lua_getfield(L, 1, read->field);
    *read->out = bind::lua::read_from_stack(L, 2);
}

} // namespace
#endif

ScriptRuntime::~ScriptRuntime() { shutdown(); }

bool ScriptRuntime::init(ScriptVM& vm, const ScriptEngineBindings& bindings) {
    if (m_vm != nullptr) {
        return m_vm == &vm;
    }
    if (!vm.is_initialized() || !vm.has_lua_backend()) {
        return false;
    }
    if (!bind_engine_api(vm, bindings)) {
        return false;
    }
    m_vm = &vm;
    m_bindings = bindings;
    m_frameCount = 0;
    m_callbackCount = 0;
    clear_errors();
    return true;
}

void ScriptRuntime::shutdown() {
    if (m_vm == nullptr) {
        return;
    }
    const bool vm_alive = m_vm->has_lua_backend();
    for (Instance& instance : m_instances) {
        if (vm_alive) {
            release_instance(instance, true);
        }
    }
    m_instances.clear();
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (vm_alive) {
        for (auto& entry : m_modules) {
            Unref unref;
            unref.refs[0] = entry.second.class_ref;
            unref.refs[1] = entry.second.meta_ref;
            m_vm->run_protected(unref_fn, &unref);
        }
    }
#endif
    m_modules.clear();
    m_vm = nullptr;
    m_bindings = {};
}

void ScriptRuntime::clear_errors() {
    m_errorCount = 0;
    m_lastError.clear();
}

void ScriptRuntime::record_error(const std::string& where, const char* message) {
    ++m_errorCount;
    m_lastError = where + ": " + (message != nullptr ? message : "script error");
}

ScriptLoadResult ScriptRuntime::load_module_file(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return {ScriptLoadStatus::InvalidArgument, "path is empty"};
    }
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        record_error(path, "file not found");
        return {ScriptLoadStatus::FileNotFound, m_lastError.c_str()};
    }
    return load_module(path, nullptr, path);
}

ScriptLoadResult ScriptRuntime::load_module_source(const char* module_name, const char* source) {
    if (module_name == nullptr || module_name[0] == '\0' || source == nullptr) {
        return {ScriptLoadStatus::InvalidArgument, "module name or source is empty"};
    }
    return load_module(module_name, source, nullptr);
}

ScriptLoadResult ScriptRuntime::load_module(const char* module_name, const char* source, const char* path) {
    if (m_vm == nullptr) {
        return {ScriptLoadStatus::BackendUnavailable, "script runtime not initialized"};
    }
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    const auto existing = m_modules.find(module_name);
    std::string parse_error;
    ModuleLoad load;
    load.source = source;
    load.chunk_name = module_name;
    load.path = path;
    load.old_class_ref = (existing != m_modules.end()) ? existing->second.class_ref : LUA_NOREF;
    load.parse_error = &parse_error;

    const ScriptLoadResult run = m_vm->run_protected(load_module_fn, &load);
    if (load.load_status != LUA_OK) {
        record_error(module_name, parse_error.c_str());
        return {load.load_status == LUA_ERRSYNTAX ? ScriptLoadStatus::ParseError : ScriptLoadStatus::RuntimeError,
                m_lastError.c_str()};
    }
    if (!run.ok()) {
        record_error(module_name, run.message);
        return {run.status, m_lastError.c_str()};
    }

    if (existing != m_modules.end()) {
        ++existing->second.version;
    } else {
        Module module;
        module.name = module_name;
        module.class_ref = load.class_ref;
        module.meta_ref = load.meta_ref;
        module.version = 1;
        m_modules.emplace(module.name, std::move(module));
    }
    return {ScriptLoadStatus::Ok, nullptr};
#else
    (void)module_name;
    (void)source;
    (void)path;
    return {ScriptLoadStatus::BackendUnavailable, "lua backend unavailable"};
#endif
}

bool ScriptRuntime::has_module(const char* module_name) const {
    return module_name != nullptr && m_modules.find(module_name) != m_modules.end();
}

u32 ScriptRuntime::module_version(const char* module_name) const {
    if (module_name == nullptr) {
        return 0;
    }
    const auto it = m_modules.find(module_name);
    return it != m_modules.end() ? it->second.version : 0u;
}

ScriptRuntime::Instance* ScriptRuntime::find_instance(ecs::EntityID entity) {
    for (Instance& instance : m_instances) {
        if (instance.entity == entity) {
            return &instance;
        }
    }
    return nullptr;
}

bool ScriptRuntime::is_attached(ecs::EntityID entity) const {
    for (const Instance& instance : m_instances) {
        if (instance.entity == entity) {
            return true;
        }
    }
    return false;
}

bool ScriptRuntime::attach(ecs::EntityID entity, const char* module_name) {
    if (m_vm == nullptr || module_name == nullptr || !entity.valid() || is_attached(entity)) {
        return false;
    }
    const auto module = m_modules.find(module_name);
    if (module == m_modules.end()) {
        return false;
    }
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    NewInstance request;
    request.meta_ref = module->second.meta_ref;
    request.entity = encode_entity_id(entity);
    if (!m_vm->run_protected(new_instance_fn, &request).ok()) {
        record_error(module_name, m_vm->last_error().c_str());
        return false;
    }
    Instance instance;
    instance.entity = entity;
    instance.module = module_name;
    instance.self_ref = request.self_ref;
    m_instances.push_back(std::move(instance));
    return true;
#else
    return false;
#endif
}

void ScriptRuntime::release_instance(Instance& instance, bool call_destroy) {
    if (call_destroy && instance.started) {
        invoke(instance, Callback::Destroy, 0.f, ecs::EntityID::null(), {});
    }
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (m_vm != nullptr && m_vm->has_lua_backend()) {
        Unref unref;
        unref.refs[0] = instance.self_ref;
        m_vm->run_protected(unref_fn, &unref);
    }
#endif
    instance.self_ref = -1;
}

bool ScriptRuntime::detach(ecs::EntityID entity) {
    for (usize i = 0; i < m_instances.size(); ++i) {
        if (m_instances[i].entity == entity) {
            Instance instance = std::move(m_instances[i]);
            m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(i));
            release_instance(instance, true);
            return true;
        }
    }
    return false;
}

bool ScriptRuntime::invoke(Instance& instance, Callback callback, f32 dt, ecs::EntityID other,
                           const ecs::vec3& point) {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (m_vm == nullptr || instance.self_ref < 0) {
        return false;
    }
    Invoke call;
    call.self_ref = instance.self_ref;
    switch (callback) {
    case Callback::Start:
        call.method = "on_start";
        break;
    case Callback::Update:
        call.method = "on_update";
        call.extra_args = 1;
        call.arg = static_cast<f64>(dt);
        break;
    case Callback::Collision:
        call.method = "on_collision";
        call.extra_args = 2;
        call.arg = encode_entity_id(other);
        call.point = point;
        break;
    case Callback::TriggerEnter:
        call.method = "on_trigger_enter";
        call.extra_args = 1;
        call.arg = encode_entity_id(other);
        break;
    case Callback::Destroy:
        call.method = "on_destroy";
        break;
    }
    const ScriptLoadResult result = m_vm->run_protected(invoke_fn, &call);
    if (call.found) {
        ++m_callbackCount;
    }
    if (!result.ok()) {
        record_error(instance.module + ":" + call.method, result.message);
        return false;
    }
    return call.found;
#else
    (void)instance;
    (void)callback;
    (void)dt;
    (void)other;
    (void)point;
    return false;
#endif
}

void ScriptRuntime::update(f32 dt) {
    if (m_vm == nullptr) {
        return;
    }
    ++m_frameCount;

    if (m_bindings.registry != nullptr) {
        for (usize i = m_instances.size(); i-- > 0;) {
            if (!m_bindings.registry->alive(m_instances[i].entity)) {
                Instance instance = std::move(m_instances[i]);
                m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(i));
                release_instance(instance, true);
            }
        }
    }

    for (usize i = 0; i < m_instances.size(); ++i) {
        if (!m_instances[i].started) {
            m_instances[i].started = true;
            invoke(m_instances[i], Callback::Start, 0.f, ecs::EntityID::null(), {});
        }
        if (i < m_instances.size()) {
            invoke(m_instances[i], Callback::Update, dt, ecs::EntityID::null(), {});
        }
    }
}

void ScriptRuntime::dispatch_collision(ecs::EntityID entity, ecs::EntityID other, const ecs::vec3& point) {
    // An entity destroyed earlier in this dispatch (e.g. `Entity.destroy(self)` on its first of
    // several same-step contacts) gets no further callbacks; `update` detaches it.
    if (m_bindings.registry != nullptr && !m_bindings.registry->alive(entity)) {
        return;
    }
    if (Instance* instance = find_instance(entity)) {
        invoke(*instance, Callback::Collision, 0.f, other, point);
    }
}

void ScriptRuntime::dispatch_trigger_enter(ecs::EntityID entity, ecs::EntityID other) {
    if (m_bindings.registry != nullptr && !m_bindings.registry->alive(entity)) {
        return;
    }
    if (Instance* instance = find_instance(entity)) {
        invoke(*instance, Callback::TriggerEnter, 0.f, other, {});
    }
}

bool ScriptRuntime::get_instance_field(ecs::EntityID entity, const char* field, bind::ScriptValue& out) {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    Instance* instance = find_instance(entity);
    if (m_vm == nullptr || instance == nullptr || field == nullptr) {
        return false;
    }
    FieldRead read;
    read.self_ref = instance->self_ref;
    read.field = field;
    read.out = &out;
    return m_vm->run_protected(field_read_fn, &read).ok();
#else
    (void)entity;
    (void)field;
    (void)out;
    return false;
#endif
}

} // namespace fuse::script
