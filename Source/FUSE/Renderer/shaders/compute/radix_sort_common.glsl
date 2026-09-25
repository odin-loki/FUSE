// GpuRadixSort shared declarations (fuse_rhi compute; see include/fuse/renderer/compute/gpu_radix_sort.hpp).
//
// Stable LSD radix sort, 8-bit digits, one reduce-then-scan pass per digit:
//   radix_histogram  per-tile 256-bin digit histogram -> scratch[digit * numBlocks + tile]
//   radix_scan       exclusive scan of 1024-entry chunks in place, chunk totals -> sums level
//   radix_scan_add   adds the scanned chunk totals back (multi-level; no inter-workgroup waits)
//   radix_scatter    stable in-tile ranking (two 4-bit split rounds) + scatter to global offsets
// No kernel ever waits on another workgroup (no decoupled look-back), so the sort needs no
// forward-progress guarantee beyond "every dispatch finishes" — Lavapipe-safe.
//
// Keys are uint (32-bit) or, with FUSE_RADIX_KEY64 defined, uvec2 (lo, hi) = little-endian u64.
// Values are always uint. Must match the C++ constants in gpu_radix_sort.hpp.

#define RADIX_WG_SIZE 256u
#define RADIX_BINS 256u
#define RADIX_BATCHES_PER_TILE 16u
#define RADIX_TILE_SIZE (RADIX_WG_SIZE * RADIX_BATCHES_PER_TILE)
#define RADIX_SCAN_ITEMS 4u
#define RADIX_SCAN_CHUNK (RADIX_WG_SIZE * RADIX_SCAN_ITEMS)

#ifdef FUSE_RADIX_KEY64
#define RADIX_KEY_T uvec2
uint radixDigit(uvec2 key, uint shift) {
    return ((shift < 32u) ? (key.x >> shift) : (key.y >> (shift - 32u))) & 0xFFu;
}
#else
#define RADIX_KEY_T uint
uint radixDigit(uint key, uint shift) {
    return (key >> shift) & 0xFFu;
}
#endif

layout(push_constant) uniform RadixPush {
    uint count;      // elements being sorted
    uint shift;      // bit offset of the current digit
    uint numBlocks;  // sort tiles (RADIX_TILE_SIZE elements each)
    uint dataOffset; // scan: first scratch uint of the level being scanned
    uint sumsOffset; // scan: first scratch uint of that level's chunk totals
    uint dataLen;    // scan: entries in the level
} pc;
