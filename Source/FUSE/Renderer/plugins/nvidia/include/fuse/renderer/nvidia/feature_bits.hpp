#pragma once

#include <cstdint>

namespace fuse::renderer::nvidia {

inline constexpr std::uint32_t kFeaturePresent = 1u;
inline constexpr std::uint32_t kFeatureTimeline = 2u;
inline constexpr std::uint32_t kFeatureMesh = 4u;

inline bool feature_bits_ready(std::uint32_t have, std::uint32_t need) {
    return (have & need) == need;
}

} // namespace fuse::renderer::nvidia
