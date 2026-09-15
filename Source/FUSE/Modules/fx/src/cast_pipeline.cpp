#include <fuse/fx/cast_pipeline.hpp>

namespace fuse::fx {

namespace {

CastState stateForPhase(SpellPhase phase) {
    switch (phase) {
    case SpellPhase::Casting:
        return CastState::Casting;
    case SpellPhase::Delivery:
    case SpellPhase::Launch:
        return CastState::Delivery;
    case SpellPhase::Linger:
    case SpellPhase::Impact:
        return CastState::Linger;
    default:
        return CastState::Inactive;
    }
}

SpellPhase nextPhase(SpellPhase phase) {
    switch (phase) {
    case SpellPhase::Casting:
        return SpellPhase::Launch;
    case SpellPhase::Launch:
        return SpellPhase::Delivery;
    case SpellPhase::Delivery:
        return SpellPhase::Impact;
    case SpellPhase::Impact:
        return SpellPhase::Linger;
    case SpellPhase::Linger:
        return SpellPhase::Count;
    default:
        return SpellPhase::Count;
    }
}

bool isTimedPhase(SpellPhase phase) {
    return phase == SpellPhase::Casting || phase == SpellPhase::Delivery || phase == SpellPhase::Linger;
}

} // namespace

void CastPipeline::beginCast(const SpellDescriptor& spell, const CastBinding& binding) {
    CastInstance instance;
    instance.spell = &spell;
    instance.binding = binding;
    instance.state = CastState::Casting;
    instance.phase = SpellPhase::Casting;
    instance.phaseElapsed = 0.f;
    instance.spellElapsed = 0.f;
    instance.finished = false;
    m_instances.push_back(instance);
}

void CastPipeline::tick(float dt) {
    for (CastInstance& instance : m_instances) {
        if (!instance.finished) {
            advanceInstance(instance, dt);
        }
    }
}

u32 CastPipeline::activeCount() const {
    u32 count = 0;
    for (const CastInstance& instance : m_instances) {
        if (!instance.finished) {
            ++count;
        }
    }
    return count;
}

void CastPipeline::advanceInstance(CastInstance& instance, float dt) {
    if (!instance.spell) {
        finishInstance(instance);
        return;
    }

    instance.spellElapsed += dt;
    instance.phaseElapsed += dt;

    if (!isTimedPhase(instance.phase)) {
        const SpellPhase next = nextPhase(instance.phase);
        if (next == SpellPhase::Count) {
            finishInstance(instance);
            return;
        }
        enterPhase(instance, next);
        return;
    }

    const float phaseDuration = instance.spell->durationFor(instance.phase);
    if (phaseDuration <= 0.f || instance.phaseElapsed >= phaseDuration) {
        const SpellPhase next = nextPhase(instance.phase);
        if (next == SpellPhase::Count) {
            finishInstance(instance);
            return;
        }
        enterPhase(instance, next);
    }
}

void CastPipeline::enterPhase(CastInstance& instance, SpellPhase phase) {
    instance.phase = phase;
    instance.phaseElapsed = 0.f;
    instance.state = stateForPhase(phase);

    if (!isTimedPhase(phase)) {
        advanceInstance(instance, 0.f);
    }
}

void CastPipeline::finishInstance(CastInstance& instance) {
    instance.state = CastState::Done;
    instance.finished = true;
    ++m_completedCount;
}

} // namespace fuse::fx
