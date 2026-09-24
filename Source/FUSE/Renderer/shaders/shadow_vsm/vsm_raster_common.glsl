// WP-3.2: the include chain of every WP-3.2 GLSL kernel (bindless heap, GPU scene, the WP-3.1 VSM
// records, the WP-3.2 shadow records / lookup) + the push constants (VsmRasterPush), the raster work
// words (bindless storage buffers, atomics) and the rasteriser math of vsm_raster_kernel.hpp
// (raster_math::setup_tri / tri_texel / clip_project / caster_bounds / box_sphere, same expressions
// in the same order, `precise`). Twin: vsm_raster_common.slang.
#ifndef FUSE_VSM_RASTER_COMMON_GLSL
#define FUSE_VSM_RASTER_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vsm_shadow.glsl"

layout(push_constant) uniform FuseVsmrPush {
    uint64_t shadow; // BDA of VsmShadowConstants
    uint64_t in_;    // vsm.probe: VsmProbeInput[count]
    uint64_t out_;   // vsm.probe: VsmProbeOutput[count]
    uint count;
    uint arg;
} pc;

FUSE_BINDLESS_SSBO_LAYOUT buffer FuseVsmrWords { uint w[]; } fuse_vsmr_words[];
FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(r32ui) uniform uimage2D fuse_vsmr_images[];

#define FUSE_VSMR_WORDS(handle) fuse_vsmr_words[fuse_handle_index(handle)].w
#define FUSE_VSMR_THREADS 64u
#define FUSE_VSMR_BIG_TEXELS 256
#define FUSE_VSMR_WORD_LOCAL_X 0u
#define FUSE_VSMR_WORD_DIR_PAGES 3u
#define FUSE_VSMR_WORD_LOCAL_PAGES 4u
#define FUSE_VSMR_WORD_DIRTY 5u
#define FUSE_VSMR_WORD_DIR_TRIANGLES 6u
#define FUSE_VSMR_WORD_DIR_BIG 7u
#define FUSE_VSMR_WORD_LOCAL_TRIANGLES 8u
#define FUSE_VSMR_WORD_LOCAL_LIST 16u
#define FUSE_VSMR_MAX_LOCAL_PAGES 256u
#define FUSE_VSMR_MAX_AREA 3.0e38

FuseVsmShadowRef fuse_vsmr_shadow() { return FuseVsmShadowRef(pc.shadow); }

// --- vertices -------------------------------------------------------------------------------------
// decode_kernel::mesh_position (exact dequantisation).
vec3 fuse_vsmr_mesh_position(FuseGpuMesh mesh, uint vertex) {
    FuseVsmrWordsRef words = FuseVsmrWordsRef(mesh.positions);
    const uint xy = words.v[vertex * 2u];
    const uint zw = words.v[vertex * 2u + 1u];
    precise vec3 p;
    p.x = mesh.quantOffset[0] + float(xy & 0xFFFFu) * mesh.quantStep[0];
    p.y = mesh.quantOffset[1] + float(xy >> 16u) * mesh.quantStep[1];
    p.z = mesh.quantOffset[2] + float(zw & 0xFFFFu) * mesh.quantStep[2];
    return p;
}

// raster_math::world_point.
vec3 fuse_vsmr_world_point(FuseGpuTransform t, vec3 p) {
    precise vec3 w;
    w.x = ((t.rows[0].x * p.x + t.rows[0].y * p.y) + t.rows[0].z * p.z) + t.rows[0].w;
    w.y = ((t.rows[1].x * p.x + t.rows[1].y * p.y) + t.rows[1].z * p.z) + t.rows[1].w;
    w.z = ((t.rows[2].x * p.x + t.rows[2].y * p.y) + t.rows[2].z * p.z) + t.rows[2].w;
    return w;
}

