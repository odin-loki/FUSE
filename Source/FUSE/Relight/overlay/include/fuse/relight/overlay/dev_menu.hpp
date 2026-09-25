// FUSE Relight RL-6.1: the developer menu (port of the Remix dev menu's user-facing parts, rtx_imgui /
// rtx_user_menu semantics, on FUSE's immediate-mode UI) and the overlay's input / visibility state machine.
//
// Tabs:
//   Stats     frame, frame time, frame mode and injection point, draws / scene draws / replaced draws (RL-3.4
//             replacement stats), instances, lights, GPU-scene instances, bindless images, texture swaps.
//   Options   every registered option (RL-0.6), filtered by a text field; bool rows toggle on click, int / float
//             rows step with - / +. Edits run as user edits (OptionLayerTarget User: UserSetting options go to the
//             user.conf layer, the others to the rtx.conf layer, as Remix routes them); Save writes both layers.
//   Textures  the textures the last frame sampled (RL-1.4 image hashes, in first-use order),
//             with one toggle per RL-1.2 texture category (sky, UI, ignore, decal, particle, terrain): a click adds
//             the hash to (or removes it from) the category's hash list in the rtx.conf layer; Save writes it.
//   Debug     full-screen debug views (albedo, normals, depth, motion, demodulated diffuse / specular) that the
//             frame renderer offers (relight.overlay.debugView).
//   Capture   the RL-1.8 capture: records relight.overlay.captureFrames frames from the next one.
//
// Widget ids (scripts): tab.stats tab.opts tab.tex tab.dbg tab.cap save opt.filter opt.<name> opt.<name>.dec
// opt.<name>.inc opt.up opt.down tex.<i>.<sky|ui|ignore|decal|particle|terrain> tex.up tex.down dbg.<view> cap.go
//
// OverlayCore owns the menu and the input front end (input.hpp): update() drains the window events of the frame;
// while hidden it only looks for the toggle and allocates nothing; while shown it runs one menu pass per event and
// a final pass, then rasterises the layer (raster.hpp).
#pragma once

#include <fuse/relight/overlay/input.hpp>
#include <fuse/relight/overlay/overlay_options.hpp>
#include <fuse/relight/overlay/script.hpp>
#include <fuse/relight/overlay/ui.hpp>
#include <fuse/relight/overlay/win32_hook.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fuse::relight::options {
class OptionBase;
}

namespace fuse::relight::overlay {

/// Same values as render::frame::DebugBuffer (0 = off).
enum class DebugView : std::int32_t { Off = 0, Albedo, Normal, Depth, Motion, DiffuseDemod, SpecularDemod, Count };
inline constexpr std::size_t kDebugViewCount = static_cast<std::size_t>(DebugView::Count);
const char* debugViewName(DebugView view); ///< "off", "albedo", "normal", "depth", "motion", "diffuse", "specular"

enum class Tab : std::uint8_t { Stats = 0, Options, Textures, Debug, Capture, Count };
const char* tabName(Tab tab); ///< "stats", "opts", "tex", "dbg", "cap"

/// What the Stats tab shows (the tap fills it from RenderTap's frame record).
struct MenuStats {
    std::uint64_t frame = 0;
    double frameMs = -1.0;        ///< < 0: not shown ("--"; relight.overlay.deterministic)
    const char* mode = "off";     ///< relight.frame.mode
    const char* inject = "none";  ///< ui / present / none / failed
    std::uint32_t injectDraw = 0;
    const char* pass = "none";
    std::uint32_t draws = 0, sceneDraws = 0, replaced = 0, instances = 0, lights = 0, gpuInstances = 0;
    std::uint32_t bindless = 0, swaps = 0;
    bool renderer = false;        ///< the FUSE renderer adopted the device
    std::string error;
};

struct MenuTexture {
    std::uint64_t hash = 0; ///< RL-1.4 image hash
    std::uint32_t width = 0, height = 0, format = 0; ///< D3DFORMAT
    std::uint32_t uses = 0; ///< draws that sampled it
};

struct CaptureStatus {
    bool requested = false; ///< waiting for the next frame
    bool active = false;
    std::uint32_t framesDone = 0, framesTotal = 0;
    bool written = false, ok = false;
    std::string dir;
    std::size_t meshes = 0, textures = 0, instances = 0;
    std::string error;
};

/// The per-frame inputs of the menu (pointers valid during OverlayCore::update).
struct MenuFrame {
    const MenuStats* stats = nullptr;
    const std::vector<MenuTexture>* textures = nullptr;
    const std::array<bool, kDebugViewCount>* debugAvailable = nullptr;
    const CaptureStatus* capture = nullptr;
    std::uint32_t captureFrames = 1;
};

/// One RL-1.2 texture category the Textures tab tags.
struct TextureCategory {
    const char* key;    ///< widget id suffix
    const char* label;  ///< button label
    const char* option; ///< hash-list option
};
inline constexpr TextureCategory kTextureCategories[] = {
    {"sky", "S", "rtx.skyBoxTextures"},       {"ui", "U", "rtx.uiTextures"},
    {"ignore", "I", "rtx.ignoreTextures"},    {"decal", "D", "rtx.decalTextures"},
    {"particle", "P", "rtx.particleTextures"}, {"terrain", "T", "rtx.terrainTextures"},
};

class DevMenu {
public:
    DevMenu();

