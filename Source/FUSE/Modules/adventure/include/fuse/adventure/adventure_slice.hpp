#pragma once

namespace fuse::adventure {

struct AdventureSlice {
    bool move;
    bool interact;
    bool dialogue;
    bool save;
};

inline bool adventure_slice_pass(AdventureSlice s) {
    return s.move && s.interact && s.dialogue && s.save;
}

inline int adventure_slice_missing(AdventureSlice s) {
    return static_cast<int>(!s.move) + static_cast<int>(!s.interact) +
           static_cast<int>(!s.dialogue) + static_cast<int>(!s.save);
}

} // namespace fuse::adventure
