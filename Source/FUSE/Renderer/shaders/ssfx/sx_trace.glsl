// WP-6.3: the screen-space ray march shared by ssfx.ssr and ssfx.ssgi — a line-for-line port of
// fuse::ssfx::ssr_kernel::trace_ray / trace_pixel / reflect (ScreenSpace/include/fuse/ssfx/ssr_kernel.hpp).
// Parameters arrive clamped (ssr_kernel::clamp_params is idempotent, so the oracle's re-clamp is a no-op).
// GLSL twin of sx_trace.slang.
#ifndef FUSE_SX_TRACE_GLSL
#define FUSE_SX_TRACE_GLSL
#include "sx_common.glsl"
#include "atmosphere/at_sample.glsl"

struct SxTrace {
    uint maxSteps;
    float stride;
    float thickness;
    float maxDistance;
    float fadeEdge;
    uint refineSteps;
};

struct SxHit {
    bool hit;
    float px;
    float py;
    float distance;
    vec3 color;
    float confidence;
};

float sx_depth_nearest(float px, float py) {
    if (!sx_inside(px, py)) {
        return 0.0;
    }
    return sx_depth_at(uint(px), uint(py));
}

float sx_ray_z_at(float k0, float k1, float f) {
    precise float z = 1.0 / (k0 + (k1 - k0) * f);
    return z;
}

vec3 sx_reflect(vec3 incident, vec3 n) {
    precise float d = 2.0 * sx_dot(incident, n);
    return sx_sub(incident, sx_scale(n, d));
}