// World vertex k of triangle `tri` of a caster; false when an index is out of range.
bool fuse_vsmr_triangle_world(FuseGpuSceneHeaderRef scene, FuseGpuMesh mesh, FuseGpuTransform xf, uint tri, out vec3 w[3]) {
    w[0] = vec3(0.0);
    w[1] = vec3(0.0);
    w[2] = vec3(0.0);
    FuseGpuSceneIndicesRef indices = fuse_gpu_scene_indices(scene);
    const uint base = mesh.firstIndex + tri * 3u;
    for (uint k = 0u; k < 3u; ++k) {
        const uint v = uint(int(indices.v[base + k]) + mesh.vertexOffset);
        if (v >= mesh.vertexCount) {
            return false;
        }
        w[k] = fuse_vsmr_world_point(xf, fuse_vsmr_mesh_position(mesh, v));
    }
    return true;
}

// raster_math::caster_bounds.
bool fuse_vsmr_caster_bounds(FuseGpuInstance inst, FuseGpuMesh mesh, FuseGpuTransform t, out vec3 c, out vec3 e) {
    c = vec3(0.0);
    e = vec3(0.0);
    const uint need = FUSE_INSTANCE_VALID | FUSE_INSTANCE_CAST_SHADOW;
    if ((inst.flags & need) != need || mesh.indexCount == 0u || mesh.positions == 0ul) { // + the stream (CPU: its view)
        return false;
    }
    precise float rad = mesh.boundsRadius + ((abs(mesh.quantStep[0]) + abs(mesh.quantStep[1])) + abs(mesh.quantStep[2]));
    for (uint k = 0u; k < 3u; ++k) {
        const vec4 row = t.rows[k];
        precise float center = ((row.x * mesh.boundsCenter[0] + row.y * mesh.boundsCenter[1]) + row.z * mesh.boundsCenter[2]) + row.w;
        precise float extent = rad * ((abs(row.x) + abs(row.y)) + abs(row.z));
        c[k] = center;
        e[k] = extent;
    }
    return true;
}

// raster_math::box_sphere.
bool fuse_vsmr_box_sphere(vec3 c, vec3 e, vec3 p, float radius) {
    precise float dx = max(abs(c.x - p.x) - e.x, 0.0);
    precise float dy = max(abs(c.y - p.y) - e.y, 0.0);
    precise float dz = max(abs(c.z - p.z) - e.z, 0.0);
    precise float d2 = (dx * dx + dy * dy) + dz * dz;
    precise float r2 = radius * radius;
    return d2 <= r2;
}

// raster_math::dir_page_of: level and absolute page of a render-list virtual page.
void fuse_vsmr_dir_page_of(FuseVsmConstantsRef C, uint vp, out uint level, out int ax, out int ay) {
    level = vp / FUSE_VSM_PAGES_PER_LEVEL;
    const uint inLevel = vp % FUSE_VSM_PAGES_PER_LEVEL;
    const uint sx = inLevel % FUSE_VSM_PAGES_PER_AXIS;
    const uint sy = inLevel / FUSE_VSM_PAGES_PER_AXIS;
    const FuseVsmLevel L = C.level[level < 16u ? level : 0u];
    const int half_ = int(FUSE_VSM_PAGES_PER_AXIS / 2u);
    const int bx = L.originX - half_;
    const int by = L.originY - half_;
    ax = bx + int(uint(int(sx) - bx) & (FUSE_VSM_PAGES_PER_AXIS - 1u));
    ay = by + int(uint(int(sy) - by) & (FUSE_VSM_PAGES_PER_AXIS - 1u));
}

// raster_math::dir_page_vertex (x, y texel coordinates; z depth).
vec3 fuse_vsmr_dir_page_vertex(FuseVsmLevel L, int ax, int ay, vec3 l) {
    precise float x = (l.x * L.invPageWorld - float(ax)) * float(FUSE_VSM_PAGE_TEXELS);
    precise float y = (l.y * L.invPageWorld - float(ay)) * float(FUSE_VSM_PAGE_TEXELS);
    return vec3(x, y, fuse_vsmr_dir_depth(L, l.z));
}

