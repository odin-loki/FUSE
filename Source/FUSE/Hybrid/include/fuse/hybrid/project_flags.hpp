#pragma once

namespace fuse::hybrid {

/// Project-level dimension enable flags (maps to future project.json).
struct DimensionFlags {
    bool enable3D = true;
    bool enable2D = true;
    bool enableUI = true;
};

} // namespace fuse::hybrid
