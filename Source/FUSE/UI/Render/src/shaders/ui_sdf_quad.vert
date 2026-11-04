#version 450
// FUSE UI SDF-quad backend (UiVkBackend::SdfQuads, plan §4.9 / UI0 spike (c)): one instance per packed draw-list
// command (gl_InstanceIndex = command index; draws are issued per segment in draw order). The quad covers the
// command's clipped pixel area — the same integer bounds the compute kernel tests — so exactly the pixels whose centres
// the tile kernel would shade are rasterised. Commands that draw nothing here (Backdrop, Effect) become empty quads.

struct GpuCmd {
    vec4 rect;
    vec4 fill;
    vec4 fill2;
    vec4 border;
    vec4 radius;
    vec4 clip;
    vec4 params;
    uvec4 info;
};
layout(set = 0, binding = 1, std430) readonly buffer UiCmds { GpuCmd cmds[]; } ui_cmds;

layout(push_constant) uniform UiPush {
    uvec2 extent;
    uint tilesX;
    uint atlasSize;
    uint tileCount;
    uint imageCount;
    uint tileBase;
    uint tileOrigin;
    uint mode;
    uint cmd;
    uint src;
    uint dst;
} pc;

layout(location = 0) flat out uint v_cmd;

const uint kRect = 0u, kShadow = 1u, kGlyph = 2u, kImage = 3u, kLine = 4u;

void main() {
    const uint id = uint(gl_InstanceIndex);
    v_cmd = id;
    const GpuCmd c = ui_cmds.cmds[id];
    const uint op = c.info.x & 0xFFu;
    vec4 b = c.rect;
    if (op == kShadow) b = vec4(c.rect.x - c.params.y * 2.0, c.rect.y - c.params.y * 2.0, c.rect.z + c.params.y * 4.0, c.rect.w + c.params.y * 4.0);
    if (op == kLine) {
        precise float hw = c.params.x * 0.5 + 1.0;
        b = vec4(min(c.rect.x, c.rect.z) - hw, min(c.rect.y, c.rect.w) - hw, abs(c.rect.z - c.rect.x) + 2.0 * hw, abs(c.rect.w - c.rect.y) + 2.0 * hw);
    }
    precise float ax0 = max(b.x, c.clip.x);
    precise float ay0 = max(b.y, c.clip.y);
    precise float ax1 = min(b.x + b.z, c.clip.x + c.clip.z);
    precise float ay1 = min(b.y + b.w, c.clip.y + c.clip.w);
    const vec2 e = vec2(pc.extent);
    const vec2 p0 = clamp(vec2(floor(ax0), floor(ay0)), vec2(0.0), e);
    const vec2 p1 = clamp(vec2(ceil(ax1), ceil(ay1)), vec2(0.0), e);
    if (op > kLine || ax1 <= ax0 || ay1 <= ay0 || p1.x <= p0.x || p1.y <= p0.y) {
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0); // empty
        return;
    }
    // two triangles: (0,0) (1,0) (0,1) / (1,0) (1,1) (0,1)
    const uint v = uint(gl_VertexIndex);
    const vec2 k = vec2((v == 1u || v == 3u || v == 4u) ? 1.0 : 0.0, (v == 2u || v == 4u || v == 5u) ? 1.0 : 0.0);
    const vec2 p = mix(p0, p1, k);
    gl_Position = vec4(p / e * 2.0 - 1.0, 0.0, 1.0);
}
