#pragma once

// MP-B6-QT-INSPECTOR / UNI-U6-INSP-1: typed, Qt-free description of every ECS component the
// editor inspector can edit, add and remove. The Qt inspector generates its editors from this
// table; the game thread applies the resulting `SetProperty` / `AddComponent` / `RemoveComponent`
// commands through the same table (editor_command_apply.cpp), so the UI never touches components.

#include <fuse/editor/undo_stack.hpp>

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/types.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::editor {

/// Editor widget a field maps to (and the text format of its `SetProperty` value).
enum class PropertyFieldType : u8 {
    Float,     ///< one float (`formatPropertyFloat`)
    Vec3,      ///< "x,y,z"
    Euler,     ///< quaternion storage "x,y,z,w"; the UI edits it as XYZ Euler degrees
    Color,     ///< linear RGB "r,g,b" (HDR values above 1 allowed)
    Enum,      ///< integer index into `enumNames`
    Bool,      ///< "1" / "0" (parsing also accepts true / false)
    UInt,      ///< decimal u32
    String,    ///< raw text; the empty string is `kPropertyEmptyString`
    AssetPath, ///< raw text path (empty = `kPropertyEmptyString`); the UI adds a file picker
};

/// Text of an empty String / AssetPath value in SetProperty commands. (The undo stack treats an
/// empty "before" value as "no before value", so an empty path would not undo back to empty.)
inline constexpr const char* kPropertyEmptyString = "\"\"";

struct PropertyFieldDesc {
    const char* label = "";        ///< field name shown in the inspector
    const char* propertyName = ""; ///< `EditorCommand::propertyName` that edits it
    PropertyFieldType type = PropertyFieldType::Float;
    f64 minValue = -1.0e6;
    f64 maxValue = 1.0e6;
    f64 step = 0.1;
    const char* const* enumNames = nullptr;
    u32 enumCount = 0;
    const char* assetFilter = nullptr; ///< e.g. "Lua scripts (*.lua)"
    /// Reads the field from a component instance of the owning kind, formatted as `type` says.
    bool (*read)(const void* component, std::string& out) = nullptr;
    /// Parses `value` and writes it into a component instance. False leaves it unchanged.
    bool (*write)(void* component, std::string_view value) = nullptr;
};

/// One component type the inspector knows (typed fields plus type-erased registry operations).
struct ComponentKindDesc {
    const char* name = "";        ///< `T::component_name` (also the AddComponent argument)
    const char* displayName = ""; ///< inspector section title
    std::span<const PropertyFieldDesc> fields{};
    bool addable = true;   ///< offered by the Add Component menu
    bool removable = true; ///< Transform is not: the hierarchy is built from it
    usize size = 0;        ///< sizeof(T)
    bool (*has)(const ecs::Registry& registry, ecs::EntityID entity) = nullptr;
    void* (*get)(ecs::Registry& registry, ecs::EntityID entity) = nullptr;
    const void* (*getConst)(const ecs::Registry& registry, ecs::EntityID entity) = nullptr;
    /// Adds the component from `bytes` (`size` bytes of a T) or a default T when null.
    void (*add)(ecs::Registry& registry, ecs::EntityID entity, const void* bytes) = nullptr;
    void (*remove)(ecs::Registry& registry, ecs::EntityID entity) = nullptr;
};

/// Registers the editor-inspectable component types that are not ECS built-ins with
/// `ecs::ComponentTypes` (AudioSource when the audio module is linked), so the inspector lists them
/// and `.fuselevel` v3 saves them. Idempotent; also registers the built-ins.
void registerEditorComponentTypes();

/// Every kind the inspector can edit / add / remove, in inspector order.
[[nodiscard]] std::span<const ComponentKindDesc> editorComponentKinds();
[[nodiscard]] const ComponentKindDesc* findComponentKind(std::string_view componentName);
/// The field edited by `propertyName` (and its kind). Null for names outside the schema.
[[nodiscard]] const PropertyFieldDesc* findComponentProperty(std::string_view propertyName,
                                                             const ComponentKindDesc** kindOut = nullptr);

/// Formatted value of a schema property on `entity` (false: no such property / component).
bool readComponentProperty(const ecs::Registry& registry, ecs::EntityID entity, std::string_view propertyName,
                           std::string& out);
/// Parses `value` into a schema property on `entity` (marks Transform dirty). False on bad input.
bool writeComponentProperty(ecs::Registry& registry, ecs::EntityID entity, std::string_view propertyName,
                            std::string_view value);

// ---- value helpers shared by the Qt editors and the command apply path ---------------------------

/// Quaternion for XYZ Euler angles in degrees (rotate about X, then Y, then Z: q = qz * qy * qx).
[[nodiscard]] ecs::quat quatFromEulerDeg(f32 xDeg, f32 yDeg, f32 zDeg);
/// XYZ Euler degrees of a unit quaternion (inverse of `quatFromEulerDeg`, Y in [-90, 90]).
[[nodiscard]] ecs::vec3 eulerDegFromQuat(const ecs::quat& rotation);
[[nodiscard]] std::string formatPropertyQuat(const ecs::quat& rotation);
bool parsePropertyFloats(std::string_view text, f32* values, u32 count);

// ---- world pose (hierarchy reparent keeps the world transform) -----------------------------------

/// Local-to-world matrix of `entity` from the TRS chain of Transform::parent (not the cached
/// `local_to_world`, which is stale until the transform system runs). Identity without Transform.
[[nodiscard]] ecs::mat4 entityWorldMatrix(const ecs::Registry& registry, ecs::EntityID entity);
/// Decomposes `inverse(parentWorld) * world` into local TRS (exact without shear).
void localTrsForWorld(const ecs::mat4& parentWorld, const ecs::mat4& world, ecs::vec3& position,
                      ecs::quat& rotation, ecs::vec3& scale);

// ---- undoable component add / remove (UndoStack) --------------------------------------------------

/// Adds (`adding`) or removes one component of `kind` on `entity`. Removing captures the
/// component's bytes so undo restores it exactly; redo of an add re-adds the same values.
class ComponentPresenceCommand final : public UndoCommand {
public:
    ComponentPresenceCommand(ecs::Registry& registry, ecs::EntityID entity, const ComponentKindDesc& kind,
                             bool adding);

    void execute() override;
    void undo() override;
    std::string description() const override;

    [[nodiscard]] bool applied() const { return m_applied; }

private:
    void addFromSnapshot_();
    void removeWithSnapshot_();

    ecs::Registry& m_registry;
    ecs::EntityID m_entity = ecs::EntityID::null();
    const ComponentKindDesc* m_kind = nullptr;
    bool m_adding = true;
    bool m_applied = false;
    bool m_haveSnapshot = false;
    std::vector<unsigned char> m_snapshot;
};

} // namespace fuse::editor
