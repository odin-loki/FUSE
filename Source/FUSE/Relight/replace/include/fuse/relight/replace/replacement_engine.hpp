// FUSE Relight RL-3.4: the runtime replacement engine (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.2, §4.6, §4.7; port of
// the semantics of dxvk-remix SceneManager::submitDrawState / drawReplacements and LightManager's light
// replacement, MIT: facts only, no code copied).
//
// Per frame:
//   beginFrame(frame)    hot reload: file notifications since the last frame reload the changed mods (and discover
//                        added / removed ones) before any draw of this frame is looked up, so a change is applied at
//                        the first frame boundary after its notification (latency <= 1 frame). The first call loads
//                        every mod.
//   replaceDraw(draw)    per committed draw:
//                          mesh      the draw's asset hash under each mod rule in the stack (a mod keeps the rule
//                                    it was authored with; legacy rules through meshReplacementHashLegacy) ->
//                                    the strongest mod's mesh_<H>: parts placed with part transform * the
//                                    draw's objectToWorld; the original draw is hidden unless
//                                    preserveOriginalDrawCall; category overrides applied; attached lights
//                                    follow the instance (light transform * objectToWorld);
//                          material  the stage-0 texture hash -> the strongest mat_<H>: applied to the original
//                                    draw when it is kept, and to replacement parts without a bound material
//                                    (untextured draws: hash 0, never material-replaced, as upstream);
//   endFrame(lights)     the frame's game lights: a light_<H> replaces (its own light and transform) or deletes
//                        (a light_<H> without a light) the game light; attached lights are added; the texture
//                        residency policy runs on the textures this frame's replacements use.
// Options (replace_options.hpp): rtx.enableReplacementAssets / Meshes / Materials / Lights gate each kind, read at
// beginFrame (after a reload), so a mod's rtx.conf layer or an option change takes effect at the next frame.
//
// Per-mod rtx.conf layers: every mod with an rtx.conf gets an option layer at the baseGameMod priority (4: above
// the game's rtx.conf, below environment variables and user.conf), named "Mod Remix Config <rank> <name>", so the
// stronger mod's value wins among mods (equal priorities order by name). Layers follow the stack on every reload
// and are released with the engine.
#pragma once

#include <fuse/relight/replace/file_watcher.hpp>
#include <fuse/relight/replace/mod_stack.hpp>
#include <fuse/relight/replace/texture_residency.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fuse::relight::replace {

struct EngineConfig {
    std::vector<ModRoot> roots; ///< search roots (children are mods)
    std::vector<ModRoot> mods;  ///< explicit mod directories
    std::string gameId = "game";
    std::vector<std::string> modOrder;
    bool hotReload = true;
    WatchBackend watchBackend = WatchBackend::Auto;
    std::uint32_t pollIntervalMs = 0;
    bool optionLayers = true;
    ResidencyConfig residency;
    // Test hook (replace_options.hpp debugReloadWaitFrame): 0 = off.
    std::uint64_t debugReloadWaitFrame = 0;
    std::string debugReloadMarker;
    std::uint32_t debugReloadWaitMs = 20000;

    /// The relight.replace.* / relight.modOrder options, resolved now; `gameId` when relight.replace.gameId is empty.
    static EngineConfig fromOptions(const std::string& gameId);
};

/// One committed draw, as the engine looks it up.
struct DrawInput {
    std::uint32_t index = 0; ///< draw number in the frame
    bool geometryValid = false;
    hash::GeometryHashes hashes;
    /// The legacy-key inputs (meshReplacementHashLegacy); called only when a legacy-rule mod is loaded.
    std::function<hash::DrawGeometryHashes()> legacyHashes;
    Hash64 materialHash = 0; ///< stage-0 colour texture hash (0: untextured)
    scene::CategoryFlags categories;
    Mat4d objectToWorld = identity4d();
    std::uint64_t instanceId = 0; ///< RL-1.7 instance (0: none)
};

struct ReplacedLight {
    enum class Origin : std::uint8_t { Game, Replaced, Attached };
    Origin origin = Origin::Game;
    Hash64 gameHash = 0; ///< Game / Replaced: the game light's hash
    std::string mod;     ///< Replaced / Attached: the mod
    std::string recordId;
    std::string type;    ///< Game: sphere / distant; else the POCO type
    Vec3d position{0.0, 0.0, 0.0};
    Vec3d direction{0.0, 0.0, 1.0};
    Vec3d color{1.0, 1.0, 1.0}; ///< Game: radiance
    double intensity = 0.0;
    std::uint64_t instanceId = 0; ///< Attached: the instance it follows
    std::uint32_t drawIndex = 0;  ///< Attached: the draw it came from
};
const char* lightOriginName(ReplacedLight::Origin o);

struct ReplacedPart {
    std::string meshId;
    Mat4d objectToWorld = identity4d();
    std::string material;       ///< material record id ("" : the draw's legacy material)
    std::string materialMod;
    std::string materialSource; ///< "bound", "replacement" (mat_<H> of the draw's texture) or "legacy"
};

struct ReplacedDraw {
    std::uint32_t index = 0;
    std::uint64_t instanceId = 0;
    bool meshReplaced = false;
    std::string meshMod, meshRecord, meshRule;
    Hash64 meshKey = 0;
    std::vector<std::string> meshShadowed; ///< weaker mods replacing the same key
    bool drawOriginal = true;              ///< false: the original draw is hidden (mesh replacement without preserve)
    std::vector<ReplacedPart> parts;
    std::vector<ReplacedLight> lights;     ///< attached lights of this instance
    bool materialReplaced = false;         ///< the kept original draw uses a material replacement
    std::string materialMod, materialRecord;
    std::vector<std::string> materialShadowed;
    bool materialIgnored = false;          ///< the material replacement says ignore_material
    scene::CategoryFlags categories;
    bool categoriesChanged = false;

