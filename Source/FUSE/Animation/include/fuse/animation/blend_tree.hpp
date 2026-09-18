#pragma once

#include <fuse/animation/clip.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fuse::animation {

struct BlendNode {
    virtual ~BlendNode() = default;
    virtual void evaluate(f32 dt, const Skeleton& skel, Pose& out) = 0;
    virtual void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out);
};

struct ClipNode : BlendNode {
    const AnimationClip* clip = nullptr;
    f32 time = 0.f;
    f32 play_rate = 1.f;
    bool looping = true;

    [[nodiscard]] bool is_empty() const;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
};

struct BlendNode2 : BlendNode {
    std::unique_ptr<BlendNode> a;
    std::unique_ptr<BlendNode> b;
    f32* blend_param = nullptr;

    [[nodiscard]] bool is_empty() const;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
};

/// 1D blend space — interpolates clips along a single runtime parameter axis.
struct BlendSpace1D : BlendNode {
    struct Entry {
        f32 param_value = 0.f;
        std::unique_ptr<ClipNode> clip;
    };

    std::vector<Entry> entries;
    f32* param = nullptr;

    [[nodiscard]] bool is_empty() const;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
};

/// 2D blend space — distance-weighted clip blend in a 2D parameter plane.
struct BlendSpace2D : BlendNode {
    struct Entry {
        vec2 param{};
        std::unique_ptr<ClipNode> clip;
    };

    std::vector<Entry> entries;
    vec2* param = nullptr;

    [[nodiscard]] bool is_empty() const;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
};

/// Bracket indices and interpolation alpha for a 1D blend-space parameter sample.
struct BlendSpace1DSample {
    u32 lower_index = 0;
    u32 upper_index = 0;
    f32 alpha = 0.f;
};

/// Normalized per-entry weights for a 2D blend-space parameter sample.
struct BlendSpace2DSample {
    std::vector<f32> weights;
};

BlendSpace1DSample sample_blend_space_1d(const BlendSpace1D& space, f32 value);
BlendSpace2DSample sample_blend_space_2d(const BlendSpace2D& space, vec2 value);

/// Layered blend — applies a masked upper-body layer over a base pose.
struct LayeredBlendNode : BlendNode {
    std::unique_ptr<BlendNode> base;
    std::unique_ptr<BlendNode> layer;
    std::vector<u32> masked_bones;
    f32 layer_weight = 1.f;

    [[nodiscard]] bool is_empty() const;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
};

/// Additive layer — adds masked local TRS delta relative to bind pose over a base pose.
struct AdditiveBlendNode : BlendNode {
    std::unique_ptr<BlendNode> base;
    std::unique_ptr<BlendNode> layer;
    std::vector<u32> masked_bones;
    f32 layer_weight = 1.f;

    [[nodiscard]] bool is_empty() const;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
};

struct AnimStateMachine : BlendNode {
    struct State {
        std::string name;
        std::unique_ptr<BlendNode> node;
        std::function<void()> on_enter;
        std::function<void()> on_exit;
    };

    struct Transition {
        u32 from = 0;
        u32 to = 0;
        f32 blend_duration = 0.2f;
        std::function<bool()> condition;
    };

