// FUSE Relight RL-4.1: what the host of a D3D9 device offers FUSE's frame orchestration
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3: injection, composite, timeline sync, texture swap).
//
// The host is the patched DXVK d3d9 front end (Source/FUSE/Relight/tap/dxvk/fuse_tap_dxvk.cpp, one
// per device) or a test harness. It reaches the tap as DeviceEvent::host (onDeviceCreate and
// onDeviceReset); the pointer stays valid until onDeviceDestroy returns, and it is only called on the
// thread that delivers tap events, with the device lock held (i.e. from inside a tap event).
//
// Shared device. FUSE records its own command buffers on the host's VkDevice and submits them to the
// host's graphics queue (VulkanDevice::queue). Two timeline semaphores order the two streams:
//   acquire   signalled by the host: flushAndSignal(v) records "signal acquire = v" after everything
//             recorded so far, submits it, and returns once the submission reached the queue;
//   release   signalled by FUSE (its submission of the frame); composite(image, v) makes the host's
//             next work wait for release >= v before it reads `image`.
// FUSE submits only between lockQueue() and unlockQueue(), after flushAndSignal: its batch then follows
// the host's signal on the queue, so a wait-before-signal on one queue (a deadlock) cannot happen.
//
// FUSE images. Images FUSE creates on the host's device and hands over with importImage (composite
// sources, FUSE's own render targets) or setTextureSwap (the image the host samples instead of a game
// texture). Contract for importImage: optimal tiling, exclusive sharing, usage with TRANSFER_SRC and
// TRANSFER_DST, VK_IMAGE_LAYOUT_GENERAL whenever the other side may use it (FUSE transitions a new
// image before importing it and leaves every submission with it in GENERAL; the host does the same at
// the end of each of its submissions). Swap images are the host's to write (it copies the game texture
// into them when that changes, passthrough) and to lay out: FUSE creates them from textureInfo() and
// never records commands on them. FUSE destroys an image only after releaseImage / setTextureSwap(id,
// nullptr) and once the acquire value signalled after the release completed (or after waitIdle).
//
// Plain C++17, raw Vulkan handle values: compiled into the DXVK d3d9.dll and Relight's modules.
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdint>

namespace fuse::relight::tap {

/// A host image as FUSE may create a twin of it (the VkImageCreateInfo parameters that matter).
struct HostImageInfo {
    std::uint64_t vkImage = 0;
    std::uint32_t imageType = 1;   ///< VkImageType (1 = 2D)
    std::uint32_t format = 0;      ///< VkFormat
    std::uint32_t flags = 0;       ///< VkImageCreateFlags (e.g. MUTABLE_FORMAT, CUBE_COMPATIBLE)
    std::uint32_t usage = 0;       ///< VkImageUsageFlags of the host image
    std::uint32_t width = 0, height = 0, depth = 1;
    std::uint32_t mipLevels = 1, arrayLayers = 1;
    std::uint32_t samples = 1;     ///< VkSampleCountFlagBits
    std::uint32_t aspects = 1;     ///< VkImageAspectFlags of the format (1 = colour)
    /// VkImageFormatListCreateInfo entries (views the host creates with other formats, e.g. sRGB).
    std::uint32_t viewFormatCount = 0;
    std::uint32_t viewFormats[4] = {};
    /// VkImageLayout the host keeps the image in between its commands (0 = unknown). FUSE work that samples a
    /// host image in another layout transitions it and restores this layout before its submission ends.
    std::uint32_t layout = 0;
};

/// How the host created its VkInstance / VkDevice (RL-4.1: the renderer adopts the device, VulkanDevice::adopt).
/// Pointers are the host's storage, valid until onDeviceDestroy returns; FUSE copies what it keeps.
struct HostDeviceInfo {
    std::uint64_t getInstanceProcAddr = 0; ///< PFN_vkGetInstanceProcAddr of the loader the device came from
    std::uint64_t instance = 0, physicalDevice = 0, device = 0, queue = 0;
    std::uint32_t queueFamily = 0;         ///< the graphics queue's family (queue index 0)
    std::uint32_t instanceApiVersion = 0;  ///< VkApplicationInfo::apiVersion
    const char* const* enabledExtensions = nullptr;
    std::uint32_t enabledExtensionCount = 0;
    const char* const* instanceExtensions = nullptr;
    std::uint32_t instanceExtensionCount = 0;
    /// VkDeviceCreateInfo::pNext as passed to vkCreateDevice (VkPhysicalDeviceFeatures2 + chained structs) and
    /// VkDeviceCreateInfo::pEnabledFeatures (null when the chain carries them).
    const void* enabledFeatureChain = nullptr;
    const void* enabledCoreFeatures = nullptr;
};

/// An image FUSE created on the host's device, handed to the host.
struct FuseImage {
    std::uint64_t vkImage = 0;
    HostImageInfo info;            ///< how FUSE created it (info.vkImage == vkImage)
};

using HostImageHandle = std::uint32_t; ///< 0 = none

class IFrameHost {
public:
    virtual ~IFrameHost() = default;

