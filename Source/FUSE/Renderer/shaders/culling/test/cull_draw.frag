#version 460
// WP-1.3 test raster: writes the instance slot (R32_UINT). GLSL twin of cull_draw_fs.slang.
layout(location = 0) flat in uint inInstance;
layout(location = 0) out uint outId;

void main() { outId = inInstance; }
