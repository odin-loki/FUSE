#include <fuse/project/manifest.hpp>

namespace fuse::project {

hybrid::DimensionFlags toDimensionFlags(const DimensionSettings& settings) {
    hybrid::DimensionFlags flags;
    flags.enable3D = settings.enable3D;
    flags.enable2D = settings.enable2D;
    flags.enableUI = settings.enableUI;
    return flags;
}

} // namespace fuse::project
