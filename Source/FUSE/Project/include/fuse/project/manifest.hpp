#pragma once

#include <fuse/hybrid/project_flags.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::project {

/// Versioned FUSE project manifest (maps to `project.json`).
struct DimensionSettings {
    bool enable3D = true;
    bool enable2D = true;
    bool enableUI = true;
};

struct ModuleSettings {
    bool ai = false;
    bool cinematics = false;
    bool fx = false;
    bool mechanics = false;
    bool adventure = false;
};

/// Newest `project.json` schema (written for new projects). Schema 1 files still load; they are migrated in memory
/// (kind game, no UI app data) and written back as schema 1 until `migrateToLatest` bumps them.
inline constexpr u32 kProjectSchemaVersion = 2u;
inline constexpr u32 kProjectSchemaVersionMin = 1u;

/// Schema 2: what the project is. `Game` is today's behaviour (continuous frame, worlds, renderer). `App` is a FUSE UI
/// application: on-demand loop, no GpuScene / FrameComposer / worlds. `Hybrid` is an app whose viewport widgets may
/// create game worlds on demand.
enum class ProjectKind : u8 { Game, App, Hybrid };

/// Host features a project needs from the process that runs it. The kind implies some (`impliedHostFeatures`);
/// `requiredFeatures` in `project.json` adds the rest. Bit i = `1u << i`.
enum class HostFeature : u8 {
    AppPresent, ///< presents through fuse_app (on-demand loop, no game frame); TrackBHostFeature::AppPresent
    GameFrame,  ///< continuous game tick + FrameComposer
    Renderer3D, ///< GpuScene + 3D worlds
    Renderer2D, ///< 2D worlds
    Physics,
    Audio,
    Script,     ///< Lua
    Jobs,       ///< job scheduler workers
    Viewport,   ///< hybrid: game worlds created on demand by a viewport widget
    Count,
};
using HostFeatureSet = u32;

[[nodiscard]] constexpr HostFeatureSet hostFeatureBit(HostFeature feature) {
    return 1u << static_cast<u32>(feature);
}

enum class RedrawMode : u8 { OnDemand, Continuous };
enum class UiProfile : u8 { Hud, App, Editor };
enum class TextProfile : u8 { Minimal, Full, Cooked };

/// Schema 2 `ui`: the main document, style sheets and profiles.
struct UiSettings {
    std::string root;                ///< main `.fuseui`, relative to the project root
    std::vector<std::string> styles; ///< `.fusestyle` sheets applied in order
    std::string theme = "system";    ///< "system", "dark", "light" or a theme name
    RedrawMode redraw = RedrawMode::OnDemand;
    UiProfile profile = UiProfile::App;
    TextProfile text = TextProfile::Minimal;
};

/// Schema 2 `windows[]` entry. An empty `root` uses `ui.root`.
struct WindowSettings {
    std::string id = "main";
    std::string title;
    u32 width = 800;
    u32 height = 600;
    std::string root;
    RedrawMode redraw = RedrawMode::OnDemand;
};

struct ProjectManifest {
    /// Version of the file this came from (1 or 2); 0 for a manifest built in code.
    u32 schemaVersion = 0;
    std::string name;
    std::string projectRoot;
    DimensionSettings dimensions;
    ModuleSettings modules;
    std::string defaultWorld3D;
    std::string defaultWorld2D;
    /// Optional override for `jobs::computeWorkerCount()` (0 = platform default).
    u32 workerCap = 0;
    // schema 2
    ProjectKind kind = ProjectKind::Game;
    UiSettings ui;
    std::vector<WindowSettings> windows;
    HostFeatureSet requiredFeatures = 0; ///< declared in the file (implied ones are not stored)
};

enum class LoadStatus : u8 {
    Ok = 0,
    FileNotFound,
    ParseError,
    UnsupportedSchema,
    Invalid, ///< schema 2 parsed but failed validation (`error` names the field)
};

struct LoadResult {
    LoadStatus status = LoadStatus::ParseError;
    ProjectManifest manifest;
    std::string error;
};

hybrid::DimensionFlags toDimensionFlags(const DimensionSettings& settings);

/// UNI-U6-FILE-1: `project.json` text for `manifest` in the layout `parseManifest` reads back. A manifest with
/// `schemaVersion == 1` that holds nothing schema 1 cannot express is written in the schema 1 layout (byte-identical
/// to what schema 1 builds wrote); everything else is written as schema 2. `projectRoot` is not stored (it is the
/// file's directory). Strings are JSON-escaped (quotes, backslashes, control characters).
std::string writeManifestJson(const ProjectManifest& manifest);

/// True when the manifest uses something only schema 2 can store (kind, ui, windows, required features).
[[nodiscard]] bool needsSchema2(const ProjectManifest& manifest);

/// Bumps a manifest to the newest schema (the loader already filled the schema 2 fields in memory). True when the
/// version changed; `saveToFile` afterwards writes schema 2.
bool migrateToLatest(ProjectManifest& manifest);

/// Schema 2 rules: name, kind vs dimensions / features, window sizes and ids, file extensions. Empty when valid.
[[nodiscard]] std::string validateManifest(const ProjectManifest& manifest);

/// Features the kind and dimensions imply. Game: GameFrame, Jobs, Renderer3D + Physics (3D), Renderer2D (2D). App:
/// AppPresent. Hybrid: AppPresent + Viewport.
[[nodiscard]] HostFeatureSet impliedHostFeatures(const ProjectManifest& manifest);
/// implied | required.
[[nodiscard]] HostFeatureSet effectiveHostFeatures(const ProjectManifest& manifest);

/// Minimal init path: what a host brings up for this project before its first frame. App projects get the UI shell
/// and AppPresent (plus declared extras such as Jobs); no worlds, renderer or physics. Hybrid projects defer the
/// game side until a viewport asks for it.
struct InitPlan {
    HostFeatureSet startup = 0;  ///< brought up before the first frame
    HostFeatureSet deferred = 0; ///< created on demand (hybrid viewports)
    bool appShell = false;       ///< fuse_app Application + windows (app / hybrid)
    [[nodiscard]] bool has(HostFeature feature) const { return (startup & hostFeatureBit(feature)) != 0u; }
};
[[nodiscard]] InitPlan planInit(const ProjectManifest& manifest);

std::string_view toString(ProjectKind kind);
std::string_view toString(HostFeature feature);
std::string_view toString(RedrawMode mode);
std::string_view toString(UiProfile profile);
std::string_view toString(TextProfile profile);
/// "AppPresent,Jobs" (enum order); empty for none.
std::string hostFeatureList(HostFeatureSet features);
bool parseProjectKind(std::string_view text, ProjectKind& out);
bool parseHostFeature(std::string_view text, HostFeature& out);
bool parseRedrawMode(std::string_view text, RedrawMode& out);
bool parseUiProfile(std::string_view text, UiProfile& out);
bool parseTextProfile(std::string_view text, TextProfile& out);

/// New Project "Application (FUSE UI)" template: a schema 2 app manifest (one 800x600 window, ui/main.fuseui).
[[nodiscard]] ProjectManifest appTemplateManifest(std::string_view name);
/// The template's `ui/main.fuseui` (title label bound to nothing, a button with a "clicked" action).
[[nodiscard]] std::string_view appTemplateMainUi();

/// JSON string escaping / unescaping used by the manifest writer and loader.
std::string escapeJsonString(std::string_view text);
std::string unescapeJsonString(std::string_view text);

} // namespace fuse::project