    std::vector<State> states;
    std::vector<Transition> transitions;
    u32 active_state = 0;
    u32 pending_state = 0;
    bool is_transitioning = false;
    f32 blend_time = 0.f;
    f32 blend_duration = 0.2f;
    Pose blend_from_pose;
    PoseSoA blend_from_pose_soa;
    bool has_entered_initial = false;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) override;
    void add_state(std::string name, std::unique_ptr<BlendNode> node);
    void add_transition(const char* from, const char* to, f32 duration, std::function<bool()> condition);

    [[nodiscard]] bool is_empty() const;

    /// Crossfade blend weight in [0, 1] while transitioning; 0 when idle.
    f32 crossfade_alpha() const;

    /// Reset to the first state without firing enter/exit callbacks.
    void reset();

    /// Lookup a state index by name; returns -1 when not found.
    s32 find_state_index(const char* name) const;

    /// Count registered transitions whose source state matches `from_state`.
    u32 outgoing_transition_count(u32 from_state) const;

    /// Count registered transitions whose destination state matches `to_state`.
    u32 incoming_transition_count(u32 to_state) const;

    /// True when a transition edge exists from `from_state` to `to_state`.
    bool has_transition(u32 from_state, u32 to_state) const;

    /// Blend duration for a registered edge, or -1 when the edge is missing.
    f32 transition_blend_duration(u32 from_state, u32 to_state) const;

    /// Name of the active state, or empty when the index is invalid.
    const char* state_name(u32 state_index) const;

    /// Name of the active state, or empty when idle/invalid.
    const char* active_state_name() const;

    /// Name of the pending crossfade target, or empty when not transitioning.
    const char* pending_state_name() const;

    /// Number of registered states.
    u32 state_count() const;

    /// True when `state_index` is within the registered state list.
    bool is_valid_state(u32 state_index) const;

    /// Number of registered transitions.
    u32 transition_count() const;

    /// True while a crossfade is in progress.
    bool has_pending_transition() const;

    /// Destination state for the `edge_index`-th outgoing transition from `from_state`, or -1.
    s32 outgoing_transition_to(u32 from_state, u32 edge_index) const;

    /// Source state for the `edge_index`-th incoming transition to `to_state`, or -1.
    s32 incoming_transition_from(u32 to_state, u32 edge_index) const;

    /// Blend duration for the `edge_index`-th outgoing transition from `from_state`, or -1.
    f32 outgoing_transition_blend_duration_at(u32 from_state, u32 edge_index) const;

    /// True when the state at `state_index` has a non-null blend node.
    bool has_state_node(u32 state_index) const;

    /// Index of the transition edge from `from_state` to `to_state`, or -1 when missing.
    s32 find_transition_index(u32 from_state, u32 to_state) const;

    /// True when a transition edge exists and its condition passes (or no condition is set).
    bool transition_condition_passes(u32 from_state, u32 to_state) const;

    /// True when a named transition edge exists and its condition passes.
    bool can_transition(const char* from, const char* to) const;

    /// Remaining crossfade time in seconds; 0 when idle or already complete.
    f32 remaining_crossfade_time() const;

    /// Global transition index of the first outgoing edge from `from_state` whose condition passes, or -1.
    s32 find_first_passing_outgoing_transition(u32 from_state) const;

    /// True when the `edge_index`-th outgoing transition from `from_state` has a passing condition.
    bool outgoing_transition_condition_passes(u32 from_state, u32 edge_index) const;

    /// Blend duration for the `edge_index`-th incoming transition to `to_state`, or -1.
    f32 incoming_transition_blend_duration_at(u32 to_state, u32 edge_index) const;

    /// Elapsed crossfade time in seconds; 0 when idle.
    f32 elapsed_crossfade_time() const;

    /// True when at least one outgoing edge from `from_state` has a passing condition.
    bool has_passing_outgoing_transition(u32 from_state) const;

    /// True when the `edge_index`-th incoming transition to `to_state` has a passing condition.
    bool incoming_transition_condition_passes(u32 to_state, u32 edge_index) const;

    /// True when `transition_index` is within the registered transition list.
    bool is_valid_transition_index(u32 transition_index) const;

    /// Source state for the `transition_index`-th registered edge, or -1 when invalid.
    s32 transition_from_at(u32 transition_index) const;

    /// Destination state for the `transition_index`-th registered edge, or -1 when invalid.
    s32 transition_to_at(u32 transition_index) const;

    /// Blend duration for the `transition_index`-th registered edge, or -1 when invalid.
    f32 transition_blend_duration_at(u32 transition_index) const;

    /// True when the `transition_index`-th registered edge has a passing condition.
    bool transition_condition_passes_at(u32 transition_index) const;

    /// True while crossfading and `pending_state` is a registered state index.
    bool is_valid_pending_state() const;
};

} // namespace fuse::animation
