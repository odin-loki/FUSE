#pragma once

namespace fuse::editor {

struct SliceChecks {
    bool playable = false;
    bool saveRoundtrip = false;
    bool audio = false;
    bool input = false;
};

inline bool slice_gate_pass(SliceChecks checks) {
    return checks.playable && checks.saveRoundtrip && checks.audio && checks.input;
}

inline int slice_gate_missing(SliceChecks checks) {
    return static_cast<int>(!checks.playable) + static_cast<int>(!checks.saveRoundtrip) +
           static_cast<int>(!checks.audio) + static_cast<int>(!checks.input);
}

} // namespace fuse::editor
