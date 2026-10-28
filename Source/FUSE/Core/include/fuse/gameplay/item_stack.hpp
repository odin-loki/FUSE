#pragma once

#include <fuse/types.hpp>

namespace fuse::gameplay {

struct ItemStack {
    u32 count = 0;
    u32 maxCount = 0;
};

struct StackSplit {
    ItemStack stay{};
    ItemStack taken{};
    bool ok = false;
};

[[nodiscard]] inline StackSplit split_stack(ItemStack stack, u32 take) {
    StackSplit result;
    result.stay = stack;
    result.taken.maxCount = stack.maxCount;
    if (stack.maxCount == 0 || take == 0 || take >= stack.count) {
        return result;
    }
    result.ok = true;
    result.stay.count = stack.count - take;
    result.taken.count = take;
    return result;
}

} // namespace fuse::gameplay
