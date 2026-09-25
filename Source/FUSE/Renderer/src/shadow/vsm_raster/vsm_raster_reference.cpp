// WP-3.2 CPU references: see include/fuse/renderer/shadow/vsm_raster/vsm_raster_reference.hpp.
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::renderer::vsm {

using namespace raster_math;

void VsmRasterReference::prepare(const visbuffer::VisSceneView& scene, const VsmFrameConstants* c) {
    m_tris.clear();
    m_instances.clear();
    for (u32 i = 0; i < scene.instances.size && i < scene.transforms.size; ++i) {
        const gpu_scene::GpuInstance& inst = scene.instances[i];
        if (inst.mesh >= scene.meshes.size || inst.mesh >= scene.positions.size) {
            continue;
        }
        const gpu_scene::GpuMesh& mesh = scene.meshes[inst.mesh];
        const visbuffer::decode_kernel::MeshPositions& pos = scene.positions[inst.mesh];
        CasterInstance ci{};
        if (pos.vpos == nullptr || !caster_bounds(inst, mesh, scene.transforms[i], ci.center, ci.extent)) {
            continue;
        }
        ci.first = static_cast<u32>(m_tris.size());
        const u32 triangles = mesh.indexCount / 3u;
        for (u32 t = 0; t < triangles; ++t) {
            const u32 base = mesh.firstIndex + t * 3u;
            if (static_cast<u64>(base) + 3u > scene.indices.size) {
                break;
            }
            CasterTri tri{};
            tri.instance = i;
            bool ok = true;
            for (u32 k = 0; k < 3u; ++k) {
                const u32 v = static_cast<u32>(static_cast<s32>(scene.indices[base + k]) + mesh.vertexOffset);
                if (v >= pos.vertexCount) {
                    ok = false;
                    break;
                }
                f32 obj[3];
                visbuffer::decode_kernel::mesh_position(mesh, pos.vpos, v, obj);
                world_point(scene.transforms[i], obj, tri.w[k]);
                if (c != nullptr) {
                    light_point(*c, tri.w[k], tri.l[k]);
                }
            }
            if (!ok) {
                continue;
            }
            for (u32 a = 0; a < 3u; ++a) {
                tri.lmin[a] = min_f(min_f(tri.l[0][a], tri.l[1][a]), tri.l[2][a]);
                tri.lmax[a] = max_f(max_f(tri.l[0][a], tri.l[1][a]), tri.l[2][a]);
            }
            m_tris.push_back(tri);
        }
        ci.count = static_cast<u32>(m_tris.size()) - ci.first;
        m_instances.push_back(ci);
    }
}

namespace {

void clearPage(u32* image, u32 width, u32 page, u32 pagesX, u32 value) {
    const u32 px = (page % pagesX) * kPageTexels;
    const u32 py = (page / pagesX) * kPageTexels;
    for (u32 y = 0; y < kPageTexels; ++y) {
        u32* row = image + static_cast<usize>(py + y) * width + px;
        for (u32 x = 0; x < kPageTexels; ++x) {
            row[x] = value;
        }
    }
}

/// Rasterises one screen triangle into a page (atomic min of the float bits); `local` = perspective depth.
u64 rasterTri(const Tri& t, const VsmLocalLight* local, u32* image, u32 width, u32 page, u32 pagesX) {
    TriSetup s{};
    if (!setup_tri(t, s)) {
        return 0u;
    }
    const u32 px = (page % pagesX) * kPageTexels;
    const u32 py = (page / pagesX) * kPageTexels;
    u64 written = 0;
    for (s32 y = s.y0; y <= s.y1; ++y) {
        for (s32 x = s.x0; x <= s.x1; ++x) {
            f32 z = 0.f;
            if (!tri_texel(t, s, x, y, z)) {
                continue;
            }
            const u32 bits = bits_of(local != nullptr ? persp_depth(t, *local, x, y) : clamp_depth(z));
            u32& dst = image[static_cast<usize>(py + static_cast<u32>(y)) * width + px + static_cast<u32>(x)];
            dst = bits < dst ? bits : dst;
            ++written;
        }
    }
    return written;
}

} // namespace

VsmRasterRefStats VsmRasterReference::renderPages(const VsmFrameConstants& c, const u32* list, u32 count, u32* pool,
                                                  u32 poolWidth) const {
    VsmRasterRefStats st{};
    const u32 pagesX = c.poolPagesX;
    const f32 texels = static_cast<f32>(kPageTexels);
    for (u32 i = 0; i < count; ++i) {
        const u32 vp = list[i * 2u];
        const u32 phys = list[i * 2u + 1u];
        u32 level = 0;
        s32 ax = 0, ay = 0;
        dir_page_of(c, vp, level, ax, ay);
        if (level >= c.levels || phys >= c.physPages) {
            continue;
        }
        clearPage(pool, poolWidth, phys, pagesX, c.clearValue);
        ++st.pages;
        const VsmLevelConstants& L = c.level[level];
        for (const CasterTri& ct : m_tris) {
            // Exactly the triangles whose texel box is empty (monotone rounding) or whose three
            // depths are > 1 are skipped: the same set setup_tri / the GPU's depth test drop.
            const f32 x0 = (ct.lmin[0] * L.invPageWorld - static_cast<f32>(ax)) * texels;
            const f32 x1 = (ct.lmax[0] * L.invPageWorld - static_cast<f32>(ax)) * texels;
            const f32 y0 = (ct.lmin[1] * L.invPageWorld - static_cast<f32>(ay)) * texels;
            const f32 y1 = (ct.lmax[1] * L.invPageWorld - static_cast<f32>(ay)) * texels;
            if (x1 < 0.5f || x0 > texels - 0.5f || y1 < 0.5f || y0 > texels - 0.5f) {
                continue;
            }
            Tri t{};
            for (u32 k = 0; k < 3u; ++k) {
                dir_page_vertex(L, ax, ay, ct.l[k], t.x[k], t.y[k], t.z[k]);
            }
            if (t.z[0] > 1.f && t.z[1] > 1.f && t.z[2] > 1.f) {
                continue;
            }
            ++st.triangles;
            st.texels += rasterTri(t, nullptr, pool, poolWidth, phys, pagesX);
        }
    }
    return st;
}

VsmRasterRefStats VsmRasterReference::renderLocal(const VsmShadowConstants& s, const u32* list, u32 count, u32* atlas,
                                                  u32 atlasWidth) const {
    VsmRasterRefStats st{};
    for (u32 i = 0; i < count; ++i) {
        const u32 light = list[i * 2u] & 0xFFu;
        const u32 face = (list[i * 2u] >> 8u) & 0xFFu;
        const u32 page = list[i * 2u + 1u];
        if (light >= s.localCount || light >= kMaxLocalLights || s.localPagesX == 0u) {
            continue;
        }
        const VsmLocalLight& e = s.local[light];
        clearPage(atlas, atlasWidth, page, s.localPagesX, s.localClear);
        ++st.pages;
        f32 f[3], r[3], u[3];
        local_basis(e, face, f, r, u);
        for (const CasterInstance& ci : m_instances) {
            if (!box_sphere(ci.center, ci.extent, e.position, e.range)) {
                continue;
            }
            for (u32 k = 0; k < ci.count; ++k) {
                const CasterTri& ct = m_tris[ci.first + k];
                f32 v[3][3];
                for (u32 j = 0; j < 3u; ++j) {
                    local_view(e, f, r, u, ct.w[j], v[j]);
                }
                if (v[0][2] > e.range && v[1][2] > e.range && v[2][2] > e.range) {
                    continue;
                }
                Tri tris[2];
                const u32 n = clip_project(e, v, tris);
                for (u32 j = 0; j < n; ++j) {
                    ++st.triangles;
                    st.texels += rasterTri(tris[j], &e, atlas, atlasWidth, page, s.localPagesX);
                }
            }
        }
    }
    return st;
}

SampleResult shadowVisibility(const ShadowReferenceSource& src, u32 slot, const math::Vec3& position, const math::Vec3& normal) {
    if (src.shadow == nullptr) {
        return SampleResult{};
    }
    const f32 p[3] = {position.x, position.y, position.z};
    const f32 n[3] = {normal.x, normal.y, normal.z};
    return shadow_visibility(*src.shadow, src.vsm, src.store, slot, p, n);
}

namespace {
f32 shadeHook(const void* user, u32 slot, const lighting_gpu::SurfaceSample& s) {
    return shadowVisibility(*static_cast<const ShadowReferenceSource*>(user), slot, s.position, s.normal).visibility;
}
} // namespace

u32 shadeShadowedReference(const lighting_gpu::ShadeReferenceDesc& desc, const ShadowReferenceSource& src,
                           std::vector<math::Vec4>& out, kernel::Backend backend) {
    lighting_gpu::ShadeReferenceDesc d = desc;
    d.shadow = &shadeHook;
    d.shadowUser = &src;
    return lighting_gpu::shadeReferenceFrame(d, out, backend);
}

} // namespace fuse::renderer::vsm
