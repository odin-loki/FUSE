// ripple(amplitude, wavelength, speed): samples displaced along the ray from the rect centre by amplitude * sin
// (CPU: Core/src/effects.cpp).
vec4 fx_main(ivec2 p) {
    precise float cx = fx_rect.x + fx_rect.z * 0.5;
    precise float cy = fx_rect.y + fx_rect.w * 0.5;
    precise float dx = (float(p.x) + 0.5) - cx;
    precise float dy = (float(p.y) + 0.5) - cy;
    precise float d2 = dx * dx + dy * dy;
    const float d = sqrtRn(d2);
    const float wl = max(fx_param(1), 1.0);
    precise float num = d - fx_time * fx_param(2);
    precise float phase = divRn(num, wl) * FX_TWO_PI;
    precise float off = fx_param(0) * fx_sin(phase);
    const float k = d > 0.5 ? divRn(off, d) : 0.0;
    precise float ox = dx * k + 0.5;
    precise float oy = dy * k + 0.5;
    return fx_src(p + ivec2(int(floor(ox)), int(floor(oy))));
}
