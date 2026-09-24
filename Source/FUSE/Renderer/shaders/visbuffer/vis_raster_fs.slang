// WP-1.4 visibility-buffer raster, R32G32_UINT target. Slang twin of vis_raster.frag.
struct FSIn {
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

[shader("fragment")]
uint2 main(FSIn input, uint primitive : SV_PrimitiveID) : SV_Target0 {
    return uint2(input.instance, primitive);
}
