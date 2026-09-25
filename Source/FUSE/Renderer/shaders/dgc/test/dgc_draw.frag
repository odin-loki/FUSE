#version 460
// WP-9.3 test raster: one pipeline per material bucket (specialization constant 0 = bucket), writes
// (slot << 6) | bucket (R32_UINT), so the image shows which pipeline drew each pixel.
// GLSL twin of dgc_draw_fs.slang.
layout(constant_id = 0) const uint kBucket = 0u;
layout(location = 0) flat in uint inInstance;
layout(location = 0) out uint outId;

void main() { outId = (inInstance << 6u) | kBucket; }
