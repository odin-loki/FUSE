#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::fx {

enum class FxSocketKind {
    Sprite2D,
    Shape3D,
};

/// FX attachment point — ore analogue: AFX effectron/choreographer sockets.
/// TODO(U5 extract): Engine/source/afx/ (218 files) → refactor into this module.
/// TODO(U5 extract): third_party/addons/AFX-Template/game/ for sample content only.
struct FxSocket {
    Handle<Object> owner = Handle<Object>::invalid();
    FxSocketKind kind = FxSocketKind::Shape3D;
    std::string effectId;
};

/// Game-thread spawn/dispatch; particle sim jobified in a later slice.
class FxComposer {
public:
    void attach(const FxSocket& socket);
    u32 attachmentCount() const { return m_attachments; }

    /// Queue effect playback for this frame (no GPU work yet).
    void tick();

    u32 tickCount() const { return m_tickCount; }

private:
    u32 m_attachments = 0;
    u32 m_tickCount = 0;
};

} // namespace fuse::fx