    /// The panel for a back buffer (scale 0 = automatic).
    void setViewport(std::uint32_t frameW, std::uint32_t frameH, std::int32_t scaleOption);
    const Rect& panel() const { return m_panel; }
    std::int32_t scale() const { return m_scale; }

    /// One pass (event may be null: the final, drawn pass).
    void pass(const InputEvent* event, const MenuFrame& frame);

    const Ui& ui() const { return m_ui; }
    Tab tab() const { return m_tab; }
    void setTab(Tab tab) { m_tab = tab; }

    /// A Capture click since the last call (cleared by the call).
    bool takeCaptureRequest();

    // Counters (stats record, tests).
    std::uint32_t edits() const { return m_edits; }
    std::uint32_t tags() const { return m_tags; }
    std::uint32_t saves() const { return m_saves; }
    std::uint32_t saveFailures() const { return m_saveFailures; }
    const std::string& status() const { return m_status; }

    // ---- the actions behind the widgets (also used by tests) ------------------------------------------------
    /// Writes `value` (config text) to the option's user-edit layer and resolves it. False when unknown / no layer.
    static bool editOption(options::OptionBase& option, const std::string& value);
    /// Toggles `hash` in the category's hash list (rtx.conf layer). False when the option is missing.
    static bool toggleTextureCategory(const TextureCategory& category, std::uint64_t hash);
    /// The category's list resolves to contain `hash`.
    static bool textureInCategory(const TextureCategory& category, std::uint64_t hash);
    /// Saves the rtx.conf layer and the user.conf layer when they have unsaved changes. False on an I/O error.
    static bool saveLayers(std::string* what = nullptr);

private:
    void header();
    void statsTab(const MenuFrame& f);
    void optionsTab();
    void texturesTab(const MenuFrame& f);
    void debugTab(const MenuFrame& f);
    void captureTab(const MenuFrame& f);
    void refreshOptionList();

    Ui m_ui;
    Rect m_panel;
    std::int32_t m_scale = 1;
    Tab m_tab = Tab::Stats;
    std::string m_filter;
    std::string m_filterBuilt = "\x01"; ///< the filter m_options was built for
    std::vector<options::OptionBase*> m_options;
    std::int32_t m_optionScroll = 0, m_textureScroll = 0;
    bool m_captureRequest = false;
    std::uint32_t m_edits = 0, m_tags = 0, m_saves = 0, m_saveFailures = 0;
    std::string m_status;
};

class OverlayCore {
public:
    static constexpr std::size_t kMaxEventsPerFrame = InputHookCore::kQueueCapacity;

    explicit OverlayCore(const OverlayConfig& config);

    InputHookCore& input() { return m_input; }
    DevMenu& menu() { return m_menu; }
    const DevMenu& menu() const { return m_menu; }
    bool visible() const { return m_visible; }
    void setVisible(bool visible);

    /// Drains the queued events: toggles flip visibility; while shown every event runs a menu pass. `finalPass`:
    /// also run the final (event-less) pass when shown (its draw list is what draw() rasterises). Returns the events
    /// drained. Hidden: no allocation.
    std::size_t update(std::uint32_t frameW, std::uint32_t frameH, const MenuFrame& frame, bool finalPass);
    /// Rasterises the last pass into the layer (panel-sized, raster.hpp).
    void draw();
    const std::vector<Color>& layer() const { return m_layer; }

    /// Runs the script's commands of `frame` (script.hpp): each becomes window messages sent through `hook` (straight
    /// to the input core without a hooked window), then update(). Returns the commands run; `error` gets the first
    /// failure (a widget that was not laid out).
    std::size_t runScript(const Script& script, std::uint64_t frame, WindowHook& hook, std::uint32_t frameW,
                          std::uint32_t frameH, const MenuFrame& menuFrame, std::string* error);

    std::uint64_t events() const { return m_eventsTotal; }
    std::uint32_t togglesApplied() const { return m_togglesApplied; }

private:
    OverlayConfig m_config;
    InputHookCore m_input;
    DevMenu m_menu;
    bool m_visible = false;
    std::array<InputEvent, kMaxEventsPerFrame> m_events{};
    std::vector<Color> m_layer;
    std::uint64_t m_eventsTotal = 0;
    std::uint32_t m_togglesApplied = 0;
};

} // namespace fuse::relight::overlay
