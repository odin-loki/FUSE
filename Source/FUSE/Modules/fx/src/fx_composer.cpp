#include <fuse/fx/fx_composer.hpp>

namespace fuse::fx {

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

void FxComposer::attach(const FxSocket& socket) {
    m_sockets.push_back(socket);
    ++m_attachments;
}

bool FxComposer::beginCast(const std::string& spellId, const CastBinding& binding) {
    const SpellDescriptor* spell = findSpell(spellId);
    if (!spell) {
        return false;
    }
    m_castPipeline.beginCast(*spell, binding);
    return true;
}

void FxComposer::tick(const frame::FrameCtx& ctx) {
    const float dt = (ctx.dt > 0.f) ? ctx.dt : (1.f / 60.f);
    m_castPipeline.tick(dt);
    m_residuals.tick(dt);
    ++m_tickCount;
}

} // namespace fuse::fx
