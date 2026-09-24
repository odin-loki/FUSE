// WP-6.1 "ddgi.trace", Slang twin of ddgi_trace.comp (see there): FUSE_DDGI_RAY_QUERY selects the T2
// ray-query permutation, otherwise T0 sphere tracing of the global SDF.
#include "ddgi_common.slang"

bool fuseDdgiSunOccluded(DdgiFrame F, float3 position, float3 normal);

float3 fuseDdgiHitRadiance(DdgiFrame F, float3 position, float3 normal, float3 albedo, float3 emissive) {
    float3 radiance = emissive;
    const float3 sunDir = fuseDdgiVec3(F.sunDirection);
    const float3 sunIrr = fuseDdgiVec3(F.sunIrradiance);
    const float cosSun = normal.x * sunDir.x + normal.y * sunDir.y + normal.z * sunDir.z;
    if (cosSun > 0.0 && fuseDdgiMaxComponent(sunIrr) > 0.0 && !fuseDdgiSunOccluded(F, position, normal)) {
        const float k = cosSun * kFuseDdgiInvPi;
        radiance = float3(radiance.x + (albedo.x * sunIrr.x) * k, radiance.y + (albedo.y * sunIrr.y) * k,
                          radiance.z + (albedo.z * sunIrr.z) * k);
    }
    if ((F.flags & kFuseDdgiFlagMultiBounce) != 0u) {
        const float3 e = fuseDdgiSampleVolume(F.volume, position, normal);
        radiance = float3(radiance.x + (albedo.x * e.x) * kFuseDdgiInvPi, radiance.y + (albedo.y * e.y) * kFuseDdgiInvPi,
                          radiance.z + (albedo.z * e.z) * kFuseDdgiInvPi);
    }
    return radiance;
}

#ifdef FUSE_DDGI_RAY_QUERY
// --- T2: ray query against the TLAS -------------------------------------------------------------------
struct DdgiInstance { // gpu_scene::GpuInstance
    uint mesh;
    uint material;
    uint flags;
    uint generation;
    uint entityIndex;
    uint entityGeneration;
    uint userData;
    uint reserved;
};
struct DdgiTransform { // gpu_scene::GpuTransform
    float4 rows[3];
};
struct DdgiMesh { // gpu_scene::GpuMesh, 144 bytes
    uint64_t meshlets;
    uint64_t meshletVertices;
    uint64_t meshletTriangles;
    uint64_t positions;
    uint64_t normals;
    uint64_t tangents;
    uint64_t uvs;
    uint64_t submeshes;
    uint meshletCount;
    uint submeshCount;
    uint vertexCount;
    uint triangleCount;
    float quantOffset[3];
    float quantStep[3];
    float boundsCenter[3];
    float boundsRadius;
    uint geometryHandle;
    uint flags;
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint reserved;
};
struct DdgiSceneHeader { // GpuSceneHeader, 144 bytes
    uint64_t addresses[6];
    uint counts[6];
    uint handles[6];
    uint capacities[6];
    uint liveInstances;
    uint liveLights;
    uint magic;
    uint version;
    uint64_t indexAddress;
};

