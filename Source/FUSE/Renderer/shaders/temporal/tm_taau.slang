// WP-4.1 "taau.resolve". Slang twin of tm_taau.comp (see there) and of taau_kernel.hpp / taa_kernel_common.hpp.
#include "tm_common.slang"

[[vk::binding(1, 0)]] Texture2D<float4> fuse_tm_textures[];
[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_tm_out_images[];

static const float kFuseTmSkyDepth = 1.0e6;
static const float kFuseTmBig = 3.0e38;
static const uint kFuseTmFilterBilinear = 0u;
static const uint kFuseTmFilterLanczos2 = 2u;

// --- taa_common twins ----------------------------------------------------------------------------------
float fuseTmSaturate(float v) { return max(0.0, min(1.0, v)); }

float3 fuseTmLerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }

int fuseTmClampIndex(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }

float fuseTmLuma709(float3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

float3 fuseTmRgbToYcocg(float3 rgb) {
    float3 r;
    r.x = 0.25 * rgb.x + 0.5 * rgb.y + 0.25 * rgb.z;
    r.y = 0.5 * rgb.x - 0.5 * rgb.z;
    r.z = -0.25 * rgb.x + 0.5 * rgb.y - 0.25 * rgb.z;
    return r;
}

float3 fuseTmYcocgToRgb(float3 c) {
    const float tmp = c.x - c.z;
    return float3(tmp + c.y, c.x + c.z, tmp - c.y);
}

float3 fuseTmClipToAabb(float3 history, float3 boxMin, float3 boxMax) {
    const float3 center = (boxMin + boxMax) * 0.5;
    const float3 extent = (boxMax - boxMin) * 0.5;
    const float3 offset = history - center;
    const float eps = 1e-6;
    const float ux = abs(offset.x) / max(extent.x, eps);
    const float uy = abs(offset.y) / max(extent.y, eps);
    const float uz = abs(offset.z) / max(extent.z, eps);
    const float maxUnit = max(ux, max(uy, uz));
    if (maxUnit > 1.0) {
        return center + offset * (1.0 / maxUnit);
    }
    return history;
}

void fuseTmCatmullRomWeights(float t, out float w[4]) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
    w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
    w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}

float fuseTmLanczos2(float x) {
    const float ax = abs(x);
    if (ax < 1e-5) {
        return 1.0;
    }
    if (ax >= 2.0) {
        return 0.0;
    }
    const float kPi = asfloat(0x40490FDBu); // 3.14159265358979f
    const float px = kPi * ax;
    return 2.0 * sin(px) * sin(0.5 * px) / (px * px);
}

void fuseTmLanczos2Weights(float t, out float w[4]) {
    const float w0 = fuseTmLanczos2(t + 1.0);
    const float w1 = fuseTmLanczos2(t);
    const float w2 = fuseTmLanczos2(1.0 - t);
    const float w3 = fuseTmLanczos2(2.0 - t);
    const float inv = 1.0 / (w0 + w1 + w2 + w3);
    w[0] = w0 * inv;
    w[1] = w1 * inv;
    w[2] = w2 * inv;
    w[3] = w3 * inv;
}

// --- span access ------------------------------------------------------------------------------------------
float3 fuseTmColor(uint handle, int x, int y) { return fuse_tm_textures[handle & 0xFFFFFu].Load(int3(x, y, 0)).xyz; }

float fuseTmMask(uint handle, int x, int y) { return fuse_tm_textures[handle & 0xFFFFFu].Load(int3(x, y, 0)).x; }

float3 fuseTmColorTexel(TmTaauFrame f, int x, int y) {
    return fuseTmColor(f.color, fuseTmClampIndex(x, int(f.renderW) - 1), fuseTmClampIndex(y, int(f.renderH) - 1));
}

float4 fuseTmHistoryTexel(TmTaauFrame f, int x, int y) {
    const int cx = fuseTmClampIndex(x, int(f.displayW) - 1);
    const int cy = fuseTmClampIndex(y, int(f.displayH) - 1);
    float4* h = (float4*)f.historyIn;
    return h[uint(cy) * f.displayW + uint(cx)];
}

// --- taau_kernel twins ------------------------------------------------------------------------------------
float fuseTmEffectiveDepth(float d) { return d > 0.0 ? d : kFuseTmSkyDepth; }

