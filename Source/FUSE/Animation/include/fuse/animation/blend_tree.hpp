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
};

struct ClipNode : BlendNode {
    const AnimationClip* clip = nullptr;
    f32 time = 0.f;
    f32 play_rate = 1.f;
    bool looping = true;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
};

struct BlendNode2 : BlendNode {
    std::unique_ptr<BlendNode> a;
    std::unique_ptr<BlendNode> b;
    f32* blend_param = nullptr;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
};

/// 1D blend space — interpolates clips along a single runtime parameter axis.
struct BlendSpace1D : BlendNode {
    struct Entry {
        f32 param_value = 0.f;
        std::unique_ptr<ClipNode> clip;
    };

    std::vector<Entry> entries;
    f32* param = nullptr;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
};

/// 2D blend space — distance-weighted clip blend in a 2D parameter plane (stub).
struct BlendSpace2D : BlendNode {
    struct Entry {
        vec2 param{};
        std::unique_ptr<ClipNode> clip;
    };

    std::vector<Entry> entries;
    vec2* param = nullptr;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
};

/// Layered blend — applies a masked upper-body layer over a base pose.
struct LayeredBlendNode : BlendNode {
    std::unique_ptr<BlendNode> base;
    std::unique_ptr<BlendNode> layer;
    std::vector<u32> masked_bones;
    f32 layer_weight = 1.f;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
};

struct AnimStateMachine : BlendNode {
    struct State {
        std::string name;
        std::unique_ptr<BlendNode> node;
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
    f32 blend_time = 0.f;
    f32 blend_duration = 0.2f;
    Pose blend_from_pose;

    void evaluate(f32 dt, const Skeleton& skel, Pose& out) override;
    void add_state(std::string name, std::unique_ptr<BlendNode> node);
    void add_transition(const char* from, const char* to, f32 duration, std::function<bool()> condition);
};

} // namespace fuse::animation