bool fuseDdgiSunOccluded(DdgiFrame F, float3 position, float3 normal) {
    RayDesc ray;
    ray.Origin = float3(position.x + normal.x * F.rayEpsilon, position.y + normal.y * F.rayEpsilon,
                        position.z + normal.z * F.rayEpsilon);
    ray.TMin = 0.0;
    ray.Direction = fuseDdgiVec3(F.sunDirection);
    ray.TMax = 1.0e30;
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(F.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, F.shadowMask & 0x7Fu, ray);
    while (q.Proceed()) {
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 fuseDdgiVertex(DdgiMesh mesh, DdgiTransform xf, uint v) {
    uint* vpos = (uint*)mesh.positions;
    const uint w0 = vpos[v * 2u + 0u];
    const uint w1 = vpos[v * 2u + 1u];
    const float3 p = float3(mesh.quantOffset[0] + float(w0 & 0xFFFFu) * mesh.quantStep[0],
                            mesh.quantOffset[1] + float(w0 >> 16u) * mesh.quantStep[1],
                            mesh.quantOffset[2] + float(w1 & 0xFFFFu) * mesh.quantStep[2]);
    return float3(xf.rows[0].x * p.x + xf.rows[0].y * p.y + xf.rows[0].z * p.z + xf.rows[0].w,
                  xf.rows[1].x * p.x + xf.rows[1].y * p.y + xf.rows[1].z * p.z + xf.rows[1].w,
                  xf.rows[2].x * p.x + xf.rows[2].y * p.y + xf.rows[2].z * p.z + xf.rows[2].w);
}

float4 fuseDdgiTrace(DdgiFrame F, float3 origin, float3 dir) {
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.0;
    ray.Direction = dir;
    ray.TMax = F.maxRayDistance;
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(F.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE, F.traceMask & 0x7Fu, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        return float4(fuseDdgiVec3(F.skyRadiance), F.maxRayDistance);
    }
    const float t = q.CommittedRayT();
    const uint slot = q.CommittedInstanceID();
    const uint prim = q.CommittedPrimitiveIndex();
    const DdgiSceneHeader scene = ((DdgiSceneHeader*)F.scene)[0];
    const DdgiInstance inst = ((DdgiInstance*)scene.addresses[0])[slot];
    const DdgiTransform xf = ((DdgiTransform*)scene.addresses[1])[slot];
    const DdgiMesh mesh = ((DdgiMesh*)scene.addresses[3])[inst.mesh];
    uint* indices = (uint*)scene.indexAddress;
    const uint i0 = mesh.firstIndex + prim * 3u;
    const float3 p0 = fuseDdgiVertex(mesh, xf, uint(int(indices[i0 + 0u]) + mesh.vertexOffset));
    const float3 p1 = fuseDdgiVertex(mesh, xf, uint(int(indices[i0 + 1u]) + mesh.vertexOffset));
    const float3 p2 = fuseDdgiVertex(mesh, xf, uint(int(indices[i0 + 2u]) + mesh.vertexOffset));
    const float3 e1 = float3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
    const float3 e2 = float3(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
    const float3 c = float3(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
    float3 n = fuseDdgiNormalized(c);
    if ((F.flags & kFuseDdgiFlagFrontCcw) == 0u) {
        n = -n;
    }
    const float facing = n.x * dir.x + n.y * dir.y + n.z * dir.z;
    if (n.x == 0.0 && n.y == 0.0 && n.z == 0.0) {
        n = -dir;
    } else if (facing > 0.0) {
        return float4(0.0, 0.0, 0.0, t * F.backfaceDistanceScale);
    }
    float3 albedo = float3(0.8, 0.8, 0.8);
    float3 emissive = float3(0.0, 0.0, 0.0);
    if (inst.material != 0xFFFFFFFFu && inst.material < scene.counts[4]) {
        float* m = (float*)scene.addresses[4];
        const uint b = inst.material * 32u;
        const float dielectric = 1.0 - clamp(m[b + 3u], 0.0, 1.0);
        albedo = float3(m[b + 0u] * dielectric, m[b + 1u] * dielectric, m[b + 2u] * dielectric);
        const float intensity = max(m[b + 14u], 0.0);
        emissive = float3(m[b + 5u] * intensity, m[b + 6u] * intensity, m[b + 7u] * intensity);
    }
    const float3 position = float3(origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t);
    return float4(fuseDdgiHitRadiance(F, position, n, albedo, emissive), t);
}

#else
// --- T0: sphere tracing of the global SDF (compute::ray_march_kernel semantics) ------------------------
static const float kFuseDdgiSdfFar = 1.0e30;
static const float kFuseDdgiSdfShadowMax = 1.0e4;

float3 fuseDdgiSphereGradient(float3 p) {
    const float len = fuseDdgiLength(p);
    if (len < 1e-8) {
        return float3(1.0, 0.0, 0.0);
    }
    const float inv = 1.0 / len;
    return float3(p.x * inv, p.y * inv, p.z * inv);
}

float fuseDdgiObjectSdf(DdgiSdfObject o, float3 local, bool wantGrad, out float3 grad) {
    grad = float3(0.0, 0.0, 0.0);
    if (o.type == 0u) {
        if (wantGrad) {
            grad = fuseDdgiSphereGradient(local);
        }
        return fuseDdgiLength(local) - o.params[0];
    }
    if (o.type == 1u) {
        const float smallest = min(o.params[0], min(o.params[1], o.params[2]));
        const float r = clamp(o.rounding, 0.0, max(smallest, 0.0));
        const float3 h = float3(o.params[0] - r, o.params[1] - r, o.params[2] - r);
        const float3 q = float3(abs(local.x) - h.x, abs(local.y) - h.y, abs(local.z) - h.z);
        const float3 outer = float3(max(q.x, 0.0), max(q.y, 0.0), max(q.z, 0.0));
        const float outside = fuseDdgiLength(outer);
        if (wantGrad) {
            const float3 s = float3(local.x < 0.0 ? -1.0 : 1.0, local.y < 0.0 ? -1.0 : 1.0, local.z < 0.0 ? -1.0 : 1.0);
            if (outside > 0.0) {
                grad = float3(s.x * outer.x / outside, s.y * outer.y / outside, s.z * outer.z / outside);
            } else if (q.x >= q.y && q.x >= q.z) {
                grad = float3(s.x, 0.0, 0.0);
            } else if (q.y >= q.z) {
                grad = float3(0.0, s.y, 0.0);
            } else {
                grad = float3(0.0, 0.0, s.z);
            }
        }
        const float inside = min(max(q.x, max(q.y, q.z)), 0.0);
        return outside + inside - r;
    }
    if (o.type == 2u) {
        const float hh = max(o.params[1], 0.0);
        const float3 v = float3(local.x, local.y - clamp(local.y, -hh, hh), local.z);
        if (wantGrad) {
            grad = fuseDdgiSphereGradient(v);
        }
        return fuseDdgiLength(v) - o.params[0];
    }
    if (o.type == 3u) {
        const float radial = sqrt(local.x * local.x + local.z * local.z);
        const float ring = radial - o.params[0];
        const float tube = sqrt(ring * ring + local.y * local.y);
        if (wantGrad) {
            if (tube < 1e-8) {
                grad = float3(1.0, 0.0, 0.0);
            } else {
                const float rx = radial > 1e-8 ? local.x / radial : 1.0;
                const float rz = radial > 1e-8 ? local.z / radial : 0.0;
                const float s = ring / tube;
                grad = float3(s * rx, local.y / tube, s * rz);
            }
        }
        return tube - o.params[1];
    }
    return kFuseDdgiSdfFar;
}

float fuseDdgiSceneEval(DdgiFrame F, float3 p, bool wantGrad, out float3 gradient) {
    DdgiSdfObject* objects = (DdgiSdfObject*)F.sdfObjects;
    float distance = kFuseDdgiSdfFar;
    float3 g = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < F.sdfCount; ++i) {
        const DdgiSdfObject o = objects[i];
        const float3 local = float3(p.x - o.position[0], p.y - o.position[1], p.z - o.position[2]);
        float3 og;
        const float od = fuseDdgiObjectSdf(o, local, wantGrad, og);
        if (o.alpha > 0.0) {
            const float h = max(o.alpha - abs(distance - od), 0.0) / o.alpha;
            if (wantGrad) {
                const float wMin = 1.0 - 0.5 * h;
                const float wMax = 0.5 * h;
                if (distance <= od) {
                    g = float3(g.x * wMin + og.x * wMax, g.y * wMin + og.y * wMax, g.z * wMin + og.z * wMax);
                } else {
                    g = float3(g.x * wMax + og.x * wMin, g.y * wMax + og.y * wMin, g.z * wMax + og.z * wMin);
                }
            }
            distance = min(distance, od) - h * h * o.alpha * 0.25;
        } else {
            if (od < distance) {
                g = og;
            }
            distance = min(distance, od);
        }
    }
    gradient = g;
    return distance;
}

float fuseDdgiSdf(DdgiFrame F, float3 p) {
    float3 unused;
    return fuseDdgiSceneEval(F, p, false, unused);
}

uint fuseDdgiSdfMaterial(DdgiFrame F, float3 p) {
    DdgiSdfObject* objects = (DdgiSdfObject*)F.sdfObjects;
    float best = kFuseDdgiSdfFar;
    uint material = 0xFFFFFFFFu;
    for (uint i = 0u; i < F.sdfCount; ++i) {
        const DdgiSdfObject o = objects[i];
        const float3 local = float3(p.x - o.position[0], p.y - o.position[1], p.z - o.position[2]);
        float3 unused;
        const float od = fuseDdgiObjectSdf(o, local, false, unused);
        if (od < best) {
            best = od;
            material = o.materialId;
        }
    }
    return material;
}

bool fuseDdgiSunOccluded(DdgiFrame F, float3 position, float3 normal) {
    const float3 origin = float3(position.x + normal.x * F.sdfShadowBias, position.y + normal.y * F.sdfShadowBias,
                                 position.z + normal.z * F.sdfShadowBias);
    const float3 dir = fuseDdgiVec3(F.sunDirection);
    float t = 0.0;
    for (uint step = 0u; step < F.sdfMaxSteps; ++step) {
        if (t >= kFuseDdgiSdfShadowMax) {
            return false;
        }
        const float3 p = float3(origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t);
        const float d = fuseDdgiSdf(F, p);
        if (d < F.sdfMinDistance) {
            return true;
        }
        t += d;
    }
    return t < kFuseDdgiSdfShadowMax;
}

float4 fuseDdgiTrace(DdgiFrame F, float3 origin, float3 dir) {
    const float maxD = F.maxRayDistance;
    const float d0 = fuseDdgiSdf(F, origin);
    if (d0 < 0.0) {
        float t = 0.0;
        for (uint step = 0u; step < F.sdfMaxSteps; ++step) {
            const float3 p = float3(origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t);
            const float d = fuseDdgiSdf(F, p);
            if (d >= 0.0 || t >= maxD) {
                break;
            }
            t += max(-d, F.sdfMinDistance);
        }
        return float4(0.0, 0.0, 0.0, min(t, maxD) * F.backfaceDistanceScale);
    }
    float t = 0.0;
    bool hit = false;
    for (uint step = 0u; step < F.sdfMaxSteps; ++step) {
        if (t >= maxD) {
            break;
        }
        const float3 p = float3(origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t);
        const float d = fuseDdgiSdf(F, p);
        if (d < F.sdfMinDistance) {
            hit = true;
            break;
        }
        t += d;
    }
    if (!hit && t < maxD) {
        hit = true;
    }
    if (!hit) {
        return float4(fuseDdgiVec3(F.skyRadiance), maxD);
    }
    const float3 position = float3(origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t);
    float3 gradient;
    const float surfaceDistance = fuseDdgiSceneEval(F, position, true, gradient);
    const float glen = fuseDdgiLength(gradient);
    float3 n = float3(0.0, 1.0, 0.0);
    if (glen > 1e-12) {
        const float inv = 1.0 / glen;
        n = float3(gradient.x * inv, gradient.y * inv, gradient.z * inv);
    }
    float3 albedo = float3(0.8, 0.8, 0.8);
    float3 emissive = float3(0.0, 0.0, 0.0);
    const uint material = fuseDdgiSdfMaterial(F, position);
    if (material < F.sdfSurfaceCount && F.sdfSurfaces != 0) {
        const DdgiSurfaceRow s = ((DdgiSurfaceRow*)F.sdfSurfaces)[material];
        albedo = s.albedo.xyz;
        emissive = s.emissive.xyz;
    }
    return float4(fuseDdgiHitRadiance(F, position, n, albedo, emissive), t);
}
#endif

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint ray = tid.x;
    const uint slot = tid.y;
    const DdgiFrame F = fuseDdgiFrame();
    if (ray >= F.raysPerProbe || slot >= F.scheduled) {
        return;
    }
    const uint probe = ((uint*)F.schedule)[slot];
    if (probe >= F.volume.probeCount) {
        return;
    }
    const float3 origin = fuseDdgiProbePosition(F.volume, probe);
    const float3 dir = ((float4*)F.rayDirs)[ray].xyz;
    ((float4*)F.rays)[slot * F.raysPerProbe + ray] = fuseDdgiTrace(F, origin, dir);
}
