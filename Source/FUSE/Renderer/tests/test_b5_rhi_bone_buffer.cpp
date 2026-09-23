// B7 gate row: "Animator component updates pose and uploads bone buffer every frame without memory
// leak" — the GPU half (the CPU pose/palette half is fuse_b7_animation_gates).
//
// A 32-bone chain plays a looping clip through fuse::animation::Animator for 240 frames on the
// real triple-buffered FrameManager ring. Each frame:
//   Animator::tick -> BoneBuffer::upload(bone_palette) into the frame slot's GPU buffer ->
//   the slot command buffer copies that GPU buffer into a GpuToCpu readback buffer ->
//   submitGraphicsQueue (fence) -> endFrame.
// When a slot comes round again (beginFrame waited its fence) its readback must equal, bit for
// bit, the palette uploaded three frames earlier — so the GPU saw every frame's palette and no
// in-flight palette was overwritten. After warm-up the GPU allocator performs zero allocations,
// live buffer counts/bytes stay flat and the Animator's palette storage is never reallocated.
#include "b5_rhi_test_common.hpp"

#include <fuse/animation/animator.hpp>
#include <fuse/core/init.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/skinning/bone_buffer.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

using b5rhi::expectTrue;
using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace anim = fuse::animation;
using namespace fuse::renderer;

constexpr u32 kBones = 32;
constexpr u32 kFrames = 240;
constexpr u32 kWarmup = kFramesInFlight;

[[maybe_unused]] anim::Skeleton makeChain() {
    anim::Skeleton skel;
    skel.bone_count = kBones;
    skel.bones.resize(kBones);
    for (u32 i = 0; i < kBones; ++i) {
        std::snprintf(skel.bones[i].name, sizeof(skel.bones[i].name), "bone%u", i);
        skel.bones[i].parent_index = i == 0 ? -1 : static_cast<fuse::s32>(i - 1);
        skel.bones[i].local_transform = anim::mat4_from_trs({0.f, i == 0 ? 0.f : 0.25f, 0.f, 0.f}, anim::quat{},
                                                             {1.f, 1.f, 1.f, 0.f});
    }
    for (u32 i = 0; i < kBones; ++i) {
        skel.bones[i].inverse_bind = anim::mat4_inverse_affine(skel.compute_world_transform(i));
    }
    return skel;
}

anim::quat axisAngleZ(f32 radians) {
    return {0.f, 0.f, std::sin(radians * 0.5f), std::cos(radians * 0.5f)};
}

[[maybe_unused]] anim::AnimationClip makeClip() {
    anim::AnimationClip clip{};
    clip.duration = 2.f;
    clip.looping = true;
    for (u32 bone = 0; bone < kBones; ++bone) {
        anim::AnimationClip::BoneChannels channel{};
        channel.bone_index = bone;
        channel.rotation.times = {0.f, 1.f, 2.f};
        const f32 swing = 0.05f + 0.01f * static_cast<f32>(bone);
        channel.rotation.values_quat = {axisAngleZ(-swing), axisAngleZ(swing), axisAngleZ(-swing)};
        clip.bone_channels.push_back(channel);
    }
    return clip;
}

} // namespace

