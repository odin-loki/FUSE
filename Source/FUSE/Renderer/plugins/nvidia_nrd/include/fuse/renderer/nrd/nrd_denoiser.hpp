#pragma once

// WP-6.4b: NVIDIA Real-time Denoisers (NRD: SIGMA, REBLUR, RELAX) behind denoise::IDenoiser, through a runtime-loaded
// provider (fuse_nrd_plugin_abi.h). This adapter is MIT and contains no NRD code; NRD itself is RTX-SDK-licensed
// and only ever reaches a machine when the developer builds / installs the provider.
//
//   NrdPluginLoader::probe(project)        FUSE_ENABLE_NRD_PLUGIN=ON only; env FUSE_NRD_SDK_DIR / FUSE_NRD_PLUGIN_LIB
//   NrdPluginLoader::load(config)          any build (the CI mock gates drive this)
//   register_nrd_denoiser(registry, plugin) "nrd" into denoise::DenoiserRegistry with the methods the provider has
//
// Frame protocol (IDenoiser): configure({signal, method Sigma | Reblur | Relax, ...}) -> bindNativeFrame(frame: NRD-packed
// native textures + cameras + jitter) -> beginFrame(serial, {w, h, reset}) -> importInto(graph) -> addPasses(graph, refs, {})
// -> collectRetired(done). addPasses adds one pass "denoise.nrd" declaring ExternalRead on every bound input and
// ExternalWrite on every bound output (NRD records its own dispatches; the graph orders and transitions around it,
// no hand barriers); its callback calls dispatch(commandBuffer). The DenoiseGraphInputs buffers of the in-tree SVGF are
// not NRD's layout: the producer (or a pack pass, WP-6.4b Open) writes the NRD textures.
//
// Parameter conventions (FUSE -> NRD): motion_vector_scale = (-1, -1, 0) (FUSE UV motion is cur - prev, NRD wants
// prev = cur + mv * scale); matrices column-major column-vector as stored; camera_jitter = the FUSE render-pixel sample
// jitter; disocclusion_threshold = DenoiserSettings::disocclusion_depth; reset -> CLEAR_AND_RESTART.
//
// Steady-state frames make no heap allocation (fixed arrays; instances are created only on resize / method change).

#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/denoise/denoiser.hpp>
#include <fuse/renderer/nrd/fuse_nrd_plugin_abi.h>
#include <fuse/types.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace fuse::renderer::nrd {

enum class NrdPluginStatus : u8 {
    Available,
    DisabledAtBuild,   ///< FUSE_ENABLE_NRD_PLUGIN=OFF: automatic probing compiled out
    NotConfigured,     ///< no directory in project settings or FUSE_NRD_SDK_DIR
    LibraryNotFound,
    LoadFailed,
    EntryPointMissing, ///< no fuseNrdPluginGetApi export
    AbiMismatch,       ///< other ABI major, or truncated function table
    InitFailed,
    RuntimeMissing,    ///< provider loaded, NRD / NRI runtime libraries absent
    GraphicsApi,       ///< graphics API not supported by the provider
    NoDenoisers,       ///< initialised, but no denoiser usable
};

[[nodiscard]] std::string_view nrd_plugin_status_name(NrdPluginStatus s) noexcept;

struct NrdPluginConfig {
    std::string sdk_dir;      ///< env FUSE_NRD_SDK_DIR
    std::string library_name; ///< env FUSE_NRD_PLUGIN_LIB; default default_nrd_provider_library_name()
    std::string options;      ///< env FUSE_NRD_PLUGIN_OPTIONS
    FuseNrdGraphicsApi graphics_api = FUSE_NRD_API_NONE;
    u64 instance = 0, physical_device = 0, device = 0;
};

/// `project` wins field by field; empty fields come from the environment.
[[nodiscard]] NrdPluginConfig resolve_nrd_plugin_config(const NrdPluginConfig& project);
/// "fuse_nrdplugin_nri.dll" on Windows, "libfuse_nrdplugin_nri.so" elsewhere.
[[nodiscard]] std::string_view default_nrd_provider_library_name() noexcept;

class NrdPlugin {
public:
    ~NrdPlugin();
    NrdPlugin(const NrdPlugin&) = delete;
    NrdPlugin& operator=(const NrdPlugin&) = delete;