// --- triangle rasteriser --------------------------------------------------------------------------
// x, y texel coordinates; directional: z = vertex depths; local: z = view-plane normal, h = offset.
struct FuseVsmrTri {
    vec3 x;
    vec3 y;
    vec3 z;
    float h;
};

struct FuseVsmrSetup {
    vec3 ex;
    vec3 ey;
    float invArea;
    float dz1;
    float dz2;
    ivec4 box; // x0, y0, x1, y1 (inclusive)
};

bool fuse_vsmr_setup(FuseVsmrTri t, out FuseVsmrSetup s) {
    precise vec3 ex = vec3(t.x[2] - t.x[1], t.x[0] - t.x[2], t.x[1] - t.x[0]);
    precise vec3 ey = vec3(t.y[2] - t.y[1], t.y[0] - t.y[2], t.y[1] - t.y[0]);
    s.ex = ex;
    s.ey = ey;
    s.invArea = 0.0;
    s.dz1 = 0.0;
    s.dz2 = 0.0;
    s.box = ivec4(0, 0, -1, -1);
    precise float area = ex[2] * (t.y[2] - t.y[0]) - ey[2] * (t.x[2] - t.x[0]);
    if (!(abs(area) > 0.0) || !(abs(area) < FUSE_VSMR_MAX_AREA)) {
        return false;
    }
    precise float invArea = 1.0 / area;
    precise float dz1 = t.z[1] - t.z[0];
    precise float dz2 = t.z[2] - t.z[0];
    s.invArea = invArea;
    s.dz1 = dz1;
    s.dz2 = dz2;
    const float minX = min(min(t.x[0], t.x[1]), t.x[2]);
    const float maxX = max(max(t.x[0], t.x[1]), t.x[2]);
    const float minY = min(min(t.y[0], t.y[1]), t.y[2]);
    const float maxY = max(max(t.y[0], t.y[1]), t.y[2]);
    precise float mx0 = minX - 0.5;
    precise float mx1 = maxX - 0.5;
    precise float my0 = minY - 0.5;
    precise float my1 = maxY - 0.5;
    const float fx0 = max(ceil(mx0), 0.0);
    const float fx1 = min(floor(mx1), 127.0);
    const float fy0 = max(ceil(my0), 0.0);
    const float fy1 = min(floor(my1), 127.0);
    if (!(fx0 <= fx1) || !(fy0 <= fy1)) {
        return false;
    }
    s.box = ivec4(int(fx0), int(fy0), int(fx1), int(fy1));
    return true;
}

bool fuse_vsmr_texel(FuseVsmrTri t, FuseVsmrSetup s, int px, int py, out float z) {
    z = 0.0;
    precise float cx = float(px) + 0.5;
    precise float cy = float(py) + 0.5;
    precise float w0 = s.ex[0] * (cy - t.y[1]) - s.ey[0] * (cx - t.x[1]);
    precise float w1 = s.ex[1] * (cy - t.y[2]) - s.ey[1] * (cx - t.x[2]);
    precise float w2 = s.ex[2] * (cy - t.y[0]) - s.ey[2] * (cx - t.x[0]);
    precise float b0 = w0 * s.invArea;
    precise float b1 = w1 * s.invArea;
    precise float b2 = w2 * s.invArea;
    if (!(b0 >= 0.0) || !(b1 >= 0.0) || !(b2 >= 0.0)) {
        return false;
    }
    precise float zi = (t.z[0] + b1 * s.dz1) + b2 * s.dz2;
    z = zi;
    return true;
}

float fuse_vsmr_clamp_depth(float d) {
    const float lo = d > 0.0 ? d : 0.0;
    return lo < 1.0 ? lo : 1.0;
}

