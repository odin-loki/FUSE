// FUSE Relight RL-5.7: Remix's local tone mapper as a Look node (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.6: "Remix's
// local tone mapper is ported as a Look node"; component row 31).
//
// Algorithm (Remix rtx_local_tone_mapping: exposure fusion [Mertens et al. 2007] over a luminance pyramid):
//   1. luminance L = BT.709 luminance of the scene-linear input (after exposure);
//   2. three exposure candidates of L: L / highlights, L, L x shadows, each mapped to a perceptual value
//      d = sqrt(L' / (1 + L')) (Reinhard, square-root gamma);
//   3. well-exposedness weights w = exp(-0.5 sigma^2 (d - 0.5 - offset)^2) (+1e-6), sigma = exposurePreferenceSigma;
//   4. Laplacian pyramids of the candidates blended by Gaussian pyramids of the normalised weights over `mip` levels
//      (2x2 box down, bilinear up, clamped edges), collapsed to the fused perceptual value F;
//   5. the node stays scene-referred: the fused value maps back through the inverse curve (Y = F^2, L_f = Y / (1 - Y))
//      and every pixel's colour is scaled by L_f / L, so the Look chain's global tone map (and grading) runs after it.
//
// Look placement: LookStage::PostUpscaleHdr, SceneHdr -> SceneHdr, before the chain's Exposure / ToneMap nodes
// (validateLocalToneMapPlacement). init() allocates every pyramid level; process() makes no heap allocation and is
// deterministic (fixed loop order, no threading).
#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/relight/render/post/post_config.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::render::post {

inline constexpr u32 kLocalToneMapMaxLevels = 9u;

/// The node's Look metadata.
inline constexpr renderer::look::LookStage kLocalToneMapStage = renderer::look::LookStage::PostUpscaleHdr;
inline constexpr renderer::look::LookDomain kLocalToneMapDomain = renderer::look::LookDomain::SceneHdr;

/// True when a local tone-map node placed in front of `graph` is legal: the graph validates, has a ToneMap node, and
/// no node before the ToneMap leaves the scene-referred domain (the local tone map runs first, on SceneHdr).
bool validateLocalToneMapPlacement(const renderer::look::LookEffectGraph& graph);

class LocalToneMapper {
public:
    bool init(u32 width, u32 height, const LocalToneMapConfig& config);
    bool ready() const { return m_width != 0u; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    u32 levels() const { return m_levels; }

    /// `in` -> `out` (width x height, scene-linear; may alias).
    void process(const math::Vec3* in, math::Vec3* out);

private:
    struct Level {
        u32 w = 0, h = 0;
        std::vector<float> d[3]; ///< Gaussian pyramid of each candidate
        std::vector<float> w3[3]; ///< Gaussian pyramid of each weight
        std::vector<float> fused; ///< collapsed fused value
    };
    void down(const std::vector<float>& src, u32 sw, u32 sh, std::vector<float>& dst, u32 dw, u32 dh) const;
    float up(const std::vector<float>& src, u32 sw, u32 sh, u32 x, u32 y) const;

    LocalToneMapConfig m_config{};
    u32 m_width = 0, m_height = 0, m_levels = 0;
    Level m_level[kLocalToneMapMaxLevels];
    std::vector<float> m_lum;
};

} // namespace fuse::relight::render::post
