#version 450
// ui.mesh3d fragment stage: opaque lit colour (premultiplied for the draw-list compositor) and the item id.
layout(location = 0) noperspective in vec4 vColor;
layout(location = 1) flat in uint vId;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outId;
void main() {
    outColor = vec4(vColor.rgb * vColor.a, vColor.a);
    outId = vId;
}
