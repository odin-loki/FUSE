// fuse_ui_vk_custom_effect: a user effect compiled with fuse_ui_add_effect. Mixes the sampled image into the target
// (exact: divRn texel addressing) and scales by a vignette made of native exp / sin (not CPU-exact: the gate compares
// it within a tolerance). CPU twin: vignetteImage in test_vk_custom_effect.cpp.
vec4 fx_main(ivec2 p) {
    const vec4 c = fx_src(p);
    const float u = divRn((float(p.x) + 0.5) - fx_rect.x, fx_rect.z);
    const float v = divRn((float(p.y) + 0.5) - fx_rect.y, fx_rect.w);
    const ivec2 sz = fx_imageSize();
    const vec4 t = fx_imageTexel(ivec2(int(u * float(sz.x)), int(v * float(sz.y))));
    const float dx = u - 0.5, dy = v - 0.5;
    const float k = exp(-(dx * dx + dy * dy) * fx_param(0)) * (0.9 + 0.1 * sin(fx_time * 3.0 + u * 10.0));
    precise vec4 m = c + (t - c) * fx_param(1);
    return m * k;
}