    /// PFN_vkGetInstanceProcAddr of the host's loader, as an integer.
    virtual std::uint64_t getInstanceProcAddr() const = 0;
    virtual VulkanDevice vulkan() const = 0;
    /// The creation parameters of the host's device (extensions, feature chain, queue), when the host knows
    /// them: the DXVK host does for the device FUSE's bootstrap created (RL-1.1 import). False otherwise; FUSE
    /// then keeps its own dispatch and the CPU bindless heap.
    virtual bool deviceCreateInfo(HostDeviceInfo& out) const {
        (void)out;
        return false;
    }
    /// VkSemaphore values of the two timeline semaphores (created on first call).
    virtual std::uint64_t acquireSemaphore() = 0;
    virtual std::uint64_t releaseSemaphore() = 0;

    /// The current back buffer (implicit swap chain, buffer 0). False when there is none.
    virtual bool backBufferInfo(HostImageInfo& out) const = 0;
    /// A live texture's image. False when unknown or without an image.
    virtual bool textureInfo(ResourceId texture, HostImageInfo& out) const = 0;

    /// Registers a FUSE image with the host (see the contract above). 0 on failure.
    virtual HostImageHandle importImage(const FuseImage& image) = 0;
    /// Drops the host's reference; the host stops using it with the work recorded next.
    virtual void releaseImage(HostImageHandle image) = 0;

    // ---- recorded into the host's command stream at the current point ------------------------------
    /// Copies the current back buffer into `dst` (same extent and format).
    virtual bool copyBackBuffer(HostImageHandle dst) = 0;
    /// "Signal acquire = value" after everything recorded so far; flushes and waits until the host's
    /// submissions reached the queue.
    virtual bool flushAndSignal(std::uint64_t acquireValue) = 0;
    /// Waits for release >= value, then copies `src` over the current back buffer (same extent and
    /// format). The host's following work (e.g. the UI draws) lands on top.
    virtual bool composite(HostImageHandle src, std::uint64_t releaseValue) = 0;
    /// From the next draw on, sampling `texture` reads `image` (a FUSE image made from textureInfo), and
    /// the host copies the texture's content into it whenever the texture changes (passthrough swap).
    /// nullptr ends the swap. False when the texture cannot be swapped.
    virtual bool setTextureSwap(ResourceId texture, const FuseImage* image) = 0;

    /// Queue access for FUSE's submissions (the host's submission lock).
    virtual void lockQueue() = 0;
    virtual void unlockQueue() = 0;
    /// Waits until the host's recorded work completed on the GPU. False when the host cannot wait (process
    /// teardown): FUSE then abandons its Vulkan objects instead of destroying them.
    virtual bool waitIdle() = 0;
};

} // namespace fuse::relight::tap
