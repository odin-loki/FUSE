// FUSE Relight RL-3.6: particle systems from mods, and spawning from replaced / attached geometry.
//
// Descriptions. A USD ParticleSystemAPI prim (RL-3.1 usd::ParticleSystem) and an RL-3.2 relight_particles record carry
// the same `primvars:particle:*` values (the record stores them raw, as JSON); descFromPrimvars turns them into a
// ParticleSystemDesc as upstream processParticleSystem does: the schema's fallbacks for unauthored values; the
// animated channels baked from their curves (curve_bake.hpp: minColor / maxColor gradients, minSize:x|y,
// maxSize:x|y, min/maxRotationSpeed, maxVelocity:x|y|z with tangents); a channel with no valid curve falls back to
// its legacy spawn -> target pair (minSpawnColor -> minTargetColor, ...; the deprecated maxSpeed for maxVelocity).
//
// Catalog. ParticleCatalog resolves the particle system of a replacement record (relight_replacement for mesh_<H>,
// material for mat_<H>: the record id is the relight_particles id) and the geometry of a mesh record (Position /
// Color0 / Uv0 streams and indices32 of the RL-3.2 store layout), per mod of a ReplacementEngine's stack: FUSE-native
// stores are read from disk, Remix USD mods are imported in memory once per engine generation (the same records the
// engine read). Lookups are cached by (mod, record) and cleared when the engine reloads.
//
// Emitters (upstream SceneManager::processDrawCallState priority: mesh replacement, then material, then the texture
// category ParticleEmitter with rtx.particles.globalPreset):
//   * a mesh replacement with parts: each part spawns from its own mesh with its world transform, with the
//     replacement's system, else its material's system, else the preset when the draw is a ParticleEmitter;
//   * the original draw (kept: no mesh replacement or preserveOriginalDrawCall) spawns from the draw's own geometry
//     (a mesh the host registered) with the draw's material replacement's system, else the preset for
//     ParticleEmitter draws. FUSE extension: a mesh replacement without parts that carries a particle system (a
//     ParticleSystemAPI on an empty mesh_<H>, which spawns nothing upstream) emits from the original geometry, which
//     stays hidden unless preserveOriginalDrawCall;
//   * hideEmitter hides the emitting part / draw (reported, as upstream sets the instance hidden).
// Systems are keyed by (description, material): the part's material record, or the draw's stage-0 texture hash.
// Previous transforms come from the emitter instance of the previous frame (instance id, part index); with
// rtx.particles.enableDiscontinuityGuard a jump away from the emitter's recent motion collapses them (upstream
// resolveSpawnPrevTransform).
#pragma once

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/particles/particle_system.hpp>
#include <fuse/relight/replace/replacement_engine.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::relight::particles {

namespace json = capture::exporter::json;

/// The description of `primvars` (names without the "primvars:particle:" prefix, values as usd::valueToJson /
/// RL-3.2 records write them). `issues` gets one line per ignored value (wrong type or shape).
ParticleSystemDesc descFromPrimvars(const json::Value& primvars, std::vector<std::string>* issues = nullptr);
/// A USD ParticleSystemAPI prim (mods::usd::readParticleSystem).
ParticleSystemDesc descFromUsd(const mods::usd::ParticleSystem& ps, std::vector<std::string>* issues = nullptr);
/// A relight_particles POCO record (its payload.primvars); nullopt when it is not one.
std::optional<ParticleSystemDesc> descFromRecord(const json::Value& record, std::vector<std::string>* issues = nullptr);

/// Emitter geometry decoded from a mesh record (owned).
struct EmitterMeshData {
    std::vector<float> positions;
    std::vector<std::uint32_t> colors;
    std::vector<float> texcoords;
    std::vector<std::uint32_t> indices;
    EmitterMesh view() const { return EmitterMesh{positions, {}, colors, texcoords, indices}; }
};
/// Decodes a mesh POCO record through `read` (store-relative paths); nullopt when a stream is missing or malformed.
std::optional<EmitterMeshData> meshFromRecord(const json::Value& record, const replace::StoreReader& read);

