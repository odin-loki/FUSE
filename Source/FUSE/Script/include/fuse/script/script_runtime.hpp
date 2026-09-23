#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/script/script_bind.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::script {

class ScriptVM;

/// Per-entity Lua behaviours (B7.3 "Script component").
///
/// A module is a Lua chunk. It either returns a table of callbacks or defines them as globals
/// (each module runs in its own environment that falls back to `_G`). Each attached entity gets
/// an instance table `self` (with `self.entity`) whose metatable indexes the module table:
///
///   function on_start(self) end
///   function on_update(self, dt) end
///   function on_collision(self, other, point) end   -- other: entity id, point: {x,y,z}
///   function on_trigger_enter(self, other) end
///   function on_destroy(self) end
///
/// Hot reload re-runs the chunk and patches the existing module table in place: instances keep
/// their `self` state and call the new functions on their next callback. A reload that fails
/// to compile or run leaves the previous code active. Every callback runs protected: a script
/// error is recorded and the frame continues with the next instance.
class ScriptRuntime {
public:
    ScriptRuntime() = default;
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    /// `vm` must be initialized with a Lua backend and outlive the runtime (non-owning).
    /// Binds the Entity/Physics API tables with `bindings`.
    bool init(ScriptVM& vm, const ScriptEngineBindings& bindings = {});
    /// Calls `on_destroy` on every instance, then releases all Lua references.
    void shutdown();
    [[nodiscard]] bool is_initialized() const { return m_vm != nullptr; }

    /// Load (or hot-reload, when already loaded) a module from a file; the key is the path.
    ScriptLoadResult load_module_file(const char* path);
    /// Load (or hot-reload) a module from source under `module_name`.
    ScriptLoadResult load_module_source(const char* module_name, const char* source);
    [[nodiscard]] bool has_module(const char* module_name) const;
    /// Number of successful (re)loads of `module_name` (0 when unknown).
    [[nodiscard]] u32 module_version(const char* module_name) const;
    [[nodiscard]] usize module_count() const { return m_modules.size(); }

    /// Attach a loaded module to `entity`. `on_start` runs on the next `update`.
    bool attach(ecs::EntityID entity, const char* module_name);
    /// Call `on_destroy` and drop the instance.
    bool detach(ecs::EntityID entity);
    [[nodiscard]] bool is_attached(ecs::EntityID entity) const;
    [[nodiscard]] usize instance_count() const { return m_instances.size(); }

    /// One frame: detach instances whose entity died (bound registry only), then for each
    /// instance in attach order run `on_start` once (first frame) followed by `on_update(dt)`.
    void update(f32 dt);

    /// Contact callbacks for `entity`'s instance (no-op when it has none).
    void dispatch_collision(ecs::EntityID entity, ecs::EntityID other, const ecs::vec3& point);
    void dispatch_trigger_enter(ecs::EntityID entity, ecs::EntityID other);

    /// Read `self[field]` of an entity's instance (numbers, strings, booleans, nil).
    bool get_instance_field(ecs::EntityID entity, const char* field, bind::ScriptValue& out);

    [[nodiscard]] u64 frame_count() const { return m_frameCount; }
    [[nodiscard]] u64 callback_count() const { return m_callbackCount; }
    [[nodiscard]] usize error_count() const { return m_errorCount; }
    [[nodiscard]] const std::string& last_error() const { return m_lastError; }
    void clear_errors();

    [[nodiscard]] ScriptVM* vm() const { return m_vm; }

private:
    enum class Callback : u8 { Start, Update, Collision, TriggerEnter, Destroy };

    struct Module {
        std::string name;
        int class_ref = -1;
        int meta_ref = -1;
        u32 version = 0;
    };

    struct Instance {
        ecs::EntityID entity = ecs::EntityID::null();
        std::string module;
        int self_ref = -1;
        bool started = false;
    };

    ScriptLoadResult load_module(const char* module_name, const char* source, const char* path);
    bool invoke(Instance& instance, Callback callback, f32 dt, ecs::EntityID other,
                const ecs::vec3& point);
    Instance* find_instance(ecs::EntityID entity);
    void release_instance(Instance& instance, bool call_destroy);
    void record_error(const std::string& where, const char* message);

    ScriptVM* m_vm = nullptr; // non-owning
    ScriptEngineBindings m_bindings{};
    std::unordered_map<std::string, Module> m_modules;
    std::vector<Instance> m_instances;
    u64 m_frameCount = 0;
    u64 m_callbackCount = 0;
    usize m_errorCount = 0;
    std::string m_lastError;
};

} // namespace fuse::script
