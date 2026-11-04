// grain(amount, fps): hash noise in [-1, 1) * amount * 255 added to the colour (kept <= alpha), re-seeded fps x / s
// (CPU: Core/src/effects.cpp).
vec4 fx_main(ivec2 p) {
    const vec4 c = fx_src(p);
    precise float tf = fx_time * fx_param(1);
    const uint frame = fx_param(1) > 0.0 ? uint(max(0.0, floor(tf))) : 0u;
    precise float n = fx_hash01(uint(p.x), uint(p.y), frame) * 2.0 - 1.0;
    precise float delta = n * fx_param(0) * 255.0;
    precise vec3 o = clamp(c.rgb + vec3(delta), vec3(0.0), vec3(c.a));
    return vec4(o, c.a);
}
