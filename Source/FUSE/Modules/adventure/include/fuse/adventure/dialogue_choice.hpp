#pragma once

namespace fuse::adventure {

/// Choice slot when `count` is positive and `choice` lies in `0 .. count - 1`.
/// Otherwise -1.
[[nodiscard]] inline int dialogue_choice_index(int choice, int count) {
    if (count > 0 && choice >= 0 && choice < count) {
        return choice;
    }
    return -1;
}

/// True when `count` is positive.
[[nodiscard]] inline bool dialogue_has_choices(int count) {
    return count > 0;
}

} // namespace fuse::adventure
