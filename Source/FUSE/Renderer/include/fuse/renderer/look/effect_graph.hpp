#pragma once

// Look effect graph: an ordered list of effect nodes plus the validation that makes an order legal.
// The graph is data (a `.fuselook` may list its own `graph` order); `LookEffectGraph::validate`
// enforces the signal-domain chain and the ordering constraints below before the chain executes it.

#include <fuse/renderer/look/look_params.hpp>
#include <fuse/types.hpp>

#include <array>

namespace fuse::renderer::look {

enum class LookGraphError : u8 {
    None = 0,
    Empty,
    DuplicateNode,
    MissingToneMap,        ///< exactly one toneMap is required
    MissingOutputTransform,///< exactly one outputTransform is required
    OutputNotLast,         ///< outputTransform must be the final node
    DomainMismatch,        ///< e.g. an HDR-only node after toneMap, or a display node before it
    StageOrder,            ///< stages must be non-decreasing (PreUpscale < PostUpscaleHdr < Display < Output)
    LensDirtWithoutBloom,  ///< lensDirt modulates the bloom buffer: bloom must run earlier
    LensFlareWithoutBloom, ///< lensFlare reads the bloom bright-pass pyramid: bloom must run earlier
    GrainBeforeSharpen,    ///< grain after sharpening (never sharpen noise)
    ExposureAfterToneMap,  ///< exposure is scene-referred
};

const char* look_graph_error_name(LookGraphError error);

struct LookGraphValidation {
    LookGraphError error = LookGraphError::None;
    u32 node_index = 0; ///< offending position in the order
    bool ok() const { return error == LookGraphError::None; }
};

/// Fixed-capacity ordered node list (each effect at most once).
class LookEffectGraph {
public:
    /// The canonical order (research §4.2): AO, DoF, motion blur, bloom, lens dirt, lens flare,
    /// exposure, tonemap, grade (3D LUT), sharpen, chromatic aberration, vignette, grain, output.
    static LookEffectGraph makeDefault();

    void clear() { m_count = 0; }
    bool push(LookEffect effect);
    u32 size() const { return m_count; }
    LookEffect at(u32 i) const { return m_nodes[i]; }
    bool contains(LookEffect effect) const { return indexOf(effect) < m_count; }
    /// Position of `effect`, or size() when absent.
    u32 indexOf(LookEffect effect) const;

    LookGraphValidation validate() const;

    bool operator==(const LookEffectGraph& o) const {
        if (m_count != o.m_count) {
            return false;
        }
        for (u32 i = 0; i < m_count; ++i) {
            if (m_nodes[i] != o.m_nodes[i]) {
                return false;
            }
        }
        return true;
    }

private:
    std::array<LookEffect, kLookEffectCount * 2> m_nodes{}; ///< room for duplicates so validate() can reject them
    u32 m_count = 0;
};

} // namespace fuse::renderer::look
