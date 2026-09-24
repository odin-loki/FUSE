// FUSE visibility buffer (WP-1.4): the pixel formats, GLSL twin of
// include/fuse/renderer/visbuffer/vis_format.hpp (the one place the packing is defined; the Slang
// twin is vis_format.slang). Keep all three in sync: fuse_rp_visbuffer_cpu pins the constants and
// fuse_rp_visbuffer checks GPU output against the C++ functions bit for bit.
//
//   raster target  R32G32_UINT: x = instance slot, y = mesh triangle (MTRI index); clear = both ~0u
//   atomic target  64-bit word, atomicMin: depth unorm24 (floor) << 40 | instance << 20 | triangle;
//                  clear = ~0 (farther than anything). Shared with the WP-5.x software raster.
#ifndef FUSE_VIS_FORMAT_GLSL
#define FUSE_VIS_FORMAT_GLSL

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_VIS_INVALID 0xFFFFFFFFu
#define FUSE_VIS64_TRIANGLE_BITS 20u
#define FUSE_VIS64_INSTANCE_SHIFT 20u
#define FUSE_VIS64_DEPTH_SHIFT 40u
#define FUSE_VIS64_ID_MASK 0xFFFFFu
#define FUSE_VIS64_DEPTH_MAX 0xFFFFFFu
#define FUSE_VIS64_CLEAR 0xFFFFFFFFFFFFFFFFul

// floor(depth * 2^24) clamped to [0, 2^24 - 1] (exact: power-of-two scale, truncating convert).
uint fuse_vis64_quantize_depth(float depth) {
    if (!(depth > 0.0)) {
        return 0u;
    }
    if (depth >= 1.0) {
        return FUSE_VIS64_DEPTH_MAX;
    }
    return min(uint(depth * 16777216.0), FUSE_VIS64_DEPTH_MAX);
}

uint64_t fuse_vis64_pack(float depth, uint instance, uint triangle) {
    return (uint64_t(fuse_vis64_quantize_depth(depth)) << FUSE_VIS64_DEPTH_SHIFT) |
           (uint64_t(instance & FUSE_VIS64_ID_MASK) << FUSE_VIS64_INSTANCE_SHIFT) |
           uint64_t(triangle & FUSE_VIS64_ID_MASK);
}

uint fuse_vis64_instance(uint64_t v) { return uint(v >> FUSE_VIS64_INSTANCE_SHIFT) & FUSE_VIS64_ID_MASK; }
uint fuse_vis64_triangle(uint64_t v) { return uint(v) & FUSE_VIS64_ID_MASK; }
uint fuse_vis64_depth_bits(uint64_t v) { return uint(v >> FUSE_VIS64_DEPTH_SHIFT); }
bool fuse_vis64_valid(uint64_t v) { return fuse_vis64_instance(v) != FUSE_VIS64_ID_MASK; }

// Raster-format pair of a 64-bit word (invalid -> both ~0u).
uvec2 fuse_vis64_to_raster(uint64_t v) {
    return fuse_vis64_valid(v) ? uvec2(fuse_vis64_instance(v), fuse_vis64_triangle(v)) : uvec2(FUSE_VIS_INVALID);
}

// Depth exported for the Hi-Z: the far end of the quantisation interval, min((q + 1) / 2^24, 1),
// so a pyramid built from it never claims a surface nearer than it is. Cleared pixels: 1.
float fuse_vis64_export_depth(uint64_t v) {
    if (!fuse_vis64_valid(v)) {
        return 1.0;
    }
    return min(float(fuse_vis64_depth_bits(v) + 1u) * (1.0 / 16777216.0), 1.0);
}

#endif // FUSE_VIS_FORMAT_GLSL
