#include <fuse/fx/fx_composer.hpp>

namespace fuse::fx {

namespace {

void enqueueImpactResiduals(const CastInstance& instance, ResidualEffectQueue& residuals) {
    if (!instance.spell) {
        return;
    }

    for (const EffectEntry& entry : instance.spell->entriesFor(SpellPhase::Impact)) {
        if (!hasCondition(entry.conditions, EffectCondition::ImpactedSomething)) {
            continue;
        }
        if (entry.timing.residueLifetime <= 0.f) {
            continue;
        }

        ResidualEntry residue;
        residue.kind = ResidualKind::Zodiac;
        residue.assetId = entry.effectTypeId;
        residue.duration = entry.timing.residueLifetime;
        residue.fadeDuration = entry.timing.residueFade;
        residuals.enqueue(residue);
    }
}

} // namespace

bool FxComposer::registerEffect(const EffectDescriptor& descriptor) {
    if (descriptor.id.empty()) {
        return false;
    }
    m_effects[descriptor.id] = descriptor;
    return true;
}

bool FxComposer::registerSpell(const SpellDescriptor& descriptor) {
    if (descriptor.id.empty()) {
        return false;
    }
    m_spells[descriptor.id] = descriptor;
    return true;
}

const EffectDescriptor* FxComposer::findEffect(const std::string& id) const {
    const auto it = m_effects.find(id);
    return (it != m_effects.end()) ? &it->second : nullptr;
}

const SpellDescriptor* FxComposer::findSpell(const std::string& id) const {
    const auto it = m_spells.find(id);
    return (it != m_spells.end()) ? &it->second : nullptr;
}

bool FxComposer::attach(const FxSocket& socket) {
    const EffectDescriptor* effect = findEffect(socket.effectId);
    if (!effect) {
        return false;
    }

    m_sockets.push_back(socket);
    ++m_attachments;
    m_effectTimeline.start(*effect, socket);
    return true;
}

bool FxComposer::beginCast(const std::string& spellId, const CastBinding& binding) {
    const SpellDescriptor* spell = findSpell(spellId);
    if (!spell) {
        return false;
    }

    m_castPipeline.setPhaseEnterHook([this](CastInstance& instance, SpellPhase phase) {
        if (phase == SpellPhase::Impact) {
            enqueueImpactResiduals(instance, m_residuals);
        }
    });

    m_castPipeline.beginCast(*spell, binding);
    return true;
}

void FxComposer::tick(const frame::FrameCtx& ctx) {
    const float dt = (ctx.dt > 0.f) ? ctx.dt : (1.f / 60.f);
    m_effectTimeline.tick(dt);
    m_castPipeline.tick(dt);
    m_residuals.tick(dt);
    ++m_tickCount;
}

} // namespace fuse::fx
