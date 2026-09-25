// WP-7.3 path-tracing mode: the RT pipeline's ray payload (GLSL twin of the PtPayload in pt_common.slang). Shared by
// every ray-tracing stage; one payload type for both ray types.
#ifndef FUSE_PT_PAYLOAD_GLSL
#define FUSE_PT_PAYLOAD_GLSL

#define PT_RAY_RADIANCE 0
#define PT_RAY_SHADOW 1
#define PT_RAY_TYPES 2

struct PtPayload {
    PtHit hit;            // radiance: the surface record (t = -1: miss); shadow: t = 0 occluded, -1 visible
    uint originInstance;  // the triangle the ray starts on (skipped by the any-hit shader)
    uint originPrimitive;
    uint pad0;
    uint pad1;
};

#endif
