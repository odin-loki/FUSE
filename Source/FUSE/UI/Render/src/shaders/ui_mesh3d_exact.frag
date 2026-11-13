#version 450
// ui.mesh3d fragment stage, exact-depth variant: recomputes the fragment depth from the triangle's vertices
// (gl_PrimitiveID into the index / vertex buffers) with exactly the float operations of fuse::ui::rasterMesh, so
// near-coplanar surfaces resolve identically on the CPU and GPU (equal depths: first drawn wins on both, LESS).
// Needs the geometryShader device feature (PrimitiveId in a fragment shader).
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 light;
    vec4 size;
} pc;
layout(std430, set = 0, binding = 0) readonly buffer Verts { float v[]; };   // 8 floats per MeshVertex
layout(std430, set = 0, binding = 1) readonly buffer Inds { uint idx[]; };
layout(location = 0) noperspective in vec4 vColor;
layout(location = 1) flat in uint vId;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outId;

struct S { precise float x, y, z; bool ok; };
S project(uint i) {
    precise float px = v[i * 8u], py = v[i * 8u + 1u], pz = v[i * 8u + 2u];
    // ((m0 x + m4 y) + m8 z) + m12, column-major, left to right as the C++ reference
    precise float cx = pc.mvp[0][0] * px + pc.mvp[1][0] * py + pc.mvp[2][0] * pz + pc.mvp[3][0];
    precise float cy = pc.mvp[0][1] * px + pc.mvp[1][1] * py + pc.mvp[2][1] * pz + pc.mvp[3][1];
    precise float cz = pc.mvp[0][2] * px + pc.mvp[1][2] * py + pc.mvp[2][2] * pz + pc.mvp[3][2];
    precise float cw = pc.mvp[0][3] * px + pc.mvp[1][3] * py + pc.mvp[2][3] * pz + pc.mvp[3][3];
    S s;
    s.ok = cw > 1e-6;
    precise float sx = (cx / cw * 0.5 + 0.5) * pc.size.x, sy = (cy / cw * 0.5 + 0.5) * pc.size.y;
    s.x = roundEven(sx * 256.0);
    s.y = roundEven(sy * 256.0);
    s.z = cz / cw;
    return s;
}
void main() {
    uint t = uint(gl_PrimitiveID) * 3u;
    S a = project(idx[t]), b = project(idx[t + 1u]), c = project(idx[t + 2u]);
    if (!a.ok || !b.ok || !c.ok) discard;
    precise float px = gl_FragCoord.x * 256.0, py = gl_FragCoord.y * 256.0;
    precise float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    precise float e0 = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x);
    precise float e1 = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x);
    precise float l0 = e0 / area, l1 = e1 / area;
    precise float l2 = 1.0 - l0 - l1;
    precise float z = l0 * a.z + l1 * b.z + l2 * c.z;
    if (!(z >= 0.0 && z <= 1.0)) discard; // NaN too
    gl_FragDepth = z;
    outColor = vec4(vColor.rgb * vColor.a, vColor.a);
    outId = vId;
}