    [[nodiscard]] const FuseNrdApi& api() const noexcept { return m_api; }
    [[nodiscard]] FuseNrdContext* context() const noexcept { return m_ctx; }
    [[nodiscard]] const std::string& library_path() const noexcept { return m_path; }
    [[nodiscard]] std::string_view provider_name() const noexcept;
    [[nodiscard]] const FuseNrdDenoiserSupport& denoiser(FuseNrdDenoiser d) const noexcept;
    [[nodiscard]] bool supports(FuseNrdDenoiser d) const noexcept;
    [[nodiscard]] u32 denoiser_mask() const noexcept;
    /// Extra export of the provider library (tests: the mock's echo hook).
    [[nodiscard]] void* symbol(const char* name) const noexcept;

private:
    friend struct NrdPluginLoader;
    NrdPlugin() = default;
    void* m_lib = nullptr;          // fuse-lint-allow(ownership): OS library handle, closed in ~NrdPlugin
    FuseNrdApi m_api{};
    FuseNrdContext* m_ctx = nullptr; // fuse-lint-allow(ownership): provider-owned, released via m_api.shutdown
    std::array<FuseNrdDenoiserSupport, FUSE_NRD_DENOISER_COUNT> m_denoisers{};
    std::string m_path;
};

struct NrdLoadResult {
    NrdPluginStatus status = NrdPluginStatus::NotConfigured;
    std::unique_ptr<NrdPlugin> plugin; ///< non-null only when Available
    std::string detail;                ///< reason (path, dlerror, provider status)
};

struct NrdPluginLoader {
    [[nodiscard]] static NrdLoadResult load(const NrdPluginConfig& config);
    [[nodiscard]] static NrdLoadResult load_file(const std::string& library_path, const NrdPluginConfig& config);
    /// resolve_nrd_plugin_config + load; DisabledAtBuild without touching the file system when
    /// FUSE_ENABLE_NRD_PLUGIN=OFF.
    [[nodiscard]] static NrdLoadResult probe(const NrdPluginConfig& project);
    [[nodiscard]] static bool enabled_at_build() noexcept;
};

// ---- mapping ------------------------------------------------------------------------------------------------

/// (signal, method) -> NRD denoiser; FUSE_NRD_DENOISER_COUNT when the pair is not an NRD one.
[[nodiscard]] FuseNrdDenoiser to_nrd_denoiser(denoise::DenoiseSignal signal, denoise::DenoiserMethod method) noexcept;
/// The output slot a denoiser writes.
[[nodiscard]] FuseNrdSlot nrd_output_slot(FuseNrdDenoiser d) noexcept;

/// Native resources + camera of one frame (NRD-packed textures, see fuse_nrd_plugin_abi.h slot comments).
struct NrdNativeFrame {
    std::array<FuseNrdResource, FUSE_NRD_SLOT_COUNT> resources{};
    math::Mat4 view{}, projection{};           ///< this frame, unjittered projection
    math::Mat4 prev_view{}, prev_projection{};
    math::Vec2 jitter_px{};      ///< FUSE render-pixel sample jitter
    math::Vec2 prev_jitter_px{};
    f32 denoising_range = 1000.f;
    f32 time_delta_ms = 1000.f / 60.f;
    u32 rect_width = 0, rect_height = 0; ///< 0 = the frame extent

    void set(FuseNrdSlot slot, const FuseNrdResource& r) {
        if (slot < FUSE_NRD_SLOT_COUNT) {
            resources[slot] = r;
        }
    }
};

/// FUSE settings + frame -> NRD common settings (pure; the gates pin every field).
[[nodiscard]] FuseNrdCommonSettings map_common_settings(const NrdNativeFrame& frame, u32 width, u32 height, u32 frame_index,
                                                        bool reset, const denoise::DenoiserSettings& settings) noexcept;
[[nodiscard]] FuseNrdDenoiserSettings map_denoiser_settings(FuseNrdDenoiser d, const denoise::DenoiserSettings& settings) noexcept;

// ---- "nrd" ----------------------------------------------------------------------------------------------------

inline constexpr const char* kNrdDenoiserName = "nrd";

