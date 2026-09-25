// WP-9.3 test raster: Slang twin of dgc_draw.frag (specialization constant 0 = bucket).
[[vk::constant_id(0)]] const uint kBucket = 0u;

struct FSIn {
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

[shader("fragment")]
uint main(FSIn input) : SV_Target0 {
    return (input.instance << 6u) | kBucket;
}
