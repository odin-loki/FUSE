#pragma once

#include <fuse/fx/fx_defs.hpp>
#include <fuse/fx/spell_descriptor.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <vector>

namespace fuse::fx {

struct CastBinding {
    Handle<Object> caster = Handle<Object>::invalid();
    Handle<Object> target = Handle<Object>::invalid();
};

/// One active spell cast — ore analogue: `afxMagicSpell` runtime instance.
struct CastInstance {
    const SpellDescriptor* spell = nullptr;
    CastBinding binding;
    CastState state = CastState::Inactive;
    SpellPhase phase = SpellPhase::Casting;
    float phaseElapsed = 0.f;
    float spellElapsed = 0.f;
    bool finished = false;
};

using CastPhaseEnterHook = std::function<void(CastInstance&, SpellPhase)>;

/// Game-thread cast state machine — ore analogue: `afxMagicSpell::change_state_*`.
class CastPipeline {
public:
    void setPhaseEnterHook(CastPhaseEnterHook hook) { m_phaseEnterHook = std::move(hook); }

    void beginCast(const SpellDescriptor& spell, const CastBinding& binding);
    void tick(float dt);

    u32 activeCount() const;
    u32 completedCount() const { return m_completedCount; }
    const std::vector<CastInstance>& instances() const { return m_instances; }

private:
    void advanceInstance(CastInstance& instance, float dt);
    void enterPhase(CastInstance& instance, SpellPhase phase);
    void finishInstance(CastInstance& instance);

    std::vector<CastInstance> m_instances;
    CastPhaseEnterHook m_phaseEnterHook;
    u32 m_completedCount = 0;
};

} // namespace fuse::fx