class NrdDenoiser final : public denoise::IDenoiser {
public:
    explicit NrdDenoiser(std::shared_ptr<NrdPlugin> plugin);
    ~NrdDenoiser() override;
    NrdDenoiser(const NrdDenoiser&) = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    const denoise::DenoiserCaps& caps() const override { return m_caps; }
    bool configure(const denoise::DenoiserSettings& settings) override;
    const denoise::DenoiserSettings& settings() const override { return m_settings; }
    void reset() override { m_pendingReset = true; }
    /// Needs bindNativeFrame() first; creates / recreates the NRD instance on resize or method change.
    bool beginFrame(u64 frameSerial, const denoise::DenoiseFrameDesc& frame) override;
    /// Imports every bound native slot; refs.outputImage = the denoiser's output slot.
    denoise::DenoiseGraphRefs importInto(rg::Graph& graph) override;
    void addPasses(rg::Graph& graph, const denoise::DenoiseGraphRefs& refs, const denoise::DenoiseGraphInputs& inputs) override;
    u32 collectRetired(u64 completedSerial) override;

    /// Native textures + camera for the next beginFrame (consumed by dispatch).
    void bindNativeFrame(const NrdNativeFrame& frame);
    /// Records NRD into `commandBuffer` (the "denoise.nrd" callback; callable directly without a graph).
    FuseNrdStatus dispatch(u64 commandBuffer);

    [[nodiscard]] FuseNrdStatus last_status() const { return m_lastStatus; }
    [[nodiscard]] FuseNrdDenoiser active_denoiser() const { return m_denoiser; }
    [[nodiscard]] const FuseNrdCommonSettings& common_settings() const { return m_common; }
    [[nodiscard]] u32 instances_created() const { return m_instancesCreated; }
    [[nodiscard]] u32 dispatches() const { return m_dispatches; }
    [[nodiscard]] rg::TextureRef slot_ref(FuseNrdSlot s) const { return s < FUSE_NRD_SLOT_COUNT ? m_refs[s] : rg::TextureRef{}; }

private:
    static void record(const rg::PassContext& context, void* user);
    void retireInstance();

    std::shared_ptr<NrdPlugin> m_plugin;
    denoise::DenoiserCaps m_caps{};
    denoise::DenoiserSettings m_settings{};
    FuseNrdDenoiser m_denoiser = FUSE_NRD_DENOISER_COUNT;
    bool m_settingsDirty = true;
    NrdNativeFrame m_frame{};
    bool m_frameBound = false;
    bool m_frameBegun = false;
    bool m_pendingReset = true;
    u32 m_frameIndex = 0;
    u64 m_serial = 0;
    FuseNrdInstance* m_instance = nullptr; // fuse-lint-allow(ownership): provider-owned, destroy_instance
    FuseNrdDenoiser m_instanceDenoiser = FUSE_NRD_DENOISER_COUNT;
    u32 m_instanceW = 0, m_instanceH = 0;
    u64 m_instanceSerial = 0; ///< last frame serial that used m_instance (retire point)
    struct Retired {
        FuseNrdInstance* instance = nullptr; // fuse-lint-allow(ownership): destroyed in collectRetired
        u64 serial = 0;
    };
    std::array<Retired, 4> m_retired{};
    FuseNrdCommonSettings m_common{};
    std::array<FuseNrdResourceBinding, FUSE_NRD_SLOT_COUNT> m_bindings{};
    u32 m_bindingCount = 0;
    std::array<rg::TextureRef, FUSE_NRD_SLOT_COUNT> m_refs{};
    FuseNrdStatus m_lastStatus = FUSE_NRD_ERR_NOT_INITIALIZED;
    u32 m_instancesCreated = 0;
    u32 m_dispatches = 0;
};

/// Caps "nrd" for what `plugin` supports: Sigma (Shadow) when SIGMA_SHADOW is usable, Reblur / Relax (Reflection + Gi)
/// when both their diffuse and specular variants are.
[[nodiscard]] denoise::DenoiserCaps nrd_denoiser_caps(const NrdPlugin* plugin);

/// Registers "nrd" (factories bind to `plugin`, process-wide) when it supports any method; false otherwise.
bool register_nrd_denoiser(denoise::DenoiserRegistry& registry, std::shared_ptr<NrdPlugin> plugin);
void unregister_nrd_denoiser(denoise::DenoiserRegistry& registry);
[[nodiscard]] std::shared_ptr<NrdPlugin> registered_nrd_plugin();
/// Start-up convenience: probe + register into DenoiserRegistry::instance().
NrdLoadResult probe_and_register_nrd_denoiser(const NrdPluginConfig& project);

} // namespace fuse::renderer::nrd
