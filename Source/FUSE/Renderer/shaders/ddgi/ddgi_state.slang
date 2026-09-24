// WP-6.1 "ddgi.state", Slang twin of ddgi_state.comp (see there): probe relocation + classification, the
// oracle's ddgi_kernel::update_probe_state. -fp-mode precise.
#include "ddgi_common.slang"

static const float kFuseDdgiStateFar = 1e27;
static const uint kFuseDdgiStateNone = 0xFFFFFFFFu;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint slot = tid.x;
    const DdgiFrame F = fuseDdgiFrame();
    if (slot >= F.scheduled || F.volume.probeData == 0) {
        return;
    }
    const uint probe = ((uint*)F.schedule)[slot];
    if (probe >= F.volume.probeCount) {
        return;
    }
    float4* data = (float4*)F.volume.probeData;
    float4* dirs = (float4*)F.rayDirs;
    float4* results = (float4*)F.rays;
    const uint rays = F.raysPerProbe;
    const uint rayBase = slot * rays;
    const float4 current = data[probe];
    float3 offset = current.xyz;
    float state = current.w;

    uint closestBack = kFuseDdgiStateNone;
    uint closestFront = kFuseDdgiStateNone;
    uint farthestFront = kFuseDdgiStateNone;
    float closestBackDistance = kFuseDdgiStateFar;
    float closestFrontDistance = kFuseDdgiStateFar;
    float farthestFrontDistance = 0.0;
    uint backfaces = 0u;
    const float undo = F.backfaceDistanceScale > 0.0 ? 1.0 / F.backfaceDistanceScale : 1.0;
    for (uint r = 0u; r < rays; ++r) {
        const float d = results[rayBase + r].w;
        if (d < 0.0) {
            ++backfaces;
            const float full = -d * undo;
            if (full < closestBackDistance) {
                closestBackDistance = full;
                closestBack = r;
            }
        } else if (d < closestFrontDistance) {
            closestFrontDistance = d;
            closestFront = r;
        } else if (d > farthestFrontDistance) {
            farthestFrontDistance = d;
            farthestFront = r;
        }
    }
    const float ratio = rays > 0u ? float(backfaces) / float(rays) : 0.0;
    const bool inside = ratio > F.probeBackfaceThreshold;
    const float minFront = F.probeMinFrontfaceDistance;

    if ((F.flags & kFuseDdgiFlagRelocation) != 0u) {
        bool moved = false;
        float3 full = float3(0.0, 0.0, 0.0);
        if (closestBack != kFuseDdgiStateNone && inside) {
            const float step = closestBackDistance + minFront * 0.5;
            const float3 b = dirs[closestBack].xyz;
            full = float3(offset.x + b.x * step, offset.y + b.y * step, offset.z + b.z * step);
            moved = true;
        } else if (closestFrontDistance < minFront) {
            if (closestFront != kFuseDdgiStateNone && farthestFront != kFuseDdgiStateNone) {
                const float3 a = dirs[closestFront].xyz;
                const float3 b = dirs[farthestFront].xyz;
                const float c = a.x * b.x + a.y * b.y + a.z * b.z;
                if (c <= 0.0) {
                    const float step = min(farthestFrontDistance, F.probeRelocationStep);
                    full = float3(offset.x + b.x * step, offset.y + b.y * step, offset.z + b.z * step);
                    moved = true;
                }
            }
        } else if (closestFrontDistance > minFront) {
            const float len2 = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
            const float len = sqrt(len2);
            if (len > 0.0) {
                const float margin = min(closestFrontDistance - minFront, len);
                const float inv = 1.0 / len;
                const float3 back = float3(-offset.x * inv, -offset.y * inv, -offset.z * inv);
                full = float3(offset.x + back.x * margin, offset.y + back.y * margin, offset.z + back.z * margin);
                moved = true;
            }
        }
        if (moved) {
            const float3 n = float3(full.x / F.volume.spacing[0], full.y / F.volume.spacing[1], full.z / F.volume.spacing[2]);
            if (n.x * n.x + n.y * n.y + n.z * n.z < F.probeMaxOffset * F.probeMaxOffset) {
                offset = full;
            }
        }
    }

    if ((F.flags & kFuseDdgiFlagClassification) != 0u) {
        state = 1.0;
        if (!inside) {
            for (uint r = 0u; r < rays; ++r) {
                const float d = results[rayBase + r].w;
                if (d < 0.0 || d >= F.maxRayDistance) {
                    continue;
                }
                const float3 dir = dirs[r].xyz;
                const float ax = abs(dir.x);
                const float ay = abs(dir.y);
                const float az = abs(dir.z);
                const float px = ax > 0.0 ? F.volume.spacing[0] / max(ax, 1e-6) : kFuseDdgiStateFar;
                const float py = ay > 0.0 ? F.volume.spacing[1] / max(ay, 1e-6) : kFuseDdgiStateFar;
                const float pz = az > 0.0 ? F.volume.spacing[2] / max(az, 1e-6) : kFuseDdgiStateFar;
                if (d <= min(px, min(py, pz))) {
                    state = 0.0;
                    break;
                }
            }
        }
    }
    data[probe] = float4(offset, state);
}
