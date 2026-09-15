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