    bool affected() const { return meshReplaced || materialReplaced; }
};

struct ReloadEvent {
    std::uint64_t generation = 0;
    std::uint64_t notifiedFrame = 0; ///< frame at which the notification was observed
    std::uint64_t appliedFrame = 0;  ///< first frame whose draws use the reloaded content
    std::vector<std::string> changed, added, removed, failed; ///< mod names
    ReplacementIndex::Diff invalidated;
};

struct ReplaceFrameStats {
    std::uint32_t draws = 0;
    std::uint32_t meshReplaced = 0;
    std::uint32_t hidden = 0;
    std::uint32_t preserved = 0;
    std::uint32_t parts = 0;
    std::uint32_t materialReplaced = 0;     ///< kept original draws with a material replacement
    std::uint32_t partMaterialReplaced = 0; ///< parts using the draw's mat_<H>
    std::uint32_t lightsGame = 0, lightsReplaced = 0, lightsDeleted = 0, lightsAttached = 0;
};

struct ReplacedFrame {
    std::uint64_t frame = 0;
    std::uint64_t generation = 0;
    std::vector<ReplacedLight> lights; ///< game lights (kept or replaced), then attached lights
    std::vector<Hash64> deletedLights;
    ReplaceFrameStats stats;
    std::optional<ReloadEvent> reload; ///< a reload applied at this frame
    ResidencyStats residency;
};

struct ModInfo {
    ModLocation location;
    std::uint32_t rank = 0; ///< position in the stack (0 = strongest)
    bool ok = false;
    std::optional<std::int64_t> priority;
    std::string optionLayer; ///< the rtx.conf layer name ("" : none)
    std::size_t meshes = 0, materials = 0, lights = 0, deletedLights = 0;
    std::size_t errors = 0;
};

class ReplacementEngine {
public:
    explicit ReplacementEngine(EngineConfig config);
    ~ReplacementEngine();
    ReplacementEngine(const ReplacementEngine&) = delete;
    ReplacementEngine& operator=(const ReplacementEngine&) = delete;

    /// Hot reload (and the first load). Call before the frame's draws.
    void beginFrame(std::uint64_t frame);
    ReplacedDraw replaceDraw(const DrawInput& draw);
    /// `gameLights`: the frame's game lights (TranslatedFrame::lights).
    ReplacedFrame endFrame(const std::vector<scene::LightRecord>& gameLights);
    /// The light list endFrame(gameLights) would return now (game lights kept / replaced, deleted ones dropped, the
    /// lights attached by the draws looked up so far), without ending the frame or counting stats. For the renderer's
    /// injection-time GPU-scene feed (RL-4.x).
    std::vector<ReplacedLight> previewLights(const std::vector<scene::LightRecord>& gameLights) const;

    /// Reloads every mod now (rediscovery included), as a notification at `frame` would.
    void reloadAll(std::uint64_t frame);

    const std::vector<ModInfo>& mods() const { return m_modInfo; }
    const ReplacementIndex& index() const { return m_index; }
    std::uint64_t generation() const { return m_generation; }
    const char* watchBackend() const;
    const TextureResidency& residency() const { return m_residency; }
    const EngineConfig& config() const { return m_config; }
    /// Diagnostics of the last (re)load, prefixed with the mod name.
    const std::vector<std::string>& diagnostics() const { return m_diagnostics; }

private:
    void loadInitial(std::uint64_t frame);
    void reload(std::uint64_t frame, bool rediscover, const std::vector<std::string>& dirtyDirs, std::uint64_t notifiedFrame);
    void rebuild(std::uint64_t frame, std::uint64_t notifiedFrame, ReloadEvent event, const ReplacementIndex* before);
    void applyOptionLayers();
    void rebuildWatcher();
    void rebuildCatalog();
    void debugWaitForNotification(std::uint64_t frame);
    void refreshSwitches();
    /// endFrame's game light step: false when a light_<H> deletes it; `replaced` tells a light_<H> replaced it.
    bool gameLight(const scene::LightRecord& rec, ReplacedLight& out, bool& replaced) const;

    EngineConfig m_config;
    bool m_loaded = false;
    std::uint64_t m_generation = 0;
    std::uint64_t m_frame = 0;
    std::vector<std::unique_ptr<ModContent>> m_contents; ///< loaded mods (discovery order)
    std::vector<ModInfo> m_modInfo;                      ///< stack order
    ReplacementIndex m_index;
    std::unique_ptr<DirectoryWatcher> m_watcher;
    std::vector<std::string> m_watchedModDirs;  ///< watcher index -> mod directory ("" for a root)
    std::vector<std::string> m_diagnostics;
    struct LayerHandles;
    std::unique_ptr<LayerHandles> m_layers;
    TextureResidency m_residency;
    std::optional<ReloadEvent> m_pendingReload; ///< reported by the next endFrame
    // The frame being built.
    bool m_meshesOn = true, m_materialsOn = true, m_lightsOn = true; ///< rtx.enableReplacement*, per frame
    ReplaceFrameStats m_stats;
    std::vector<ReplacedLight> m_attached;
    std::set<std::string> m_usedTextures;
    std::vector<std::size_t> m_debugChanged; ///< notifications seen while waiting (debugReloadWaitFrame)
};

} // namespace fuse::relight::replace
