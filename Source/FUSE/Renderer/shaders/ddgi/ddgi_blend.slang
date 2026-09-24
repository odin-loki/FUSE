// WP-6.1 "ddgi.blend", Slang twin of ddgi_blend.comp (see there).
#include "ddgi_common.slang"

groupshared float4 s_irr[kFuseDdgiMaxIrradianceRes * kFuseDdgiMaxIrradianceRes];
groupshared float2 s_dist[kFuseDdgiMaxDepthRes * kFuseDdgiMaxDepthRes];
groupshared uint s_flags[3];

// copyOctahedralBorder entry e of a res x res tile: destination (row, col) and interior source (row, col).
void fuseDdgiBorderEntry(uint e, uint res, out uint row, out uint col, out uint srcRow, out uint srcCol) {
    const uint last = res + 1u;
    if (e < 4u * res) {
        const uint i = e % res + 1u;
        const uint mirror = last - i;
        const uint side = e / res;
        if (side == 0u) { row = 0u; col = i; srcRow = 1u; srcCol = mirror; }
        else if (side == 1u) { row = last; col = i; srcRow = res; srcCol = mirror; }
        else if (side == 2u) { row = i; col = 0u; srcRow = mirror; srcCol = 1u; }
        else { row = i; col = last; srcRow = mirror; srcCol = res; }
    } else {
        const uint k = e - 4u * res;
        if (k == 0u) { row = 0u; col = 0u; srcRow = res; srcCol = res; }
        else if (k == 1u) { row = 0u; col = last; srcRow = res; srcCol = 1u; }
        else if (k == 2u) { row = last; col = 0u; srcRow = 1u; srcCol = res; }
        else { row = last; col = last; srcRow = 1u; srcCol = 1u; }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid: SV_GroupID, uint3 lid: SV_GroupThreadID) {
    const uint slot = gid.x;
    const uint tid = lid.x;
    const uint threads = 64u;
    const DdgiFrame F = fuseDdgiFrame();
    if (slot >= F.scheduled) {
        return;
    }
    const uint probe = ((uint*)F.schedule)[slot];
    if (probe >= F.volume.probeCount) {
        return;
    }
    const uint ir = F.volume.irradianceRes;
    const uint texels = ir * ir;
    const uint irrStride = ir + 2u;
    const uint irrBase = probe * irrStride * irrStride * 3u;
    const uint dr = F.volume.depthRes;
    const uint distStride = dr + 2u;
    const uint distBase = probe * distStride * distStride * 2u;
    const uint rays = F.raysPerProbe;
    float* irr = (float*)F.volume.irradiance;
    float* dist = (float*)F.volume.distance;
    float4* dirs = (float4*)F.rayDirs;
    float4* results = (float4*)F.rays;
    float4* irrTexelDirs = (float4*)F.irradianceTexelDirs;
    float4* distTexelDirs = (float4*)F.distanceTexelDirs;
    uint* counts = (uint*)F.updateCounts;
    const uint rayBase = slot * rays;

    if (tid == 0u) {
        s_flags[0] = 0u;
        s_flags[1] = 0u;
        s_flags[2] = counts[probe] == 0u ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint t = tid; t < texels; t += threads) {
        const float3 td = irrTexelDirs[t].xyz;
        float sumR = 0.0;
        float sumG = 0.0;
        float sumB = 0.0;
        float weightSum = 0.0;
        for (uint r = 0u; r < rays; ++r) {
            const float3 d = dirs[r].xyz;
            const float w = td.x * d.x + td.y * d.y + td.z * d.z;
            if (w > 0.0) {
                const float3 l = results[rayBase + r].xyz;
                sumR += l.x * w;
                sumG += l.y * w;
                sumB += l.z * w;
                weightSum += w;
            }
        }
        if (weightSum <= 0.0) {
            s_irr[t] = float4(0.0, 0.0, 0.0, 0.0);
            continue;
        }
        const float inv = 1.0 / weightSum;
        s_irr[t] = float4(sumR * inv, sumG * inv, sumB * inv, 1.0);
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid == 0u && s_flags[2] == 0u) {
        float3 inSum = float3(0.0, 0.0, 0.0);
        float3 histSum = float3(0.0, 0.0, 0.0);
        uint valid = 0u;
        for (uint t = 0u; t < texels; ++t) {
            if (s_irr[t].w == 0.0) {
                continue;
            }
            const uint y = t / ir;
            const uint x = t % ir;
            const uint o = irrBase + ((y + 1u) * irrStride + (x + 1u)) * 3u;
            inSum = float3(inSum.x + s_irr[t].x, inSum.y + s_irr[t].y, inSum.z + s_irr[t].z);
            histSum = float3(histSum.x + irr[o + 0u], histSum.y + irr[o + 1u], histSum.z + irr[o + 2u]);
            ++valid;
        }
        if (valid > 0u) {
            const float inv = 1.0 / float(valid);
            const float3 inMean = float3(inSum.x * inv, inSum.y * inv, inSum.z * inv);
            const float3 histMean = float3(histSum.x * inv, histSum.y * inv, histSum.z * inv);
            const float scale = max(max(fuseDdgiMaxComponent(inMean), fuseDdgiMaxComponent(histMean)), F.changeFloor);
            const float3 diff = float3(abs(inMean.x - histMean.x), abs(inMean.y - histMean.y), abs(inMean.z - histMean.z));
            s_flags[0] = fuseDdgiMaxComponent(diff) > F.probeChangeThreshold * scale ? 1u : 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const bool first = s_flags[2] != 0u;
    const bool probeChanged = s_flags[0] != 0u;
    const float hysteresis = first ? 0.0 : F.hysteresis;
    const float fastHysteresis = max(0.0, hysteresis - F.changeHysteresisDrop);
    for (uint t = tid; t < texels; t += threads) {
        const uint y = t / ir;
        const uint x = t % ir;
        const uint o = irrBase + ((y + 1u) * irrStride + (x + 1u)) * 3u;
        const float3 texel = float3(irr[o + 0u], irr[o + 1u], irr[o + 2u]);
        const float4 incoming = s_irr[t];
        if (incoming.w == 0.0) {
            s_irr[t] = float4(texel, 0.0);
            continue;
        }
        const float3 inc = incoming.xyz;
        float h = hysteresis;
        if (!first) {
            const float scale = max(max(fuseDdgiMaxComponent(texel), fuseDdgiMaxComponent(inc)), F.changeFloor);
            const float3 diff = float3(abs(inc.x - texel.x), abs(inc.y - texel.y), abs(inc.z - texel.z));
            if (probeChanged) {
                h = min(hysteresis, F.probeChangeHysteresis);
                InterlockedAdd(s_flags[1], 1u);
            } else if (fuseDdgiMaxComponent(diff) > F.changeThreshold * scale) {
                h = fastHysteresis;
                InterlockedAdd(s_flags[1], 1u);
            }
        }
        const float keep = 1.0 - h;
        const float3 blended = float3(inc.x * keep + texel.x * h, inc.y * keep + texel.y * h, inc.z * keep + texel.z * h);
        irr[o + 0u] = blended.x;
        irr[o + 1u] = blended.y;
        irr[o + 2u] = blended.z;
        s_irr[t] = float4(blended, 0.0);
    }
    for (uint t = tid; t < dr * dr; t += threads) {
        const uint o = distBase + (((t / dr) + 1u) * distStride + ((t % dr) + 1u)) * 2u;
        const float2 texel = float2(dist[o + 0u], dist[o + 1u]);
        const float3 td = distTexelDirs[t].xyz;
        float sumD = 0.0;
        float sumD2 = 0.0;
        float weightSum = 0.0;
        for (uint r = 0u; r < rays; ++r) {
            const float3 d = dirs[r].xyz;
            const float c = td.x * d.x + td.y * d.y + td.z * d.z;
            if (c > F.distanceMinCos) {
                const float w = pow(c, F.distancePower);
                const float dd = min(results[rayBase + r].w, F.maxRayDistance);
                sumD += dd * w;
                sumD2 += dd * dd * w;
                weightSum += w;
            }
        }
        if (weightSum <= 1e-12) {
            s_dist[t] = texel;
            continue;
        }
        const float keep = 1.0 - hysteresis;
        const float2 incoming = float2(sumD / weightSum, sumD2 / weightSum);
        const float2 blended = float2(incoming.x * keep + texel.x * hysteresis, incoming.y * keep + texel.y * hysteresis);
        dist[o + 0u] = blended.x;
        dist[o + 1u] = blended.y;
        s_dist[t] = blended;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint e = tid; e < 4u * ir + 4u; e += threads) {
        uint row, col, srcRow, srcCol;
        fuseDdgiBorderEntry(e, ir, row, col, srcRow, srcCol);
        const float3 v = s_irr[(srcRow - 1u) * ir + (srcCol - 1u)].xyz;
        const uint o = irrBase + (row * irrStride + col) * 3u;
        irr[o + 0u] = v.x;
        irr[o + 1u] = v.y;
        irr[o + 2u] = v.z;
    }
    for (uint e = tid; e < 4u * dr + 4u; e += threads) {
        uint row, col, srcRow, srcCol;
        fuseDdgiBorderEntry(e, dr, row, col, srcRow, srcCol);
        const float2 v = s_dist[(srcRow - 1u) * dr + (srcCol - 1u)];
        const uint o = distBase + (row * distStride + col) * 2u;
        dist[o + 0u] = v.x;
        dist[o + 1u] = v.y;
    }
    if (tid == 0u) {
        counts[probe] = counts[probe] + 1u;
        ((uint*)F.slotStats)[slot] = s_flags[1];
    }
}
