#pragma once

namespace fuse::editor {

struct Phase6Checks {
    bool host = false;
    bool play = false;
    bool undo = false;
    bool panels = false;
};

inline bool phase6_gate_pass(Phase6Checks checks) {
    return checks.host && checks.play && checks.undo && checks.panels;
}

inline int phase6_gate_missing(Phase6Checks checks) {
    return static_cast<int>(!checks.host) + static_cast<int>(!checks.play) + static_cast<int>(!checks.undo) +
           static_cast<int>(!checks.panels);
}

} // namespace fuse::editor
