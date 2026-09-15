#include <fuse/fx/spell_descriptor.hpp>

namespace fuse::fx {

SpellDescriptor SpellDescriptor::makeFireball() {
    SpellDescriptor spell;
    spell.id = "fireball";
    spell.castingDuration = 0.5f;
    spell.deliveryDuration = 0.25f;
    spell.lingerDuration = 0.4f;

    EffectEntry castGlow;
    castGlow.effectTypeId = "gui_cast_bar";
    castGlow.timing.lifetime = spell.castingDuration;
    spell.phaseEntries[static_cast<std::size_t>(SpellPhase::Casting)].push_back(castGlow);

    EffectEntry launchFlash;
    launchFlash.effectTypeId = "light_flash";
    launchFlash.timing.lifetime = 0.1f;
    spell.phaseEntries[static_cast<std::size_t>(SpellPhase::Launch)].push_back(launchFlash);

    EffectEntry missileTrail;
    missileTrail.effectTypeId = "particle_emitter";
    missileTrail.timing.lifetime = spell.deliveryDuration;
    spell.phaseEntries[static_cast<std::size_t>(SpellPhase::Delivery)].push_back(missileTrail);

    EffectEntry impactBurst;
    impactBurst.effectTypeId = "explosion";
    impactBurst.timing.lifetime = 0.2f;
    impactBurst.timing.residueLifetime = 2.f;
    impactBurst.timing.residueFade = 1.f;
    impactBurst.conditions = EffectCondition::Enabled | EffectCondition::ImpactedSomething;
    spell.phaseEntries[static_cast<std::size_t>(SpellPhase::Impact)].push_back(impactBurst);

    EffectEntry lingerSmoke;
    lingerSmoke.effectTypeId = "volume_light";
    lingerSmoke.timing.lifetime = spell.lingerDuration;
    spell.phaseEntries[static_cast<std::size_t>(SpellPhase::Linger)].push_back(lingerSmoke);

    return spell;
}

} // namespace fuse::fx