class ParticleCatalog {
public:
    explicit ParticleCatalog(const replace::ReplacementEngine& engine);
    ~ParticleCatalog();

    /// Drops every cache when the engine's generation changed (call once per frame).
    void sync();
    /// The particle system of record `recordId` of mod `modName` (nullptr: none).
    const ParticleSystemDesc* particles(const std::string& modName, const std::string& recordId);
    std::uint64_t particlesHash(const std::string& modName, const std::string& recordId);
    /// The geometry of mesh record `meshId` of mod `modName` (nullptr: unreadable).
    const EmitterMeshData* mesh(const std::string& modName, const std::string& meshId);
    /// Diagnostics of the lookups since the last sync ("<mod>: <record>: message").
    const std::vector<std::string>& diagnostics() const { return m_diagnostics; }

private:
    struct ModReader;
    ModReader* reader(const std::string& modName);
    const replace::ReplacementEngine& m_engine;
    std::uint64_t m_generation = 0;
    std::map<std::string, std::unique_ptr<ModReader>> m_readers;
    struct DescEntry {
        std::optional<ParticleSystemDesc> desc;
        std::uint64_t hash = 0;
    };
    std::unordered_map<std::uint64_t, DescEntry> m_descs;
    std::unordered_map<std::uint64_t, std::optional<EmitterMeshData>> m_meshes;
    std::vector<std::string> m_diagnostics;
};

struct EmitterFrameStats {
    std::uint32_t emitters = 0;      ///< spawn requests made
    std::uint32_t particles = 0;     ///< slots queued
    std::uint32_t meshSystems = 0;   ///< requests with a mesh replacement's system
    std::uint32_t materialSystems = 0;
    std::uint32_t presetSystems = 0;
    std::uint32_t hiddenEmitters = 0;
};

struct EmitterResult {
    bool hideOriginal = false;    ///< the kept original draw spawned with hideEmitter
    std::uint64_t hiddenParts = 0; ///< bit k: part k spawned with hideEmitter (parts >= 64 are never hidden)
    std::uint32_t requests = 0;
};

class ParticleEmitterBridge {
public:
    ParticleEmitterBridge(ParticleSystemManager& manager, ParticleCatalog& catalog);

    /// Once per frame, before the draws (catalog sync, stats reset).
    void beginFrame(std::uint64_t frame);
    /// One committed draw after ReplacementEngine::replaceDraw. `originalMesh`: the draw's own geometry registered
    /// with the manager (kInvalidMesh: unknown; the original draw then spawns nothing).
    EmitterResult processDraw(const replace::DrawInput& input, const replace::ReplacedDraw& draw, EmitterMeshId originalMesh);
    const EmitterFrameStats& stats() const { return m_stats; }

private:
    struct Choice {
        const ParticleSystemDesc* desc = nullptr;
        std::uint64_t hash = 0;
        int source = 0; ///< 1 mesh, 2 material, 3 preset
    };
    Mat34 prevTransform(std::uint64_t instance, std::uint32_t part, const Mat34& current);
    bool request(const Choice& c, std::uint64_t materialKey, EmitterMeshId mesh, const Mat34& world, const Mat34& prev);
    EmitterMeshId partMesh(const std::string& mod, const std::string& meshId);

    ParticleSystemManager& m_manager;
    ParticleCatalog& m_catalog;
    std::uint64_t m_frame = 0;
    EmitterFrameStats m_stats;
    ParticleSystemDesc m_preset;
    std::uint64_t m_presetHash = 0;
    struct Motion {
        Mat34 last{};
        std::uint64_t frame = 0;
        float average[3] = {0.f, 0.f, 0.f};
        bool discontinuity = false;
    };
    std::unordered_map<std::uint64_t, Motion> m_motion;
    std::unordered_map<std::uint64_t, EmitterMeshId> m_partMeshes;
};

} // namespace fuse::relight::particles