int main() {
#if !defined(FUSE_VULKAN_BACKEND)
    return b5rhi::skip("fuse_b5_rhi_bone_buffer", "Vulkan backend disabled");
#else
    fuse::core::initialize();
    VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady || bootstrap->frameManager() == nullptr ||
        !bootstrap->frameManager()->isReady()) {
        bootstrap.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_bone_buffer", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }
    VulkanDevice& device = *bootstrap->device();
    FrameManager& frames = *bootstrap->frameManager();

    {
        BindlessDescriptors bindless;
        bindless.init(device);
        ResourceManager resources;
        expectTrue(resources.init(device, bindless), "resource manager ready");

        BoneBuffer bones;
        BoneBufferDesc boneDesc{};
        boneDesc.maxBones = 64;
        expectTrue(bones.init(resources, boneDesc), "bone buffer ring created");
        expectTrue(bones.stats().buffersCreated == BoneBuffer::kSlots, "one GPU bone buffer per frame slot");

        BufferHandle readback[kFramesInFlight];
        for (BufferHandle& handle : readback) {
            BufferDesc desc{};
            desc.size = kBones * sizeof(anim::mat4);
            desc.usage = BufferUsage::TransferDst;
            desc.memoryUsage = MemoryUsage::GpuToCpu;
            desc.name = "fuse.test.bone_readback";
            handle = resources.createBuffer(desc);
        }

        const anim::Skeleton skel = makeChain();
        const anim::AnimationClip clip = makeClip();
        anim::Animator animator;
        animator.state_machine = std::make_unique<anim::AnimStateMachine>();
        auto node = std::make_unique<anim::ClipNode>();
        node->clip = &clip;
        animator.state_machine->add_state("sway", std::move(node));

        fuse::frame::FrameCtx ctx{};
        ctx.dt = 1.f / 60.f;

        std::vector<anim::mat4> uploaded[kFramesInFlight];
        bool slotPending[kFramesInFlight] = {false, false, false};
        u32 verified = 0;
        u32 mismatches = 0;
        u32 unchangedFrames = 0;
        const anim::mat4* paletteStorage = nullptr;
        bool paletteStable = true;
        u64 allocsAfterWarmup = 0;
        usize bytesAfterWarmup = 0;
        u32 buffersAfterWarmup = 0;
        ResourceManager::LiveCounts liveAfterWarmup{};
        std::vector<anim::mat4> readbackPalette(kBones);

        auto verifySlot = [&](u32 slot) {
            if (!slotPending[slot]) {
                return;
            }
            const Buffer* rb = resources.getBuffer(readback[slot]);
            if (rb != nullptr && rb->mapped != nullptr) {
                std::memcpy(readbackPalette.data(), rb->mapped, kBones * sizeof(anim::mat4));
                if (std::memcmp(readbackPalette.data(), uploaded[slot].data(), kBones * sizeof(anim::mat4)) != 0) {
                    ++mismatches;
                }
                ++verified;
            }
            slotPending[slot] = false;
        };

        for (u32 frame = 0; frame < kFrames; ++frame) {
            frames.signalTickComplete();
            frames.beginFrame(frame); // waits this slot's fence: its previous copy has completed
            const u32 slot = frames.currentIndex();
            verifySlot(slot);

            animator.tick(skel, ctx);
            expectTrue(animator.bone_palette.size() == kBones, "Animator produced a palette for every bone");
            if (frame == 0) {
                paletteStorage = animator.bone_palette.data();
            } else {
                paletteStable = paletteStable && animator.bone_palette.data() == paletteStorage;
                if (std::memcmp(uploaded[(slot + kFramesInFlight - 1u) % kFramesInFlight].data(),
                                animator.bone_palette.data(), kBones * sizeof(anim::mat4)) == 0) {
                    ++unchangedFrames;
                }
            }
            expectTrue(bones.upload(slot, animator.bone_palette.data(), kBones), "palette uploaded");
            uploaded[slot] = animator.bone_palette;

            // GPU copy of this frame's bone buffer, recorded into the frame slot command buffer.
            if (!resetFrameSlotCommandPool(device, frames)) {
                expectTrue(false, "slot command pool reset");
                break;
            }
            auto cmd = static_cast<VkCommandBuffer>(frames.currentCommandBuffer());
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &beginInfo);
            const Buffer* src = resources.getBuffer(bones.buffer(slot));
            const Buffer* dst = resources.getBuffer(readback[slot]);
            VkBufferCopy region{0, 0, kBones * sizeof(anim::mat4)};
            vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(src->handle), static_cast<VkBuffer>(dst->handle), 1, &region);
            VkMemoryBarrier hostRead{};
            hostRead.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0,
                                 nullptr, 0, nullptr);
            vkEndCommandBuffer(cmd);

            GraphicsQueueSubmitDesc submitDesc{};
            submitDesc.device = &device;
            submitDesc.frameManager = &frames;
            submitDesc.commandsAlreadyRecorded = true;
            const GraphicsQueueSubmitResult submit = submitGraphicsQueue(submitDesc);
            expectTrue(submit.ok && submit.submitted, "bone buffer frame submitted");
            slotPending[slot] = true;
            frames.endFrame();

            if (frame + 1u == kWarmup) {
                const GpuAllocStats* stats = resources.allocatorStats();
                allocsAfterWarmup = stats != nullptr ? stats->allocCount : 0u;
                bytesAfterWarmup = stats != nullptr ? stats->usedBytes : 0u;
                buffersAfterWarmup = stats != nullptr ? stats->bufferCount : 0u;
                liveAfterWarmup = resources.liveCounts();
            }
        }

        device.waitIdle();
        for (u32 slot = 0; slot < kFramesInFlight; ++slot) {
            verifySlot(slot);
        }

        const GpuAllocStats* stats = resources.allocatorStats();
        const ResourceManager::LiveCounts live = resources.liveCounts();
        std::printf("bone buffer: %u frames, %u GPU readbacks verified, %u mismatches, %llu uploads (%llu bytes), "
                    "allocs after warm-up %llu -> %llu, live buffers %u -> %u, GPU bytes %zu -> %zu\n",
                    kFrames, verified, mismatches, static_cast<unsigned long long>(bones.stats().uploads),
                    static_cast<unsigned long long>(bones.stats().bytesUploaded),
                    static_cast<unsigned long long>(allocsAfterWarmup),
                    static_cast<unsigned long long>(stats != nullptr ? stats->allocCount : 0u), liveAfterWarmup.buffers,
                    live.buffers, bytesAfterWarmup, stats != nullptr ? stats->usedBytes : static_cast<usize>(0));

        expectTrue(verified == kFrames, "every frame's bone buffer was read back from the GPU");
        expectTrue(mismatches == 0u, "GPU bone buffer == Animator palette for every frame");
        expectTrue(unchangedFrames == 0u, "palette changes every frame while the clip plays");
        expectTrue(bones.stats().uploads == kFrames && bones.stats().rejectedUploads == 0u, "one upload per frame");
        expectTrue(paletteStable, "Animator palette storage reused (no per-frame reallocation)");
        expectTrue(stats != nullptr && stats->allocCount == allocsAfterWarmup,
                   "zero GPU allocations after warm-up (no per-frame bone buffers)");
        expectTrue(stats != nullptr && stats->usedBytes == bytesAfterWarmup && stats->bufferCount == buffersAfterWarmup,
                   "GPU memory in use is flat across the run");
        expectTrue(live.buffers == liveAfterWarmup.buffers && live.textures == liveAfterWarmup.textures,
                   "live resource counts are flat across the run");
        expectTrue(!bones.upload(0, animator.bone_palette.data(), boneDesc.maxBones + 1u),
                   "oversized palette is rejected");

        const u32 buffersBeforeDestroy = resources.liveCounts().buffers;
        bones.destroy();
        for (BufferHandle handle : readback) {
            resources.destroyBuffer(handle);
        }
        expectTrue(resources.liveCounts().buffers + BoneBuffer::kSlots + kFramesInFlight == buffersBeforeDestroy,
                   "bone buffer ring + readbacks released on destroy");
        resources.destroy();
        bindless.destroy(device);
    }

    bootstrap.reset();
    fuse::core::shutdown();
    return b5rhi::finish("fuse_b5_rhi_bone_buffer");
#endif
}
