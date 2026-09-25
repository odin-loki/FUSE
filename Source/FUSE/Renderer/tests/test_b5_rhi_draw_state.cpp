// B3 gate row: "RenderDoc capture shows correct draw list ordering — sorted by material, no
// redundant state changes".
//
// Headless capture: b5_vk_call_hooks counts the vkCmdBindPipeline / vkCmdBindVertexBuffers /
// vkCmdBindIndexBuffer / vkCmdPushConstants / vkCmdDrawIndexed calls fuse_rhi actually records for
// a RhiContext::submitDrawList frame (what a RenderDoc event list would show).
//   * 12 draws over 4 materials pushed in scrambled order, then DrawList::sortByMaterial():
//     draws reach the GPU grouped by material in ascending order, stable within a material;
//     binds == unique pipelines (1), index/vertex binds == unique buffers (1),
//     push-constant (material) updates == unique materials (4).
//   * The same list unsorted: material updates == number of material runs — state is only
//     re-emitted when it really changes, and sorting is what minimises it.
#include "b5_rhi_test_common.hpp"
#include "b5_vk_call_hooks.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/rhi_context.hpp>

#include <algorithm>
#include <set>
#include <vector>

namespace {

using b5rhi::expectTrue;
using fuse::u32;

constexpr u32 kMaterials[] = {3, 1, 4, 2, 1, 3, 2, 4, 4, 1, 2, 3};

u32 materialRuns(const std::vector<u32>& sequence) {
    u32 runs = 0;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        if (i == 0 || sequence[i] != sequence[i - 1]) {
            ++runs;
        }
    }
    return runs;
}

} // namespace

int main() {
    if (!b5hooks::available()) {
        return b5rhi::skip("fuse_b5_rhi_draw_state", "Vulkan backend disabled");
    }
    fuse::core::initialize();

    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.raster.width = 64;
    desc.raster.height = 64;
    desc.enableCompositePass = false;
    desc.enableComputePipeline = false;
    auto context = fuse::renderer::RhiContext::create(desc);
    if (context == nullptr || !context->bootstrap().status().deviceReady) {
        context.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_draw_state", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }

    const u32 drawCount = static_cast<u32>(sizeof(kMaterials) / sizeof(kMaterials[0]));
    const std::set<u32> uniqueMaterials(std::begin(kMaterials), std::end(kMaterials));

    for (int pass = 0; pass < 2; ++pass) {
        const bool sorted = pass == 0;
        fuse::renderer::DrawList draws;
        for (u32 i = 0; i < drawCount; ++i) {
            fuse::renderer::DrawCall call{};
            call.indexCount = 3;
            call.materialId = kMaterials[i];
            call.firstIndex = 0;
            call.instanceCount = 1u + (i % 2u); // tags the draw so stable order is observable
            expectTrue(draws.push(call), "draw pushed");
        }
        if (sorted) {
            draws.sortByMaterial();
        }
        std::vector<u32> expectedOrder;
        for (u32 i = 0; i < draws.count(); ++i) {
            expectedOrder.push_back(draws.at(i).materialId);
        }
        if (sorted) {
            expectTrue(std::is_sorted(expectedOrder.begin(), expectedOrder.end()), "sortByMaterial orders by material");
            // Within a material the original push order is kept: the n-th draw of material m in
            // the sorted list is the n-th draw of m in kMaterials (checked via its instance tag).
            bool stable = true;
            u32 seenPerMaterial[8] = {};
            for (u32 i = 0; i < draws.count(); ++i) {
                const u32 material = draws.at(i).materialId;
                u32 nth = 0;
                u32 originalIndex = drawCount;
                for (u32 k = 0; k < drawCount; ++k) {
                    if (kMaterials[k] == material && nth++ == seenPerMaterial[material]) {
                        originalIndex = k;
                        break;
                    }
                }
                stable = stable && originalIndex < drawCount &&
                         draws.at(i).instanceCount == 1u + (originalIndex % 2u);
                ++seenPerMaterial[material];
            }
            expectTrue(stable, "sort is stable within a material");
        }

        b5hooks::resetCmdCounters();
        expectTrue(context->beginFrame(static_cast<u32>(pass)), "beginFrame");
        expectTrue(context->submitDrawList(draws, static_cast<u32>(pass)), "submitDrawList");
        const b5hooks::CmdCounters& c = b5hooks::cmdCounters();
        const std::set<std::uint64_t> pipelines(c.graphicsPipelines.begin(), c.graphicsPipelines.end());

        std::printf("%s: drawIndexed %u, pipelines bound %u (unique %zu), vb binds %u, ib binds %u, "
                    "push constants %u (materials %zu, runs %u), descriptor binds %u\n",
                    sorted ? "sorted" : "unsorted", c.drawIndexed, c.bindPipelineGraphics, pipelines.size(),
                    c.bindVertexBuffers, c.bindIndexBuffer, c.pushConstants, uniqueMaterials.size(),
                    materialRuns(expectedOrder), c.bindDescriptorSets);

        expectTrue(c.drawIndexed == drawCount, "every draw reached vkCmdDrawIndexed");
        expectTrue(c.drawIndexedMaterial == expectedOrder, "GPU draw order == draw list order (material per draw)");
        expectTrue(c.bindPipelineGraphics == pipelines.size() && pipelines.size() == 1u,
                   "pipeline binds == unique pipelines (1)");
        expectTrue(c.bindVertexBuffers == 1u && c.bindIndexBuffer == 1u,
                   "vertex/index buffer binds == unique buffers (1 each)");
        expectTrue(c.bindDescriptorSets <= 1u, "bindless set bound at most once");
        expectTrue(c.pushConstants == materialRuns(expectedOrder),
                   "material push-constant updates == material changes in draw order");
        if (sorted) {
            expectTrue(c.pushConstants == uniqueMaterials.size(),
                       "sorted list: material updates == unique materials");
            expectTrue(std::is_sorted(c.pushFirstWords.begin(), c.pushFirstWords.end()) &&
                           std::set<u32>(c.pushFirstWords.begin(), c.pushFirstWords.end()).size() ==
                               c.pushFirstWords.size(),
                       "sorted list: each material pushed once, ascending");
        } else {
            expectTrue(c.pushConstants > uniqueMaterials.size(), "unsorted list needs more material changes");
        }

        const fuse::renderer::CommandBufferRecorder& recorder = context->commandRecorder();
        expectTrue(recorder.vulkanPipelineBindCount() == c.bindPipelineGraphics &&
                       recorder.vulkanPushConstantCount() == c.pushConstants &&
                       recorder.vulkanVertexBufferBindCount() == c.bindVertexBuffers &&
                       recorder.vulkanIndexBufferBindCount() == c.bindIndexBuffer,
                   "recorder state-change counters match the captured vkCmd* calls");
    }

    context.reset();
    fuse::core::shutdown();
    return b5rhi::finish("fuse_b5_rhi_draw_state");
}
