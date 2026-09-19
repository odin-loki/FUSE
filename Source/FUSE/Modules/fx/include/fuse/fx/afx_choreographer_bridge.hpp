#pragma once

// Ore: Engine/source/afx/afxChoreographer.h — socket/cast/graph orchestration bridge

#include <fuse/fx/fx_socket.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::fx {

class FxComposer;

struct ChoreographerBinding {
    FxSocket socket;
    std::string spellId;
    bool beginCastOnAttach = false;
};

/// Deepens afxChoreographer orchestration — binds sockets, timelines, casts, and graph ticks.
class AfxChoreographerBridge {
public:
    bool bindSocket(FxComposer& composer, const ChoreographerBinding& binding);
    void tick(FxComposer& composer, const frame::FrameCtx& ctx);

    u32 bindingCount() const { return static_cast<u32>(m_bindings.size()); }
    u32 attachCount() const { return m_attachCount; }
    u32 castCount() const { return m_castCount; }
    u32 tickCount() const { return m_tickCount; }

private:
    std::vector<ChoreographerBinding> m_bindings;
    u32 m_attachCount = 0;
    u32 m_castCount = 0;
    u32 m_tickCount = 0;
};

} // namespace fuse::fx
