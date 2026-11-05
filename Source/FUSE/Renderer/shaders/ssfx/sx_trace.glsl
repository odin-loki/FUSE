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
    bool hiz; // Hi-Z skipping (SSR with kSsfxFlagHiz)
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
    precise float den = k0 + (k1 - k0) * f;
    const float z = fuse_div_rn(1.0, den);
    return z;
}


// --- Hi-Z march (kSsfxFlagHiz; SsfxGpuSettings::ssrHiz) -------------------------------------------------------
// The march tests samples f_i = min(1, i fStep), i = 1..stepCount; sample i hits when the scene depth under it
// (nearest pixel, > 0) satisfies max(z_{i-1}, z_i) >= sceneZ (and the thickness test). The Hi-Z pyramid stores per cell
// the minimum depth of its pixels (sky = SX_HIZ_SKY), so when every sample i..b of a run lies inside ONE cell and
// max(z_{i-1}, z_i, z_b) < cellMin, no sample of the run can hit (the ray depth 1 / (k0 + (k1 - k0) f) is monotonic in
// f, so its maximum over the run is at an end; every sceneZ of the cell is >= cellMin) and the run is skipped. The
// samples after it see exactly the state (f_b, z_b) the full march would have, so the hits are the full march's bit
// for bit. Sample positions are monotonic in i (rounding is monotonic), so the run is inside the cell when its two
// ends are. Counters: samples skipped / tested (written by ssfx.ssr into the ssrSteps section).
uint g_sx_hiz_skipped = 0u;
uint g_sx_tested = 0u;

float sx_hiz_at(uint level, uint cx, uint cy) {
    const uint w = (F.width + (1u << level) - 1u) >> level;
    return SxFloatsRef(F.hiz).v[F.hizOffset[level] + cy * w + cx];
}

// Last sample index b in [i, stepCount] of the run starting at sample i (pixel (px, py)) that stays in the level-L
// cell of (px, py); the estimate is verified on the exact sample positions (only a speed choice: the hits do not
// depend on it). Returns b and f_b.
uint sx_hiz_run(uint i, uint stepCount, float fStep, float p0x, float p0y, float ddx, float ddy, uint px, uint py,
                uint level, out float fb) {
    const uint x0 = (px >> level) << level;
    const uint y0 = (py >> level) << level;
    const uint x1 = min(F.width, x0 + (1u << level));
    const uint y1 = min(F.height, y0 + (1u << level));
    float fLim = 1.0;
    if (ddx > 0.0) {
        fLim = min(fLim, (float(x1) - p0x) / ddx);
    } else if (ddx < 0.0) {
        fLim = min(fLim, (float(x0) - p0x) / ddx);
    }
    if (ddy > 0.0) {
        fLim = min(fLim, (float(y1) - p0y) / ddy);
    } else if (ddy < 0.0) {
        fLim = min(fLim, (float(y0) - p0y) / ddy);
    }
    uint b = uint(max(0.0, fLim / fStep));
    b = clamp(b, i, stepCount);
    for (uint tries = 0u; tries < 8u; ++tries) {
        precise float fj = float(b) * fStep;
        const float f = min(1.0, fj);
        precise float sx = p0x + ddx * f;
        precise float sy = p0y + ddy * f;
        if (sx >= float(x0) && sx < float(x1) && sy >= float(y0) && sy < float(y1)) {
            fb = f;
            return b;
        }
        if (b == i) {
            break;
        }
        b = i + (b - i) / 2u;
    }
    precise float fi = float(i) * fStep;
    fb = min(1.0, fi);
    return i;
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
        precise float lenN = p.z - F.nearZ * 1.01;
        const float maxLen = fuse_div_rn(lenN, -r.z);
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
    const float k0 = fuse_div_rn(1.0, p.z);
    const float k1 = fuse_div_rn(1.0, e.z);
    const vec3 q0 = sx_scale(p, k0);
    const vec3 q1 = sx_scale(e, k1);
    precise float ddx = p1x - p0x;
    precise float ddy = p1y - p0y;
    const float pixelLength = max(abs(ddx), abs(ddy));
    if (pixelLength < 1e-3) {
        return result;
    }
    const float strideQ = fuse_div_rn(pixelLength, float(P.maxSteps));
    const float stride = max(P.stride, strideQ);
    const float stepsF = fuse_div_rn(pixelLength, stride);
    const uint stepCount = uint(ceil(stepsF));
    const float fStep = fuse_div_rn(stride, pixelLength);

    float prevF = 0.0;
    float prevZ = p.z;
    bool hit = false;
    float hitF = 0.0;
    const bool useHiz = P.hiz && F.hizLevels > 0u;
    uint level = 1u;
    for (uint i = 1u; i <= stepCount; ++i) {
        precise float fi = float(i) * fStep;
        const float f = min(1.0, fi);
        precise float sx = p0x + ddx * f;
        precise float sy = p0y + ddy * f;
        if (!sx_inside(sx, sy)) {
            break;
        }
        const float rayZ = sx_ray_z_at(k0, k1, f);
        if (useHiz) {
            bool skipped = false;
            for (uint L = min(level, F.hizLevels); L > 0u; --L) {
                float fb;
                const uint b = sx_hiz_run(i, stepCount, fStep, p0x, p0y, ddx, ddy, uint(sx), uint(sy), L, fb);
                const float zb = sx_ray_z_at(k0, k1, fb);
                const float runMax = max(prevZ, max(rayZ, zb));
                if (runMax < sx_hiz_at(L, uint(sx) >> L, uint(sy) >> L)) {
                    g_sx_hiz_skipped += b - i + 1u;
                    prevF = fb;
                    prevZ = zb;
                    i = b; // the loop's ++i continues after the run
                    level = min(L + 1u, F.hizLevels);
                    skipped = true;
                    break;
                }
            }
            if (skipped) {
                continue;
            }
            level = 1u;
        }
        ++g_sx_tested;
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
    const float invK = fuse_div_rn(1.0, k);
    const vec3 hitPos = sx_scale(sx_add(q0, sx_scale(sx_sub(q1, q0), hitF)), invK);
    result.distance = sx_length(sx_sub(hitPos, p));

    float edgeFade = 1.0;
    if (P.fadeEdge > 0.0) {
        const float u = fuse_div_rn(hpx, float(F.width));
        const float vv = fuse_div_rn(hpy, float(F.height));
        precise float u1 = 1.0 - u;
        precise float v1 = 1.0 - vv;
        const float edge = min(min(u, u1), min(vv, v1));
        const float ef = fuse_div_rn(edge, P.fadeEdge);
        edgeFade = sx_saturate(ef);
    }
    precise float df = 4.0 * (1.0 - fuse_div_rn(result.distance, P.maxDistance));
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
        precise float lenN = p.z - F.nearZ * 1.01;
        const float maxLen = fuse_div_rn(lenN, -r.z);
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
