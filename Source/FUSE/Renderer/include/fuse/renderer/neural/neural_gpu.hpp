#pragma once

// WP-9.1 neural inference on the GPU: a NeuralNet's GPU blob (NeuralGpuHeader + parameters, neural_mlp.hpp) in
// host-visible ring slots (one per frame in flight, read through buffer device addresses) and the
// "neural.infer" compute pass on render graph v2.
//
//   gpu.beginFrame(serial, net);                 // copies the blob into this frame's slot only when that slot
//                                                // holds an older NeuralNet::version() (steady state: no copy)
//   NeuralGraphRefs refs = gpu.importInto(graph);
//   gpu.addInferPass(graph, refs, inputs, inOffset, inAddress, outputs, outOffset, outAddress, count);
//   gpu.collectRetired(completedSerial);
//
// Kernels:
//   portable   shaders/neural/nn_infer.{slang,comp}: one sample per invocation, fp32 arithmetic, fp16 parameters
//              widened from packed halves (no 16-bit storage / arithmetic features). Matches NeuralNet::infer
//              up to the GPU's sin / cos / exp (frequency encoding, sigmoid); the rest is the same IEEE ops.
//   coopmat    shaders/neural/nn_infer_coopmat.comp (GL_KHR_cooperative_matrix, 16x16x16 fp16 tiles, fp32
//              accumulators, 16 samples per 32-invocation subgroup). Built when glslang has the extension; used
//              only when the device reports VK_KHR_cooperative_matrix, the desc allows it and the network fits
//              (fp16 parameters, encoded and hidden widths multiples of 16). Not run on Lavapipe (no extension).
//
// Ring slots: frame serial % framesInFlight (the caller must not reuse a slot before the frame that last read
// it completed). A larger network reallocates the ring (the old buffer retires at the current serial).
// Steady-state frames make no heap allocation.

#include <fuse/renderer/neural/neural_mlp.hpp>
#include <fuse/renderer/neural/neural_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::neural {

enum class NeuralKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct NeuralCapabilities {
    bool gpu = false;                ///< portable kernel usable
    bool cooperativeMatrix = false;  ///< device has VK_KHR_cooperative_matrix and the coopmat kernel is built
    bool coopMatrixKernelBuilt = false;
    const char* reason = "no device"; ///< "ok" when gpu
};

NeuralCapabilities queryNeuralCapabilities(const VulkanDevice* device);

/// SPIR-V of the cooperative-matrix kernel (nullptr / 0 when glslang could not build it). For compile checks.
const u32* neuralCoopMatrixSpirv(usize* bytes);

struct NeuralGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    /// Bound for the pass (descriptor-buffer backend: pipelines carry its create flags); the kernels read only
    /// buffer device addresses.
    BindlessDescriptors* bindless = nullptr;
    NeuralKernelLanguage language = NeuralKernelLanguage::Auto;
    u32 framesInFlight = 3;
    u64 initialSlotBytes = 64u * 1024u;
    /// Build and use the cooperative-matrix kernel where the device and network allow it (off by default: the
    /// path has no hardware gate in CI yet, docs/unification/RENDERER-EXECUTION.md WP-9.1).
    bool allowCooperativeMatrix = false;
};

struct NeuralGraphRefs {
    rg::BufferRef net;        ///< the ring buffer
    rg::BufferRange range{};  ///< this frame's slot
    u64 header = 0;           ///< BDA of this frame's NeuralGpuHeader
    u64 params = 0;           ///< BDA of this frame's parameters
};

struct NeuralGpuStats {
    u32 uploads = 0;       ///< slot copies (cumulative)
    u64 uploadBytes = 0;   ///< cumulative
    u32 reallocations = 0;
    u32 retired = 0;
    u32 passes = 0;        ///< infer passes added this frame
    u32 coopPasses = 0;    ///< of which on the cooperative-matrix kernel
};

class NeuralGpu {
public:
    NeuralGpu() = default;
    ~NeuralGpu();
    NeuralGpu(const NeuralGpu&) = delete;
    NeuralGpu& operator=(const NeuralGpu&) = delete;

    /// False (nothing created) without a capable device or a built kernel of the requested language, or in the
    /// stub backend.
    bool init(const NeuralGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }

    bool beginFrame(u64 frameSerial, const NeuralNet& net);
    NeuralGraphRefs importInto(rg::Graph& graph);
    /// "neural.infer": count x inputDims floats at inputsAddress -> count x outputDims floats at outputsAddress.
    bool addInferPass(rg::Graph& graph, const NeuralGraphRefs& refs, rg::BufferRef inputs, u64 inputsOffset,
                      u64 inputsAddress, rg::BufferRef outputs, u64 outputsOffset, u64 outputsAddress, u32 count);
    u32 collectRetired(u64 completedSerial);

    const char* kernelLanguage() const { return m_language; }
    bool coopMatrixReady() const { return m_coopPipeline != nullptr; }
    const NeuralGpuStats& stats() const { return m_stats; }

private:
    struct Retired {
        Buffer buffer{};
        u64 serial = 0;
    };
    struct PassRecord {
        NeuralGpu* self = nullptr;
        NeuralPush push{};
        u32 groups = 1;
        bool coop = false;
    };
    static constexpr u32 kMaxPasses = 16u;
    static constexpr u32 kMaxSlots = 8u;

    bool createPipelines();
    bool ensureCapacity(u64 slotBytes);
    static void recordDispatch(const rg::PassContext& context, void* user);

    NeuralGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    Buffer m_ring{};
    u8 m_ringQueue = rg::kNoQueue;
    u64 m_slotStride = 0;
    u32 m_slot = 0;
    u64 m_slotVersion[kMaxSlots] = {};
    u64 m_slotBytes = 0;
    u32 m_inputDims = 0;
    u32 m_outputDims = 0;
    bool m_coopEligible = false;
    std::vector<Retired> m_retired;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout
    void* m_pipeline = nullptr;     ///< VkPipeline (portable)
    void* m_coopPipeline = nullptr; ///< VkPipeline (cooperative matrix), null unless allowed and supported
    NeuralGpuStats m_stats{};
};

} // namespace fuse::renderer::neural
