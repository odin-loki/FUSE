#pragma once

// WP-9.2 splat asset loader: the standard 3DGS .ply layout (the INRIA reference implementation's output).
//
//   element vertex N
//   property float x y z [nx ny nz]
//   property float f_dc_0 f_dc_1 f_dc_2
//   property float f_rest_0 .. f_rest_{3 * ((d + 1)^2 - 1) - 1}   (channel-major: f_rest[c * (K - 1) + k - 1])
//   property float opacity                                         (logit)
//   property float scale_0 scale_1 scale_2                         (log)
//   property float rot_0 rot_1 rot_2 rot_3                          (w, x, y, z; not normalised)
//
// Formats: binary_little_endian, binary_big_endian and ascii; property order is free; scalar types
// char / uchar / short / ushort / int / uint / float / double (and the int8 .. float64 aliases). Other vertex
// properties and other elements (fixed-size scalar properties or lists) are skipped. The SH degree follows the
// f_rest count (0, 9, 24, 45 -> 0, 1, 2, 3). Activations applied on load: opacity = sigmoid, scale = exp,
// rotation normalised (a zero quaternion becomes identity).

#include <fuse/renderer/gsplat/gsplat_types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::gsplat {

struct GsAsset {
    std::vector<GsSplat> splats;
    u32 shDegree = 0;
};

struct GsPlyResult {
    bool ok = false;
    std::string error;
};

GsPlyResult gs_load_ply(const u8* data, usize size, GsAsset& out);
GsPlyResult gs_load_ply_file(const char* path, GsAsset& out);

/// Writes the standard layout (binary little endian; x y z nx ny nz f_dc f_rest opacity scale rot), inverting
/// the activations (the round-trip / fixture writer).
std::vector<u8> gs_write_ply(const GsAsset& asset);

} // namespace fuse::renderer::gsplat