float fuseTmSanitize1(float v) { return (v >= 0.0 && v < kFuseTmBig) ? v : 0.0; }
float3 fuseTmSanitize(float3 c) { return float3(fuseTmSanitize1(c.x), fuseTmSanitize1(c.y), fuseTmSanitize1(c.z)); }

float fuseTmSampleWeight(float d) { return d < 1.0 ? fuseTmLanczos2(d) : 0.0; }

float fuseTmSampleHistoryWeight(TmTaauFrame f, float px, float py) {
    const float fx = px - 0.5;
    const float fy = py - 0.5;
    const int x0 = int(floor(fx));
    const int y0 = int(floor(fy));
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);
    const float a = fuseTmHistoryTexel(f, x0, y0).w;
    const float b = fuseTmHistoryTexel(f, x0 + 1, y0).w;
    const float c = fuseTmHistoryTexel(f, x0, y0 + 1).w;
    const float d = fuseTmHistoryTexel(f, x0 + 1, y0 + 1).w;
    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

float3 fuseTmSampleHistory(TmTaauFrame f, float px, float py, uint filter) {
    const float fx = px - 0.5;
    const float fy = py - 0.5;
    const int x1 = int(floor(fx));
    const int y1 = int(floor(fy));
    const float tx = fx - float(x1);
    const float ty = fy - float(y1);
    if (filter == kFuseTmFilterBilinear) {
        const float4 a = fuseTmHistoryTexel(f, x1, y1);
        const float4 b = fuseTmHistoryTexel(f, x1 + 1, y1);
        const float4 c = fuseTmHistoryTexel(f, x1, y1 + 1);
        const float4 d = fuseTmHistoryTexel(f, x1 + 1, y1 + 1);
        const float3 top = fuseTmLerp3(a.xyz, b.xyz, tx);
        const float3 bottom = fuseTmLerp3(c.xyz, d.xyz, tx);
        return fuseTmLerp3(top, bottom, ty);
    }
    float wx[4];
    float wy[4];
    if (filter == kFuseTmFilterLanczos2) {
        fuseTmLanczos2Weights(tx, wx);
        fuseTmLanczos2Weights(ty, wy);
    } else {
        fuseTmCatmullRomWeights(tx, wx);
        fuseTmCatmullRomWeights(ty, wy);
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 lo = float3(kFuseTmBig, kFuseTmBig, kFuseTmBig);
    float3 hi = float3(0.0, 0.0, 0.0);
    for (int j = 0; j < 4; ++j) {
        float3 row = float3(0.0, 0.0, 0.0);
        for (int i = 0; i < 4; ++i) {
            const float3 c = fuseTmHistoryTexel(f, x1 - 1 + i, y1 - 1 + j).xyz;
            row = row + c * wx[i];
            if (i == 1 || i == 2) {
                if (j == 1 || j == 2) {
                    lo = min(lo, c);
                    hi = max(hi, c);
                }
            }
        }
        sum = sum + row * wy[j];
    }
    return max(min(sum, hi), lo);
}

float3 fuseTmSpatialSample(TmTaauFrame f, float px, float py) {
    const float fx = px - 0.5;
    const float fy = py - 0.5;
    const int x1 = int(floor(fx));
    const int y1 = int(floor(fy));
    float wx[4];
    float wy[4];
    fuseTmCatmullRomWeights(fx - float(x1), wx);
    fuseTmCatmullRomWeights(fy - float(y1), wy);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int j = 0; j < 4; ++j) {
        float3 row = float3(0.0, 0.0, 0.0);
        for (int i = 0; i < 4; ++i) {
            row = row + fuseTmColorTexel(f, x1 - 1 + i, y1 - 1 + j) * wx[i];
        }
        sum = sum + row * wy[j];
    }
    return sum;
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const TmTaauFrame f = ((TmTaauFrame*)pc.frame)[0];
    const uint x = tid.x;
    const uint y = tid.y;
    if (x >= f.displayW || y >= f.displayH) {
        return;
    }
    float* depthBuf = (float*)f.depth;
    float2* motionBuf = (float2*)f.motion;
    const uint di = y * f.displayW + x;
    const float rwf = float(f.renderW);
    const float rhf = float(f.renderH);
    const float dwf = float(f.displayW);
    const float dhf = float(f.displayH);
    const float scaleX = dwf / rwf;
    const float scaleY = dhf / rhf;
    const float e = f.exposure;

    const float u = (float(x) + 0.5) / dwf;
    const float v = (float(y) + 0.5) / dhf;
    const float prx = u * rwf;
    const float pry = v * rhf;
    const float qx = prx - f.jitterX;
    const float qy = pry - f.jitterY;
    const int maxRx = int(f.renderW) - 1;
    const int maxRy = int(f.renderH) - 1;
    const int cix = fuseTmClampIndex(int(floor(qx)), maxRx);
    const int ciy = fuseTmClampIndex(int(floor(qy)), maxRy);
    const uint ci = uint(ciy) * f.renderW + uint(cix);

    // 1. Current-frame samples.
    float sumW = 0.0;
    float sumWt = 0.0;
    float3 sumC = float3(0.0, 0.0, 0.0);
    float3 nMin = float3(kFuseTmBig, kFuseTmBig, kFuseTmBig);
    float3 nMax = float3(-kFuseTmBig, -kFuseTmBig, -kFuseTmBig);
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    const float ownDepth = fuseTmEffectiveDepth(depthBuf[ci]);
    float closest = ownDepth;
    uint closestIndex = ci;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int tx = fuseTmClampIndex(cix + dx, maxRx);
            const int ty = fuseTmClampIndex(ciy + dy, maxRy);
            const uint ti = uint(ty) * f.renderW + uint(tx);
            const float3 c = fuseTmSanitize(fuseTmColor(f.color, tx, ty)) * e;
            const float ox = (float(cix + dx) + 0.5 + f.jitterX - prx) * scaleX;
            const float oy = (float(ciy + dy) + 0.5 + f.jitterY - pry) * scaleY;
            const float w = fuseTmSampleWeight(sqrt(ox * ox + oy * oy) * f.sampleKernelScale);
            const float wt = w / (1.0 + fuseTmLuma709(c));
            sumW += w;
            sumWt += wt;
            sumC = sumC + c * wt;
            const float3 yc = fuseTmRgbToYcocg(c);
            nMin = min(nMin, yc);
            nMax = max(nMax, yc);
            m1 = m1 + yc;
            m2 = m2 + yc * yc;
            if (f.dilateMotion != 0u) {
                const float d = fuseTmEffectiveDepth(depthBuf[ti]);
                if (d < closest) {
                    closest = d;
                    closestIndex = ti;
                }
            }
        }
    }
    const float2 motion = closest < ownDepth * (1.0 - f.dilateDepthThreshold) ? motionBuf[closestIndex] : motionBuf[ci];
    float3 spatialY = fuseTmRgbToYcocg(fuseTmSanitize(fuseTmSpatialSample(f, qx, qy)) * e);
    spatialY = max(min(spatialY, nMax), nMin);
    const float3 spatial = fuseTmYcocgToRgb(spatialY);
    const float spatialWt = f.spatialWeight / (1.0 + fuseTmLuma709(spatial));
    const float3 current = (sumC + spatial * spatialWt) * (1.0 / (sumWt + spatialWt));
    const float confidence = sumW + f.spatialWeight;

    const float reactive = f.reactive == 0u ? 0.0 : fuseTmSaturate(fuseTmMask(f.reactive, cix, ciy));
    const float transparency = f.transparency == 0u ? 0.0 : fuseTmSaturate(fuseTmMask(f.transparency, cix, ciy));

    // 2. Reprojection.
    const float mvx = motion.x * dwf;
    const float mvy = motion.y * dhf;
    const float hx = float(x) + 0.5 - mvx;
    const float hy = float(y) + 0.5 - mvy;
    float historyW = 0.0;
    float3 history = float3(0.0, 0.0, 0.0);
    if (f.historyValid != 0u && hx >= 0.0 && hy >= 0.0 && hx < dwf && hy < dhf) {
        const float motionCap = f.maxAccumulation / (1.0 + sqrt(mvx * mvx + mvy * mvy) * f.accumulationMotionFalloff);
        historyW = min(fuseTmSampleHistoryWeight(f, hx, hy), motionCap);
        history = fuseTmSampleHistory(f, hx, hy, f.historyFilter);

        // 3a. Depth disocclusion.
        const float prevRx = (u - motion.x) * rwf;
        const float prevRy = (v - motion.y) * rhf;
        if (f.hasPrev != 0u && f.depthRejection > 0.0) {
            const float d = depthBuf[ci];
            float expected = fuseTmEffectiveDepth(d);
            if (f.hasCamera != 0u && d > 0.0) {
                const float su = (float(cix) + 0.5 + f.jitterX) / rwf;
                const float sv = (float(ciy) + 0.5 + f.jitterY) / rhf;
                const float vx = (2.0 * su - 1.0) * f.tanHalfX * d;
                const float vy = (1.0 - 2.0 * sv) * f.tanHalfY * d;
                const float vz = -d;
                const float4 m = f.curToPrevView[2];
                expected = -(m.x * vx + m.y * vy + m.z * vz + m.w);
            }
            const int pcx = fuseTmClampIndex(int(floor(prevRx)), maxRx);
            const int pcy = fuseTmClampIndex(int(floor(prevRy)), maxRy);
            float* prevDepth = (float*)f.prevDepth;
            float lo = kFuseTmBig;
            float hi = 0.0;
            for (int j = -1; j <= 1; ++j) {
                for (int i = -1; i <= 1; ++i) {
                    const uint pi = uint(fuseTmClampIndex(pcy + j, maxRy)) * f.renderW + uint(fuseTmClampIndex(pcx + i, maxRx));
                    const float dp = fuseTmEffectiveDepth(prevDepth[pi]);
                    lo = min(lo, dp);
                    hi = max(hi, dp);
                }
            }
            if (expected < lo * (1.0 - f.depthRejection) || expected > hi * (1.0 + f.depthRejection)) {
                historyW = 0.0;
            }
        }
        // 3b. Velocity disagreement.
        if (f.hasPrev != 0u && f.velocityRejectionPx > 0.0 && historyW > 0.0) {
            const uint pi = uint(fuseTmClampIndex(int(floor(prevRy)), maxRy)) * f.renderW +
                            uint(fuseTmClampIndex(int(floor(prevRx)), maxRx));
            float2* prevMotion = (float2*)f.prevMotion;
            const float2 pm = prevMotion[pi];
            const float ddx = (motion.x - pm.x) * dwf;
            const float ddy = (motion.y - pm.y) * dhf;
            historyW *= 1.0 - fuseTmSaturate(sqrt(ddx * ddx + ddy * ddy) / f.velocityRejectionPx);
        }
        // 3c. Reactive mask.
        historyW *= 1.0 - fuseTmSaturate(reactive * f.reactiveStrength);
    }

    // 4. Neighbourhood clipping (YCoCg variance box).
    const float3 currentY = fuseTmRgbToYcocg(current);
    float3 resolvedY = currentY;
    if (historyW > 0.0) {
        const float3 historyY = fuseTmRgbToYcocg(history);
        const float3 mean = m1 * (1.0 / 9.0);
        const float3 var = m2 * (1.0 / 9.0) - mean * mean;
        const float3 sigma = float3(sqrt(max(0.0, var.x)), sqrt(max(0.0, var.y)), sqrt(max(0.0, var.z)));
        float3 boxMin = max(nMin, mean - sigma * f.clampGamma);
        float3 boxMax = min(nMax, mean + sigma * f.clampGamma);
        boxMin = min(min(boxMin, boxMax), currentY);
        boxMax = max(max(boxMin, boxMax), currentY);
        const float motionPx = sqrt(mvx * mvx + mvy * mvy);
        const float clipStrength = max(max(fuseTmSaturate(f.staticClipStrength), fuseTmSaturate(motionPx / max(1e-4, f.clipFullMotionPx))),
                                       max(reactive > 0.0 ? 1.0 : 0.0, fuseTmSaturate(transparency * f.transparencyClip)));
        const float3 clipped = fuseTmClipToAabb(historyY, boxMin, boxMax);
        const float3 clippedHistory = fuseTmLerp3(historyY, clipped, clipStrength);
        // 5. Accumulate.
        const float alpha = confidence / (historyW + confidence);
        resolvedY = fuseTmLerp3(clippedHistory, currentY, alpha);
    }
    const float3 resolved = fuseTmSanitize(fuseTmYcocgToRgb(resolvedY));
    const float newW = min(historyW + confidence, f.maxAccumulation);
    float4* historyOut = (float4*)f.historyOut;
    historyOut[di] = float4(resolved, newW);
    const float3 outColor = resolved * (1.0 / e);
    fuse_tm_out_images[f.output_ & 0xFFFFFu][int2(int(x), int(y))] = float4(outColor, 1.0);
    if (pc.out_ != 0ull) {
        float4* dump = (float4*)pc.out_;
        dump[di] = float4(outColor, 0.0);
    }
}
