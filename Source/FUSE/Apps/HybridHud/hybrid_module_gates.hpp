#pragma once

#include <fuse/adventure/hud_prompt_interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/dimension/world_handle.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/mechanics/interactable.hpp>
#include <fuse/mechanics/registry.hpp>
#include <fuse/mechanics/trigger_zone.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <string>

namespace fuse::hybrid::gates {

constexpr int kFrameCount = 60;
constexpr float kDt = 1.f / 60.f;
constexpr fuse::cinematics::TimelineMs kTimelineDurationMs = 1'000;

/// Prestarter §10 U5 module gate wiring for demo_hybrid_hud + headless tests.
struct State {
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D hudSprite{"hud_sprite"};
    fuse::SceneObject3D agent3D{"agent_3d"};
    fuse::SceneObject3D lever3D{"lever_3d"};

    fuse::ai::BehaviorRuntime aiRuntime;
    fuse::cinematics::Timeline timeline;
    fuse::fx::FxComposer fxComposer;

    fuse::mechanics::MechanicsRegistry mechanicsRegistry;
    fuse::mechanics::InteractableComponent leverInteractable{"lever_interactable"};
    fuse::mechanics::TriggerZoneComponent leverTrigger;

    fuse::adventure::InteractionSystem adventureSystem;
    fuse::adventure::HudPromptInteractable hudPrompt{"Press E to activate lever"};

    std::string hudPromptText;
    bool agentInsideTrigger = false;
    float initialClearR = 0.f;
    float initialClearG = 0.f;
    float initialClearB = 0.f;
};

void setup(State& state, fuse::hybrid::HybridComposer& composer);
void tickFrame(State& state, fuse::hybrid::HybridComposer& composer, const fuse::frame::FrameCtx& ctx);

struct VerifyResult {
    bool ok = true;
    const char* message = nullptr;
};

VerifyResult verify(const State& state, const fuse::hybrid::HybridComposer& composer);

} // namespace fuse::hybrid::gates