// raster_math::persp_depth.
float fuse_vsmr_persp_depth(FuseVsmrTri t, FuseVsmLocal e, int px, int py) {
    precise float qx = ((float(px) + 0.5 - 64.0) * (1.0 / 64.0)) / e.invTanHalf;
    precise float qy = ((float(py) + 0.5 - 64.0) * (1.0 / 64.0)) / e.invTanHalf;
    precise float den = (t.z[0] * qx + t.z[1] * qy) + t.z[2];
    precise float z = t.h / den;
    precise float d = z * e.invRange;
    return z > 0.0 ? fuse_vsmr_clamp_depth(d) : 1.0;
}

// Rasterises one texel of a directional page triangle into page `origin` of image `image` (atomic min).
void fuse_vsmr_write_dir(uint image, ivec2 origin, FuseVsmrTri t, FuseVsmrSetup s, int px, int py) {
    float z;
    if (fuse_vsmr_texel(t, s, px, py, z)) {
        imageAtomicMin(fuse_vsmr_images[image], origin + ivec2(px, py), floatBitsToUint(fuse_vsmr_clamp_depth(z)));
    }
}

// The same for a local page (depth from the view plane of the light's face).
void fuse_vsmr_write_local(uint image, ivec2 origin, FuseVsmrTri t, FuseVsmrSetup s, int px, int py, FuseVsmLocal e) {
    float z;
    if (fuse_vsmr_texel(t, s, px, py, z)) {
        imageAtomicMin(fuse_vsmr_images[image], origin + ivec2(px, py), floatBitsToUint(fuse_vsmr_persp_depth(t, e, px, py)));
    }
}

// raster_math::view_plane.
vec4 fuse_vsmr_view_plane(vec3 v[3]) {
    precise vec3 e1 = v[1] - v[0];
    precise vec3 e2 = v[2] - v[0];
    precise float nx = e1.y * e2.z - e1.z * e2.y;
    precise float ny = e1.z * e2.x - e1.x * e2.z;
    precise float nz = e1.x * e2.y - e1.y * e2.x;
    const vec3 n = vec3(nx, ny, nz);
    return vec4(n, fuse_vsmr_dot(n, v[0]));
}

// raster_math::clip_project: clips a view-space triangle against z >= nearPlane and projects it.
uint fuse_vsmr_clip_project(FuseVsmLocal e, vec3 v[3], out FuseVsmrTri outTri[2]) {
    vec3 poly[4];
    uint n = 0u;
    const float zn = e.nearPlane;
    for (uint i = 0u; i < 3u; ++i) {
        const vec3 a = v[i];
        const vec3 b = v[(i + 1u) % 3u];
        const bool ain = a.z >= zn;
        const bool bin = b.z >= zn;
        if (ain) {
            poly[n] = a;
            ++n;
        }
        if (ain != bin) {
            precise float t = (zn - a.z) / (b.z - a.z);
            precise float px = a.x + (b.x - a.x) * t;
            precise float py = a.y + (b.y - a.y) * t;
            poly[n] = vec3(px, py, zn);
            ++n;
        }
    }
    outTri[0].x = vec3(0.0);
    outTri[0].y = vec3(0.0);
    outTri[0].z = vec3(0.0);
    outTri[0].h = 0.0;
    outTri[1] = outTri[0];
    if (n < 3u) {
        return 0u;
    }
    vec3 sp[4];
    for (uint i = 0u; i < n; ++i) {
        sp[i] = fuse_vsmr_local_project(e, poly[i]);
    }
    const vec4 plane = fuse_vsmr_view_plane(v);
    const uint count = n - 2u;
    for (uint k = 0u; k < count; ++k) {
        const uvec3 idx = uvec3(0u, k + 1u, k + 2u);
        for (uint j = 0u; j < 3u; ++j) {
            outTri[k].x[j] = sp[idx[j]].x;
            outTri[k].y[j] = sp[idx[j]].y;
            outTri[k].z[j] = plane[j];
        }
        outTri[k].h = plane.w;
    }
    return count;
}

#endif // FUSE_VSM_RASTER_COMMON_GLSL
