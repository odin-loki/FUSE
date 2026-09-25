// FUSE Relight RL-5.4 research track: a small online-trained neural radiance cache (docs/plans/FUSE_REMIX_PORT_PLAN.md
// §5.4 "Neural cache (research, T3 HW)"). FUSE's own prototype after Mueller, Rousselle, Novak and Keller 2021,
// "Real-time Neural Radiance Caching for Path Tracing" (ACM TOG 40(4)), on the renderer's WP-9.1 neural runtime
// (fuse_neural: a multiresolution hash-grid encoding [Mueller et al. 2022, Instant-NGP] + a small MLP, the
// deterministic CPU trainer with Adam, portable GPU inference). No NVIDIA NRC SDK / tiny-cuda-nn code is used.
//
// What it shares with the hash grid (render/pathtrace/radiance_cache*): the training data (the same training paths
// and records, RadianceCacheCpu::train) and the termination decision (path spread vs the adaptive cell size); only the
// answer at a terminating vertex differs (RadianceCacheCpu::setLookup(nrc.lookup())). That makes the comparison of
// comparison of rl_nrc_report a comparison of the two function approximators at an equal training budget.
//
// Input: the vertex position offset by `normalOffset` along its facing normal (the WP-9.1 hash grid encodes at most
// three raw inputs; the offset separates the two sides of thin geometry), normalised into the scene bounds [0, 1]^3.
// Output: radiance (3), ReLU hidden layers, linear output clamped at 0 on query. Loss: MSE on the clamped one-sample
// targets (the minimiser is the conditional mean: no tone-mapping bias). Online: every frame's records train
// stepsPerFrame Adam steps (deterministic: fixed shuffle by hash).
#pragma once

#include <fuse/relight/render/pathtrace/radiance_cache.hpp>
#include <fuse/renderer/neural/neural_mlp.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::research {

struct NrcSettings {
    renderer::neural::NeuralNetConfig net{};    ///< defaults below (init() fills them when inputDims == 0)
    renderer::neural::NeuralAdamDesc adam{};
    u32 maxBatch = 1024;          ///< samples per step (records beyond are subsampled)
    u32 stepsPerFrame = 4;
    float boundsMin[3] = {-1.f, -1.f, -1.f};
    float boundsMax[3] = {1.f, 1.f, 1.f};
    float normalOffset = 0.02f;   ///< world units along the facing normal
    float maxRadiance = 32.f;     ///< target clamp (as the hash grid's)
    u64 seed = 1;

    /// The prototype's network: hash grid 3D, 12 levels x 2 features, 2^14 entries per level, base 4, scale 1.5;
    /// 2 hidden layers of 32 (ReLU); 3 linear outputs; fp32; Adam lr 1e-2.
    static NrcSettings defaults();
};

struct NrcStats {
    u32 frames = 0;
    u32 steps = 0;
    u32 samples = 0;   ///< training samples of the last frame
    float loss = 0.f;  ///< mean batch loss of the last frame's steps
};

class NeuralRadianceCache {
public:
    bool init(const NrcSettings& settings);
    bool valid() const { return m_net.valid(); }

    /// Online training on one frame's records (kRcRecordWords float4 each; invalid ones skipped). No heap allocation.
    bool trainFrame(const render::pathtrace::Word* records, u32 recordCount);
    /// The raw network input of a vertex (inputDims floats).
    void encodeInput(const float p[3], const float n[3], float* x) const;
    /// Query (thread-safe, no allocation): radiance at (p, n), clamped at 0. Always answers.
    void query(const float p[3], const float n[3], float out[3]) const;
    /// For RadianceCacheCpu::setLookup (valid while this object lives).
    render::pathtrace::RadianceCacheLookup lookup() const;

    const renderer::neural::NeuralNet& net() const { return m_net; }
    const NrcSettings& settings() const { return m_settings; }
    const NrcStats& stats() const { return m_stats; }

private:
    NrcSettings m_settings{};
    renderer::neural::NeuralNet m_net;
    renderer::neural::NeuralTrainer m_trainer;
    std::vector<float> m_inputs;
    std::vector<float> m_targets;
    std::vector<u32> m_order;
    float m_scale[3] = {1.f, 1.f, 1.f};
    NrcStats m_stats{};
};

} // namespace fuse::relight::research
