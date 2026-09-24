#pragma once

// Vulkan function loading for fuse_rhi (WP-0.2, docs/unification/RENDERER-EXECUTION.md).
//
// fuse_rhi does not link the Vulkan loader. It loads it at run time through the vendored volk
// (Engine/lib/volk): every `vk*` name that fuse_rhi code and its consumers call is a volk
// function-pointer global (the `<vulkan/vulkan.h>` those targets see is a shim that includes the
// real header with VK_NO_PROTOTYPES and then volk.h; see cmake/FuseVolk.cmake).
//
// Dispatch policy, applied whenever an instance or device is created or destroyed:
//   * no instance yet       - global entry points only (vkCreateInstance, vkEnumerateInstance*),
//                             loaded when the process starts and again by `initialize()`.
//   * exactly one device    - device-level dispatch (`volkLoadDevice`): device and command-buffer
//                             calls go straight to the top layer / ICD, skipping the loader
//                             trampoline.
//   * zero or 2+ devices    - device entry points resolved through the instance
//                             (`volkLoadInstance`): the loader trampolines, which are valid for any
//                             device, so several live devices never call through another device's
//                             table.
// VulkanInstance and VulkanDevice register themselves; code that creates raw VkInstance/VkDevice
// handles should call the register/unregister functions below. Tables are process-wide globals
// (volk's), so (re)loads happen only at instance/device creation or destruction.
//
// This header has no Vulkan types, so stub builds compile it; there every call is a no-op and
// `initialize()` returns false.

#include <fuse/types.hpp>

namespace fuse::renderer::vkloader {

enum class DispatchMode : u8 {
    /// No loader library found (or stub build).
    Unavailable = 0,
    /// Loader library found; only global entry points are loaded.
    Global = 1,
    /// Instance and device entry points resolved through a VkInstance (loader trampolines).
    Instance = 2,
    /// Device entry points resolved through the single live VkDevice.
    Device = 3,
};

/// Loads the Vulkan loader library through volk (`volkInitialize`). Idempotent and thread-safe;
/// also runs once during static initialisation so global entry points are callable before any
/// fuse_rhi object exists. False when there is no loader (or in a stub build).
bool initialize();

/// Instance version the loader reports (`volkGetInstanceVersion`), 0 when unavailable.
u32 loaderInstanceVersion();

/// Registers a newly created VkInstance and loads its instance-level table.
void registerInstance(void* vkInstance);
/// Unregisters a VkInstance after vkDestroyInstance and reloads from another live instance, if any.
void unregisterInstance(void* vkInstance);
/// Registers a newly created VkDevice (created from `vkInstance`) and reloads the device table.
void registerDevice(void* vkDevice, void* vkInstance);
/// Unregisters a VkDevice after vkDestroyDevice and reloads the device table.
void unregisterDevice(void* vkDevice);

DispatchMode dispatchMode();
/// The device whose table is loaded when `dispatchMode() == Device`, otherwise null.
void* dispatchDevice();
/// The instance the instance-level table was loaded from, otherwise null.
void* dispatchInstance();
/// Number of table (re)loads since process start (instance/device registration changes).
u32 reloadCount();

/// Called, under the loader lock, after every table (re)load. Tests use it to wrap volk's
/// function-pointer globals (b5_vk_call_hooks.cpp); the hook must only read and assign those
/// globals. Setting a hook applies it once immediately when an instance table is loaded.
/// Pass nullptr to remove it (the globals are then reloaded, dropping any wrappers).
using ReloadHook = void (*)(void* user);
void setReloadHook(ReloadHook hook, void* user);

} // namespace fuse::renderer::vkloader
