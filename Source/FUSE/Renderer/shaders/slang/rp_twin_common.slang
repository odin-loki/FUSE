// WP-0.5 Slang module shared by the twin kernels (imported, not #included: exercises the slangc
// depfile -> rebuild path). Constants mirror shaders/compute/radix_sort_common.glsl.
module rp_twin_common;

public static const uint kRadixWgSize = 256;
public static const uint kRadixBins = 256;
public static const uint kRadixBatchesPerTile = 16;
public static const uint kRadixTileSize = kRadixWgSize * kRadixBatchesPerTile;

/// Same push block as radix_sort_common.glsl (RadixPush): 6 x uint, 24 bytes.
public struct RadixPush {
    public uint count;
    public uint shift;
    public uint numBlocks;
    public uint dataOffset;
    public uint sumsOffset;
    public uint dataLen;
};

public uint radixDigit(uint key, uint shift) {
    return (key >> shift) & 0xFFu;
}
