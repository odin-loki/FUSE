// FUSE Relight RL-4.1: FUSE's GPU work on the host's device (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3).
//
// FrameGpu owns FUSE's images (device-local memory, optimal tiling) and a small command-buffer ring on the
// host's graphics queue family. submitFrame() records FUSE's frame as a render graph v2 (renderer WP-0.3,
// rg::Graph: Vulkan-free compile + plan) over the imported images, records the planned synchronization2
// barriers and the passes, and submits it between IFrameHost::lockQueue / unlockQueue:
//
//   wait     acquire >= waitValue (ALL_COMMANDS: every host command recorded before flushAndSignal)
//   barrier  ALL_COMMANDS / MEMORY_WRITE -> first use (the host's copies into FUSE images, same queue)
//   passes   Solid: clear the output; Passthrough: copy input -> output (the frame graph RL-4.2+ extends)
//   barrier  the graph's final transitions (every image back to GENERAL) + MEMORY_WRITE -> ALL_COMMANDS
//   signal   release = signalValue (ALL_COMMANDS)
//
// Images handed to the host for importImage are transitioned to GENERAL at creation by a one-off
// submission (the host may copy into them before FUSE's first frame submission).
//
// No Vulkan header here: <vulkan/vulkan.h> brings <windows.h> into PE builds, whose macros (DrawState ->
// DrawStateA, ...) would rename the tap's types in every file that includes this one. The Vulkan state lives in
// FrameGpu::Impl (frame_gpu.cpp, vk_dispatch.hpp).
#pragma once

#include <fuse/relight/tap/frame_host.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace fuse::relight::render::frame {

struct GpuImage {
    tap::FuseImage image;               ///< vkImage + creation parameters
    std::uint64_t memory = 0;           ///< VkDeviceMemory
    tap::HostImageHandle host = 0;      ///< importImage handle (composite images), 0 otherwise
    bool valid() const { return image.vkImage != 0; }
};

enum class FramePass : std::uint8_t { Solid = 0, Passthrough = 1 };

struct FrameSubmitStats {
    bool ok = false;
    std::uint32_t passes = 0;
    std::uint32_t imageBarriers = 0; ///< planned by the render graph
    std::uint32_t barrierCalls = 0;  ///< vkCmdPipelineBarrier2 calls recorded (graph + the two globals)
    std::uint32_t graphBatches = 0;
};

class FrameGpu {
public:
    FrameGpu();
    ~FrameGpu();
    FrameGpu(const FrameGpu&) = delete;
    FrameGpu& operator=(const FrameGpu&) = delete;

    /// Loads the dispatch table from the host and creates the command ring. `host` must outlive this.
    bool init(tap::IFrameHost& host, std::string* error = nullptr);
    /// Waits for FUSE's own submissions and destroys the ring (images are destroyed by their owners first).
    void shutdown();
    bool ready() const;

    /// A device-local optimal-tiling image like `like` (type, format, flags, extent, mips, layers, view formats)
    /// with `usage`. `general`: transitioned UNDEFINED -> GENERAL now (images for importImage).
    bool createImage(const tap::HostImageInfo& like, std::uint32_t usage, bool general, GpuImage& out);
    void destroyImage(GpuImage& image);

    /// Records and submits FUSE's frame (see the header comment). `input` is required for Passthrough.
    /// `solidRgb` = 0xRRGGBB for Solid.
    FrameSubmitStats submitFrame(FramePass pass, const GpuImage* input, const GpuImage& output, std::uint32_t solidRgb,
                                 std::uint64_t waitAcquire, std::uint64_t signalRelease);

    /// The acquire semaphore's completed value (0 before the first signal or without a host).
    std::uint64_t acquireCompleted() const;
    /// Blocks until release >= value (bounded; false on timeout / error).
    bool waitRelease(std::uint64_t value, std::uint64_t timeoutNs = 5'000'000'000ull) const;
    /// Blocks until FUSE's own submissions completed.
    bool waitIdle();

    std::uint64_t submissions() const { return m_submissions; }
    const std::string& lastError() const { return m_error; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::uint64_t m_submissions = 0;
    std::string m_error;
};

} // namespace fuse::relight::render::frame
