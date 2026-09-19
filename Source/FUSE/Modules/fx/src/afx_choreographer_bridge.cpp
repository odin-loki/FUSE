#include <fuse/fx/afx_choreographer_bridge.hpp>

#include <fuse/fx/fx_composer.hpp>

namespace fuse::fx {

bool AfxChoreographerBridge::bindSocket(FxComposer& composer, const ChoreographerBinding& binding) {
    if (!composer.attach(binding.socket)) {
        return false;
    }

    if (binding.beginCastOnAttach && !binding.spellId.empty()) {
        CastBinding castBinding;
        castBinding.caster = binding.socket.owner;
        castBinding.target = binding.socket.owner;
        if (!composer.beginCast(binding.spellId, castBinding)) {
            return false;
        }
        ++m_castCount;
    }

    m_bindings.push_back(binding);
    ++m_attachCount;
    return true;
}

void AfxChoreographerBridge::tick(FxComposer& composer, const frame::FrameCtx& ctx) {
    const float dt = (ctx.dt > 0.f) ? ctx.dt : (1.f / 60.f);
    composer.effectTimeline().tick(dt);
    composer.castPipeline().tick(dt);
    composer.residuals().tick(dt);
    composer.missiles().tick(dt);
    composer.particlePool().tick(ctx);
    composer.particlePoolGpu().syncFromCpu(composer.particlePool());
    composer.particlePoolGpu().tick(ctx);
    ++m_tickCount;
}

} // namespace fuse::fx
