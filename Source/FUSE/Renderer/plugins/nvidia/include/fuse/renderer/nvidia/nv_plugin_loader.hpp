#pragma once

// Runtime loader for the optional NVIDIA provider (docs/nvidia-plugin.md).
//
// Never links NVIDIA code: the provider shared library is opened with dlopen / LoadLibraryExW from a
// directory the user configures (project setting, else env FUSE_NVIDIA_SDK_DIR). Every failure is a
// status, never a crash or exception: a machine without the SDK, the GPU or the driver simply reports
// "unavailable" and the NVIDIA backends are not registered.

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>
#include <fuse/types.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace fuse::renderer::nvidia {

/// Why the plugin is (un)available. Everything except Available means "no NVIDIA backends".
enum class PluginStatus : u8 {
    Available,
    DisabledAtBuild,   ///< FUSE_ENABLE_NVIDIA_PLUGIN=OFF: automatic probing compiled out
    NotConfigured,     ///< no SDK directory in project settings or FUSE_NVIDIA_SDK_DIR
    LibraryNotFound,   ///< directory set, provider library file absent
    LoadFailed,        ///< dlopen / LoadLibrary failed (wrong arch, missing dependency, ...)
    EntryPointMissing, ///< library has no fuseNvPluginGetApi export
    AbiMismatch,       ///< provider built for another FUSE_NV_PLUGIN_ABI_MAJOR, or truncated table
    InitFailed,        ///< provider init() failed for another reason
    RuntimeMissing,    ///< provider loaded but the NVIDIA runtime (interposer / NGX) is absent
    NoNvidiaGpu,       ///< provider reports no NVIDIA RTX adapter
    DriverTooOld,
    NoFeatures,        ///< initialised, but no feature is supported on this adapter
};

[[nodiscard]] std::string_view plugin_status_name(PluginStatus s) noexcept;

/// Where and how to load the provider. Project settings fill this; empty fields fall back to env.
struct PluginConfig {
    std::string sdk_dir;      ///< directory holding the provider + NVIDIA runtime; env FUSE_NVIDIA_SDK_DIR
    std::string library_name; ///< provider file name; env FUSE_NVIDIA_PLUGIN_LIB; default per platform
    std::string options;      ///< "k=v;k=v" passed to the provider; env FUSE_NVIDIA_PLUGIN_OPTIONS
    FuseNvGraphicsApi graphics_api = FUSE_NV_API_NONE;
    u32 application_id = 0;
    u64 instance = 0, physical_device = 0, device = 0; ///< native handles (VkInstance, ...)
};

/// `project` wins field by field; empty fields are taken from the environment.
[[nodiscard]] PluginConfig resolve_plugin_config(const PluginConfig& project);

/// Default provider file name for this platform ("fuse_nvplugin_streamline.dll" on Windows,
/// "libfuse_nvplugin_ngx.so" elsewhere).
[[nodiscard]] std::string_view default_provider_library_name() noexcept;

/// A loaded, initialised provider. Owns the library handle and the provider context; destruction
/// calls shutdown() then closes the library.
class NvPlugin {
public:
    ~NvPlugin();
    NvPlugin(const NvPlugin&) = delete;
    NvPlugin& operator=(const NvPlugin&) = delete;

    [[nodiscard]] const FuseNvApi& api() const noexcept { return api_; }
    [[nodiscard]] FuseNvContext* context() const noexcept { return ctx_; }
    [[nodiscard]] const FuseNvAdapterInfo& adapter() const noexcept { return adapter_; }
    [[nodiscard]] const std::string& library_path() const noexcept { return path_; }
    [[nodiscard]] std::string_view provider_name() const noexcept;

    /// Feature detection result from init time (FUSE_NV_OK = usable).
    [[nodiscard]] const FuseNvFeatureSupport& feature(FuseNvFeature f) const noexcept;
    [[nodiscard]] bool supports(FuseNvFeature f) const noexcept;
    /// FUSE_NV_FEATURE_BIT mask of supported features.
    [[nodiscard]] u32 feature_mask() const noexcept;

    /// Resolve an extra export from the provider library (tests use the mock's echo hook).
    [[nodiscard]] void* symbol(const char* name) const noexcept;

private:
    friend struct NvPluginLoader;
    NvPlugin() = default;
    void* lib_ = nullptr; // fuse-lint-allow(ownership): OS library handle, closed in ~NvPlugin
    FuseNvApi api_{};
    FuseNvContext* ctx_ = nullptr; // fuse-lint-allow(ownership): provider-owned, released via api_.shutdown
    FuseNvAdapterInfo adapter_{};
    std::array<FuseNvFeatureSupport, FUSE_NV_FEATURE_COUNT> features_{};
    std::string path_;
};

struct LoadResult {
    PluginStatus status = PluginStatus::NotConfigured;
    std::unique_ptr<NvPlugin> plugin; ///< non-null only when status == Available
    std::string detail;               ///< human-readable reason (path, dlerror, provider status)
};

struct NvPluginLoader {
    /// Load from an already-resolved config (no environment lookups). Always available, whatever
    /// FUSE_ENABLE_NVIDIA_PLUGIN says: this is what the CI mock gates drive.
    [[nodiscard]] static LoadResult load(const PluginConfig& config);
    /// Load from an explicit library path (directory + file), still no environment lookups.
    [[nodiscard]] static LoadResult load_file(const std::string& library_path, const PluginConfig& config);
    /// Engine start-up entry: resolve_plugin_config(project) then load(). Returns DisabledAtBuild
    /// without touching the file system when FUSE_ENABLE_NVIDIA_PLUGIN=OFF.
    [[nodiscard]] static LoadResult probe(const PluginConfig& project);
    /// True when this build was configured with FUSE_ENABLE_NVIDIA_PLUGIN=ON.
    [[nodiscard]] static bool enabled_at_build() noexcept;
};

} // namespace fuse::renderer::nvidia
