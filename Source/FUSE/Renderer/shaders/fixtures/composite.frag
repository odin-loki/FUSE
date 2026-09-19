#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    float blend;
    uint rasterTexIndex;
    uint cudaTexIndex;
} pc;

layout(set = 0, binding = 1) uniform texture2D rasterTextures[];
layout(set = 0, binding = 2) uniform sampler compositeSampler;

void main() {
    vec4 raster = texture(sampler2D(rasterTextures[nonuniformEXT(pc.rasterTexIndex)], compositeSampler), vUV);
    vec4 cuda = vec4(0.05, 0.15, 0.35, 1.0);
    outColor = mix(cuda, raster, pc.blend);
}
