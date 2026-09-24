// WP-1.3 test raster: writes the instance slot (R32_UINT). Slang twin of cull_draw.frag.
struct FSIn {
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

[shader("fragment")]
uint main(FSIn input) : SV_Target0 {
    return input.instance;
}
