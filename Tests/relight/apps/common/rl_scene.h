// FUSE Relight test-app kit (RL-0.4): include for the scene sources (scenes/*.cpp).
// Scenes use the D3D9 enum names from d3d9types.h for both builds; the D3D8 values of every
// enum a D3D8 twin uses are identical (render states, stage states, FVF bits, formats, pools,
// usages, lock flags, primitive types, light types, transform ids).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9types.h>

#include <vector>

#include "rl_app.h"
#include "rl_shader.h"

namespace rl {

struct VtxPC { // D3DFVF_XYZ | D3DFVF_DIFFUSE
    float x, y, z;
    uint32_t color;
};
struct VtxPCT { // D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1
    float x, y, z;
    uint32_t color;
    float u, v;
};
struct VtxPNT { // D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};
struct VtxPNC { // D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE
    float x, y, z;
    float nx, ny, nz;
    uint32_t color;
};
struct VtxTL { // D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 (POSITIONT)
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};
static const uint32_t FVF_PC = D3DFVF_XYZ | D3DFVF_DIFFUSE;
static const uint32_t FVF_PCT = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
static const uint32_t FVF_PNT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
static const uint32_t FVF_PNC = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE;
static const uint32_t FVF_TL = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Camera {
    Vec3 eye, at, up;
    float fovY, aspect, zn, zf;
    Mat4 view, proj;
};

// Sets VIEW and PROJECTION from a look-at camera and records it under annotations.camera.
inline Camera setCamera(Gfx& g, Vec3 eye, Vec3 at, float fovY = kPi / 3.0f, float zn = 0.5f, float zf = 100.0f)
{
    Camera c{eye, at, {0.0f, 1.0f, 0.0f}, fovY, float(kWidth) / float(kHeight), zn, zf, {}, {}};
    c.view = lookAtLH(c.eye, c.at, c.up);
    c.proj = perspectiveFovLH(c.fovY, c.aspect, c.zn, c.zf);
    g.setTransform(D3DTS_VIEW, c.view);
    g.setTransform(D3DTS_PROJECTION, c.proj);
    JV j = JV::obj();
    j.set("eye", JV::floats(&c.eye.x, 3));
    j.set("at", JV::floats(&c.at.x, 3));
    j.set("up", JV::floats(&c.up.x, 3));
    j.set("fov_y", JV::num(c.fovY));
    j.set("aspect", JV::num(c.aspect));
    j.set("z_near", JV::num(c.zn));
    j.set("z_far", JV::num(c.zf));
    j.set("handedness", JV::str("left"));
    g.annotate("camera", j);
    return c;
}

// Probe at the projection of object-space point p under world * camera.
inline void probeAt(Gfx& g, const Mat4& world, const Camera& c, Vec3 p, uint8_t r, uint8_t gg, uint8_t b, int tol,
                    const char* what)
{
    int x, y;
    if (projectToPixel(mul(mul(world, c.view), c.proj), p, kWidth, kHeight, x, y))
        g.probe(x, y, r, gg, b, tol, what);
}

inline uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }

// Default fixed-function state every scene starts from (all recorded).
inline void baseState(Gfx& g)
{
    g.setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g.setRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g.setRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g.setRenderState(D3DRS_LIGHTING, FALSE);
    g.setRenderState(D3DRS_DITHERENABLE, FALSE);
    g.setRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
}

// Unit cube, 24 vertices (4 per face, per-face normal, uv 0..1) and 36 u16 indices.
inline void makeCube(std::vector<VtxPNT>& v, std::vector<uint16_t>& idx, float h = 0.5f)
{
    static const float n[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    v.clear();
    idx.clear();
    for (int f = 0; f < 6; ++f) {
        Vec3 N{n[f][0], n[f][1], n[f][2]};
        Vec3 U = f < 4 ? Vec3{N.z, 0.0f, -N.x} : Vec3{1.0f, 0.0f, 0.0f};
        if (f == 5)
            U = Vec3{-1.0f, 0.0f, 0.0f};
        Vec3 V = cross(N, U);
        static const float c[4][2] = {{-1, -1}, {-1, 1}, {1, 1}, {1, -1}};
        uint16_t base = uint16_t(v.size());
        for (int k = 0; k < 4; ++k) {
            float a = c[k][0], b = c[k][1];
            VtxPNT p;
            p.x = (N.x + a * U.x + b * V.x) * h;
            p.y = (N.y + a * U.y + b * V.y) * h;
            p.z = (N.z + a * U.z + b * V.z) * h;
            p.nx = N.x; p.ny = N.y; p.nz = N.z;
            p.u = (a + 1.0f) * 0.5f;
            p.v = (1.0f - b) * 0.5f;
            v.push_back(p);
        }
        uint16_t q[6] = {0, 1, 2, 0, 2, 3};
        for (uint16_t k : q)
            idx.push_back(uint16_t(base + k));
    }
}

// UV sphere / grid helpers produce deterministic data from integer loops only.
inline void makeGrid(std::vector<VtxPNT>& v, std::vector<uint16_t>& idx, int nx, int nz, float size)
{
    v.clear();
    idx.clear();
    for (int z = 0; z <= nz; ++z)
        for (int x = 0; x <= nx; ++x) {
            VtxPNT p;
            p.x = (float(x) / float(nx) - 0.5f) * size;
            p.y = 0.0f;
            p.z = (float(z) / float(nz) - 0.5f) * size;
            p.nx = 0.0f; p.ny = 1.0f; p.nz = 0.0f;
            p.u = float(x) / float(nx) * 4.0f;
            p.v = float(z) / float(nz) * 4.0f;
            v.push_back(p);
        }
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x) {
            uint16_t a = uint16_t(z * (nx + 1) + x), b = uint16_t(a + 1), c = uint16_t(a + nx + 1), d = uint16_t(c + 1);
            uint16_t q[6] = {a, c, d, a, d, b};
            for (uint16_t k : q)
                idx.push_back(k);
        }
}

} // namespace rl
