#version 460
// WP-1.4 visibility-buffer raster, R32G32_UINT target (T0 default, depth test LESS on D32).
// x = instance slot, y = gl_PrimitiveID = the mesh triangle (MTRI index; one instance per draw).
// GLSL twin of vis_raster_fs.slang.
layout(location = 0) flat in uint inInstance;
layout(location = 0) out uvec2 outVis;

void main() { outVis = uvec2(inInstance, uint(gl_PrimitiveID)); }
