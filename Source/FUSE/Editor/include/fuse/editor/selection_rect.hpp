#pragma once

#include <utility>

namespace fuse::editor {

struct SelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

inline SelRect normalize_selection_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) {
        std::swap(x0, x1);
    }
    if (y0 > y1) {
        std::swap(y0, y1);
    }
    return SelRect{x0, y0, x1 - x0, y1 - y0};
}

} // namespace fuse::editor
