// FUSE Relight RL-3.5: what Logic graph components can sense (docs/plans/FUSE_REMIX_PORT_PLAN.md, Wave R3).
//
// Upstream components read the renderer directly through the DxvkContext (SceneManager, RtCamera, LightManager,
// GlobalTime, the developer-menu key state). Relight evaluates graphs on the CPU from plain per-frame data instead,
// so the evaluation is a pure function of (graph state, FrameInputs): the same inputs give the same outputs on every
// platform, which is what the per-frame fixture tests rely on.
//
//   FrameInputs    the frame's senses: delta time, main camera, per-hash usage of meshes (asset hash) and
//                  textures (stage-0 colour texture hash) this frame, the frame's light hashes, the active fog
//                  hash, keyboard state (Windows virtual-key codes) and, per graph owner, the prim table
//                  snapshots that Prim properties resolve against.
//   LogicContext   FrameInputs + the prim resolution used by the component batches.
#pragma once

#include <fuse/relight/logic/graph_types.hpp>
#include <fuse/relight/logic/logic_math.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace fuse::relight::logic {

/// 60 degrees: upstream's default vertical FOV for an invalid camera.
inline constexpr float kDefaultFovRadians = kPi / 3.0f;

struct CameraSense {
    bool valid = false; ///< a main camera was seen this frame (else the outputs are the defaults)
    Vector3 position{0.0f, 0.0f, 0.0f};
    Vector3 forward{0.0f, 0.0f, -1.0f};
    Vector3 right{1.0f, 0.0f, 0.0f};
    Vector3 up{0.0f, 1.0f, 0.0f};
    float fovRadians = kDefaultFovRadians;
    float aspectRatio = 1.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

/// One prim of a replacement, as a Prim property resolves it (upstream PrimInstance: RtInstance / RtLight / graph).
struct PrimSnapshot {
    enum class Kind : std::uint8_t { None, Mesh, Light, Graph };
    Kind kind = Kind::None;
    Matrix4 objectToWorld;                ///< Mesh: instance transform
    AxisAlignedBoundingBox bounds;        ///< Mesh: object-space bounds (invalid when unknown)
    std::vector<Matrix4> boneMatrices;    ///< Mesh: skinning bone matrices (empty: not skinned)
    Vector3 lightPosition{0.0f, 0.0f, 0.0f}; ///< Light: world position
};

struct FrameInputs {
    std::uint64_t frame = 0;
    float deltaTime = 1.0f / 60.0f; ///< seconds (GlobalTime::deltaTime)
    CameraSense camera;
    std::map<std::uint64_t, std::uint32_t> meshHashUsage;    ///< asset hash -> draws this frame
    std::map<std::uint64_t, std::uint32_t> textureHashUsage; ///< texture hash -> draws this frame
    std::set<std::uint64_t> lightHashes;                     ///< game lights (and replacements) this frame
    std::uint64_t fogHash = 0;                               ///< the frame's fog state hash (0: no fog)
    std::set<std::uint32_t> keysDown;    ///< VK codes held now
    std::set<std::uint32_t> keysPressed; ///< VK codes that went down this frame
    /// Per graph owner (GraphInstance::owner()): the prim table, indexed by PrimTarget::replacementIndex.
    std::map<std::uint64_t, std::vector<PrimSnapshot>> prims;
};

class LogicContext {
public:
    explicit LogicContext(const FrameInputs& inputs) : m_inputs(inputs) {}
    const FrameInputs& inputs() const { return m_inputs; }
    float deltaTime() const { return m_inputs.deltaTime; }
    /// nullptr: no owner table, an invalid target, or a target in another draw (unsupported, as upstream).
    const PrimSnapshot* resolvePrim(std::uint64_t owner, const PrimTarget& target) const;

private:
    const FrameInputs& m_inputs;
};

} // namespace fuse::relight::logic
