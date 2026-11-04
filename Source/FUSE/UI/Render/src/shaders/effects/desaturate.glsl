// desaturate(amount, r, g, b): Rec. 709 luminance times the tint, mixed in by amount (CPU: Core/src/effects.cpp).
vec4 fx_main(ivec2 p) {
    const vec4 c = fx_src(p);
    precise float y = c.r * uintBitsToFloat(0x3e59b3d0u) + c.g * uintBitsToFloat(0x3f371759u) + c.b * uintBitsToFloat(0x3d93dd98u);
    const float a = fx_param(0);
    precise vec3 o = c.rgb + (y * vec3(fx_param(1), fx_param(2), fx_param(3)) - c.rgb) * a;
    return vec4(o, c.a);
}
