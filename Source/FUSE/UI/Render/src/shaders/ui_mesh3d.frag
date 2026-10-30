#version 450
// ui.mesh3d fragment stage: opaque lit colour, premultiplied for the draw-list compositor.
layout(location = 0) noperspective in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vColor.rgb * vColor.a, vColor.a); }