SxHit sx_trace_ray(uint64_t sceneColor, SxTrace P, uint x, uint y, vec3 direction) {
    SxHit result;
    result.hit = false;
    result.px = 0.0;
    result.py = 0.0;
    result.distance = 0.0;
    result.color = vec3(0.0);
    result.confidence = 0.0;
    if (x >= F.width || y >= F.height || sx_depth_at(x, y) <= 0.0 || sx_length(direction) <= 0.0) {
        return result;
    }
    const vec3 p = sx_position_at(x, y);
    const vec3 r = sx_normalize(direction);

    precise float rayLength = P.maxDistance;
    if (r.z < 0.0) {
        precise float maxLen = (p.z - F.nearZ * 1.01) / -r.z;
        rayLength = min(rayLength, maxLen);
    }
    if (rayLength <= 1e-4) {
        return result;
    }
    const vec3 e = sx_add(p, sx_scale(r, rayLength));

    float p0x, p0y, p1x, p1y;
    if (!sx_project(p, p0x, p0y) || !sx_project(e, p1x, p1y)) {
        return result;
    }
    precise float k0 = 1.0 / p.z;
    precise float k1 = 1.0 / e.z;
    const vec3 q0 = sx_scale(p, k0);
    const vec3 q1 = sx_scale(e, k1);
    precise float ddx = p1x - p0x;
    precise float ddy = p1y - p0y;
    const float pixelLength = max(abs(ddx), abs(ddy));
    if (pixelLength < 1e-3) {
        return result;
    }
    precise float strideQ = pixelLength / float(P.maxSteps);
    const float stride = max(P.stride, strideQ);
    precise float stepsF = pixelLength / stride;
    const uint stepCount = uint(ceil(stepsF));
    precise float fStep = stride / pixelLength;

    float prevF = 0.0;
    float prevZ = p.z;
    bool hit = false;
    float hitF = 0.0;
    for (uint i = 1u; i <= stepCount; ++i) {
        precise float fi = float(i) * fStep;
        const float f = min(1.0, fi);
        precise float sx = p0x + ddx * f;
        precise float sy = p0y + ddy * f;
        if (!sx_inside(sx, sy)) {
            break;
        }
        const float rayZ = sx_ray_z_at(k0, k1, f);
        const float sceneZ = sx_depth_nearest(sx, sy);
        if (sceneZ > 0.0 && !(uint(sx) == x && uint(sy) == y)) {
            const float zMin = min(prevZ, rayZ);
            const float zMax = max(prevZ, rayZ);
            precise float farZ = sceneZ + P.thickness;
            if (zMax >= sceneZ && zMin <= farZ) {
                float lo = prevF;
                float hi = f;
                for (uint k = 0u; k < P.refineSteps; ++k) {
                    precise float mid = 0.5 * (lo + hi);
                    precise float mx = p0x + ddx * mid;
                    precise float my = p0y + ddy * mid;
                    const float midSceneZ = sx_depth_nearest(mx, my);
                    if (midSceneZ > 0.0 && sx_ray_z_at(k0, k1, mid) >= midSceneZ) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                hit = true;
                hitF = hi;
                break;
            }
        }
        prevF = f;
        prevZ = rayZ;
    }
    if (!hit) {
        return result;
    }

    result.hit = true;
    precise float hpx = p0x + ddx * hitF;
    precise float hpy = p0y + ddy * hitF;
    result.px = hpx;
    result.py = hpy;
    const uint hx = min(F.width - 1u, uint(max(0.0, hpx)));
    const uint hy = min(F.height - 1u, uint(max(0.0, hpy)));
    result.color = sx_color_at(sceneColor, sx_index(hx, hy));

    precise float k = k0 + (k1 - k0) * hitF;
    precise float invK = 1.0 / k;
    const vec3 hitPos = sx_scale(sx_add(q0, sx_scale(sx_sub(q1, q0), hitF)), invK);
    result.distance = sx_length(sx_sub(hitPos, p));

    float edgeFade = 1.0;
    if (P.fadeEdge > 0.0) {
        precise float u = hpx / float(F.width);
        precise float vv = hpy / float(F.height);
        precise float u1 = 1.0 - u;
        precise float v1 = 1.0 - vv;
        const float edge = min(min(u, u1), min(vv, v1));
        precise float ef = edge / P.fadeEdge;
        edgeFade = sx_saturate(ef);
    }
    precise float df = 4.0 * (1.0 - result.distance / P.maxDistance);
    const float distanceFade = sx_saturate(df);
    precise float conf = edgeFade * distanceFade;
    result.confidence = conf;
    return result;
}

// --- sky fallback (kSsfxFlagSky; CPU twins: ssfx_gpu::sky_exit / sky_world_dir / pixel_reflection) ------------
// A ray the march missed counts as reaching the sky when its end point (the march's own near-plane clip and max
// distance) leaves the screen or lies over a sky pixel (depth 0).
bool sx_sky_exit(SxTrace P, uint x, uint y, vec3 direction) {
    if (x >= F.width || y >= F.height || sx_depth_at(x, y) <= 0.0 || sx_length(direction) <= 0.0) {
        return false;
    }
    const vec3 p = sx_position_at(x, y);
    const vec3 r = sx_normalize(direction);
    precise float rayLength = P.maxDistance;
    if (r.z < 0.0) {
        precise float maxLen = (p.z - F.nearZ * 1.01) / -r.z;
        rayLength = min(rayLength, maxLen);
    }
    if (rayLength <= 1e-4) {
        return false;
    }
    const vec3 e = sx_add(p, sx_scale(r, rayLength));
    float ex, ey;
    if (!sx_project(e, ex, ey)) {
        return false;
    }
    if (!sx_inside(ex, ey)) {
        return true;
    }
    return sx_depth_nearest(ex, ey) <= 0.0;
}

// The WP-8.2 sky radiance (no sun disk) along the view-space (ssfx convention) direction `d`.
vec3 sx_sky(vec3 d) {
    const float ex = d.x;
    const float ey = -d.y;
    const float ez = -d.z;
    precise float wx = F.viewRot[0] * ex + F.viewRot[1] * ey + F.viewRot[2] * ez;
    precise float wy = F.viewRot[4] * ex + F.viewRot[5] * ey + F.viewRot[6] * ez;
    precise float wz = F.viewRot[8] * ex + F.viewRot[9] * ey + F.viewRot[10] * ez;
    return at_sky_radiance(at_load(F.sky), vec3(wx, wy, wz), false);
}

// Mirror reflection direction of pixel (x, y) (sx_trace_pixel's).
vec3 sx_pixel_reflection(uint x, uint y) {
    const vec3 p = sx_position_at(x, y);
    vec3 n = sx_normalize(sx_normal_at(x, y));
    const vec3 v = sx_normalize(p);
    if (sx_dot(n, v) > 0.0) {
        n = sx_neg(n);
    }
    return sx_reflect(v, n);
}

SxHit sx_trace_pixel(uint64_t sceneColor, SxTrace P, uint x, uint y) {
    if (x >= F.width || y >= F.height || sx_depth_at(x, y) <= 0.0) {
        SxHit none;
        none.hit = false;
        none.px = 0.0;
        none.py = 0.0;
        none.distance = 0.0;
        none.color = vec3(0.0);
        none.confidence = 0.0;
        return none;
    }
    const vec3 p = sx_position_at(x, y);
    vec3 n = sx_normalize(sx_normal_at(x, y));
    const vec3 v = sx_normalize(p);
    if (sx_dot(n, v) > 0.0) {
        n = sx_neg(n);
    }
    return sx_trace_ray(sceneColor, P, x, y, sx_reflect(v, n));
}
#endif
