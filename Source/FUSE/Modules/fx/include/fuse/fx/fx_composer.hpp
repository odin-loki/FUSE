#pragma once

#include <fuse/fx/cast_pipeline.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/fx_defs.hpp>
#include <fuse/fx/residual_effects.hpp>
#include <fuse/fx/spell_descriptor.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::fx {

/// FX attachment point — ore analogue: AFX effectron/choreographer sockets.
struct FxSocket {
    Handle<Object> owner = Handle<Object>::invalid();
    FxSocketKind kind = FxSocketKind::Shape3D;
    std::string effectId;
};

/// Descriptor registry + cast/residual tick facade.
/// Ore refs: `afxChoreographer`, `afxEffectron`, `afxMagicSpell`, `afxResidueMgr`.
class FxComposer {
public:
    bool registerEffect(const EffectDescriptor& descriptor);
    bool registerSpell(const SpellDescriptor& descriptor);

    const EffectDescriptor* findEffect(const std::string& id) const;
    const SpellDescriptor* findSpell(const std::string& id) const;

    u32 effectCount() const { return static_cast<u32>(m_effects.size()); }
    u32 spellCount() const { return static_cast<u32>(m_spells.size()); }

    void attach(const FxSocket& socket);
    u32 attachmentCount() const { return m_attachments; }

    bool beginCast(const std::string& spellId, const CastBinding& binding);
    CastPipeline& castPipeline() { return m_castPipeline; }
    const CastPipeline& castPipeline() const { return m_castPipeline; }

    ResidualEffectQueue& residuals() { return m_residuals; }
    const ResidualEffectQueue& residuals() const { return m_residuals; }

    /// Queue effect playback, advance casts, and cull residuals (no GPU work yet).
    void tick(const frame::FrameCtx& ctx = {});

    u32 tickCount() const { return m_tickCount; }

private:
    std::unordered_map<std::string, EffectDescriptor> m_effects;
    std::unordered_map<std::string, SpellDescriptor> m_spells;
    std::vector<FxSocket> m_sockets;
    CastPipeline m_castPipeline;
    ResidualEffectQueue m_residuals;
    u32 m_attachments = 0;
    u32 m_tickCount = 0;
};

} // namespace fuse::fx
