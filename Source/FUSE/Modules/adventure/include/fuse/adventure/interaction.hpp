#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::adventure {

enum class InteractionKind {
    PickUp,
    Use,
    Talk,
};

/// Interaction request — game-thread only; 2D/3D share this interface.
/// TODO(U5 extract): 3DAAK interaction templates under third_party/addons/3DAAK/
struct InteractionRequest {
    Handle<Object> actor = Handle<Object>::invalid();
    Handle<Object> target = Handle<Object>::invalid();
    InteractionKind kind = InteractionKind::Use;
    std::string payload;
};

class InteractionSystem {
public:
    bool tryInteract(const InteractionRequest& request);
    u32 successCount() const { return m_successCount; }

private:
    u32 m_successCount = 0;
};

} // namespace fuse::adventure
