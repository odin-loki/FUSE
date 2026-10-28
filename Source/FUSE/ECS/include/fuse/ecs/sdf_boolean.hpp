#pragma once

#include <cmath>

namespace fuse::ecs {

enum class SdfBoolean { Union, Subtract, Intersect };

inline float csg_combine(SdfBoolean op, float a, float b) {
    const bool aOk = std::isfinite(a);
    const bool bOk = std::isfinite(b);
    if (!aOk && !bOk) {
        return 0.f;
    }
    if (!aOk) {
        return b;
    }
    if (!bOk) {
        return a;
    }
    switch (op) {
    case SdfBoolean::Union:
        return a < b ? a : b;
    case SdfBoolean::Intersect:
        return a > b ? a : b;
    case SdfBoolean::Subtract:
        return a > -b ? a : -b;
    }
    return 0.f;
}

} // namespace fuse::ecs
