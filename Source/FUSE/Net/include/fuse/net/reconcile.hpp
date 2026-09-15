#pragma once

#include <fuse/net/game_state.hpp>
#include <fuse/net/input_history.hpp>
#include <fuse/types.hpp>

namespace fuse::net {

class RollbackBuffer;

enum class ReconcileAction : u8 {
    NoOp,      ///< Frame not tracked or no local prediction yet.
    Confirmed, ///< Authoritative input matches the predicted local input.
    Mismatch,  ///< Authoritative input differs — caller should rollback/resimulate.
};

struct ReconcileResult {
    ReconcileAction action = ReconcileAction::NoOp;
    u32 frame = 0;
};

/// Records authoritative input and compares it against the predicted local history (B7.4 stub).
[[nodiscard]] ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                                          const PlayerInput& authoritative);

/// Records confirmed remote input and compares it against the stored local prediction (B7.4 deepen).
[[nodiscard]] ReconcileResult reconcile_rollback_buffer(RollbackBuffer& buffer, u32 frame,
                                                          const PlayerInput& remote);

} // namespace fuse::net
