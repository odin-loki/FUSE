#include <fuse/renderer/geometry/meshlet_builder.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/geometry/meshlet_bounds_kernel.hpp>
#include <fuse/renderer/geometry/vertex_codec_kernel.hpp>

#include <meshoptimizer.h>
// Pin check: #errors when the vendored header's MESHOPTIMIZER_VERSION disagrees with Engine/lib/meshoptimizer/VERSION.
#include <fuse_meshoptimizer_version.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace fuse::renderer::geometry {

namespace {

bool set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = "meshlet build: " + message;
    }
    return false;
}

bool all_finite(const f32* values, usize count) {
    for (usize i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

void normalize3(f64 v[3]) {
    const f64 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.0) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

/// Final unit normals: source normals where usable, area-weighted face normals otherwise, +Z last.
std::vector<f32> resolve_normals(const MeshletSource& s, u32& fromSource) {
    const u32 n = s.vertex_count;
    std::vector<f64> face(static_cast<usize>(n) * 3u, 0.0);
    for (u32 i = 0; i + 2u < s.index_count; i += 3u) {
        const u32 a = s.indices[i], b = s.indices[i + 1u], c = s.indices[i + 2u];
        f64 e1[3], e2[3];
        for (u32 k = 0; k < 3u; ++k) {
            e1[k] = static_cast<f64>(s.positions[b * 3u + k]) - s.positions[a * 3u + k];
            e2[k] = static_cast<f64>(s.positions[c * 3u + k]) - s.positions[a * 3u + k];
        }
        const f64 cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (u32 v : {a, b, c}) {
            for (u32 k = 0; k < 3u; ++k) {
                face[v * 3u + k] += cr[k];
            }
        }
    }
    std::vector<f32> out(static_cast<usize>(n) * 3u);
    fromSource = 0;
    for (u32 v = 0; v < n; ++v) {
        f64 nv[3] = {0.0, 0.0, 0.0};
        if (s.normals != nullptr) {
            for (u32 k = 0; k < 3u; ++k) {
                nv[k] = s.normals[v * 3u + k];
            }
        }
        if (nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2] > 1e-24) {
            ++fromSource;
        } else {
            for (u32 k = 0; k < 3u; ++k) {
                nv[k] = face[v * 3u + k];
            }
            if (!(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2] > 0.0)) {
                nv[0] = 0.0;
                nv[1] = 0.0;
                nv[2] = 1.0;
            }
        }
        normalize3(nv);
        for (u32 k = 0; k < 3u; ++k) {
            out[v * 3u + k] = static_cast<f32>(nv[k]);
        }
    }
    return out;
}

/// Tangents from UVs (per-vertex accumulation + Gram-Schmidt), or copied from the source.
std::vector<f32> resolve_tangents(const MeshletSource& s, const std::vector<f32>& normals) {
    const u32 n = s.vertex_count;
    std::vector<f32> out(static_cast<usize>(n) * 4u);
    if (s.tangents != nullptr) {
        for (u32 v = 0; v < n; ++v) {
            for (u32 k = 0; k < 4u; ++k) {
                out[v * 4u + k] = s.tangents[v * 4u + k];
            }
            out[v * 4u + 3u] = s.tangents[v * 4u + 3u] < 0.f ? -1.f : 1.f;
        }
        return out;
    }
    std::vector<f64> tanAcc(static_cast<usize>(n) * 3u, 0.0);
    std::vector<f64> bitAcc(static_cast<usize>(n) * 3u, 0.0);
    if (s.uvs != nullptr) {
        for (u32 i = 0; i + 2u < s.index_count; i += 3u) {
            const u32 a = s.indices[i], b = s.indices[i + 1u], c = s.indices[i + 2u];
            f64 e1[3], e2[3];
            for (u32 k = 0; k < 3u; ++k) {
                e1[k] = static_cast<f64>(s.positions[b * 3u + k]) - s.positions[a * 3u + k];
                e2[k] = static_cast<f64>(s.positions[c * 3u + k]) - s.positions[a * 3u + k];
            }
            const f64 du1 = static_cast<f64>(s.uvs[b * 2u]) - s.uvs[a * 2u];
            const f64 dv1 = static_cast<f64>(s.uvs[b * 2u + 1u]) - s.uvs[a * 2u + 1u];
            const f64 du2 = static_cast<f64>(s.uvs[c * 2u]) - s.uvs[a * 2u];
            const f64 dv2 = static_cast<f64>(s.uvs[c * 2u + 1u]) - s.uvs[a * 2u + 1u];
            const f64 det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-20) {
                continue;
            }
            const f64 r = 1.0 / det;
            for (u32 v : {a, b, c}) {
                for (u32 k = 0; k < 3u; ++k) {
                    tanAcc[v * 3u + k] += (e1[k] * dv2 - e2[k] * dv1) * r;
                    bitAcc[v * 3u + k] += (e2[k] * du1 - e1[k] * du2) * r;
                }
            }
        }
    }
    for (u32 v = 0; v < n; ++v) {
        const f64 nv[3] = {normals[v * 3u], normals[v * 3u + 1u], normals[v * 3u + 2u]};
        f64 t[3] = {tanAcc[v * 3u], tanAcc[v * 3u + 1u], tanAcc[v * 3u + 2u]};
        const f64 d = t[0] * nv[0] + t[1] * nv[1] + t[2] * nv[2];
        for (u32 k = 0; k < 3u; ++k) {
            t[k] -= nv[k] * d;
        }
        if (t[0] * t[0] + t[1] * t[1] + t[2] * t[2] < 1e-20) {
            // Arbitrary basis: cross the normal with the axis it is least aligned with.
            const f64 ax = std::fabs(nv[0]), ay = std::fabs(nv[1]), az = std::fabs(nv[2]);
            const f64 other[3] = {ax <= ay && ax <= az ? 1.0 : 0.0, ay < ax && ay <= az ? 1.0 : 0.0,
                                  az < ax && az < ay ? 1.0 : 0.0};
            t[0] = other[1] * nv[2] - other[2] * nv[1];
            t[1] = other[2] * nv[0] - other[0] * nv[2];
            t[2] = other[0] * nv[1] - other[1] * nv[0];
        }
        normalize3(t);
        const f64 c[3] = {nv[1] * t[2] - nv[2] * t[1], nv[2] * t[0] - nv[0] * t[2], nv[0] * t[1] - nv[1] * t[0]};
        const f64 hand = c[0] * bitAcc[v * 3u] + c[1] * bitAcc[v * 3u + 1u] + c[2] * bitAcc[v * 3u + 2u];
        for (u32 k = 0; k < 3u; ++k) {
            out[v * 4u + k] = static_cast<f32>(t[k]);
        }
        out[v * 4u + 3u] = hand < 0.0 ? -1.f : 1.f;
    }
    return out;
}

template <typename T>
kernel::Span<const T> cspan(const std::vector<T>& v) {
    return kernel::make_span(v.data(), static_cast<u32>(v.size()));
}

template <typename T>
kernel::Span<T> mspan(std::vector<T>& v) {
    return kernel::make_span(v.data(), static_cast<u32>(v.size()));
}

} // namespace

QuantParams compute_quant_params(const f32 min[3], const f32 max[3]) {
    QuantParams q{};
    for (u32 a = 0; a < 3u; ++a) {
        const f64 lo = min[a];
        const f64 hi = max[a];
        const f64 extent = hi - lo;
        s32 e = -126;
        if (extent > 0.0) {
            e = std::max(-126, std::ilogb(extent) - 17);
        }
        for (;; ++e) {
            const f64 step = std::ldexp(1.0, e);
            const f64 k = std::floor(lo / step);
            if ((hi - k * step) / step <= static_cast<f64>(vertex_codec::kQuantMax) &&
                std::fabs(k) + static_cast<f64>(vertex_codec::kQuantMax) < 16777216.0) {
                q.exponent[a] = e;
                q.step[a] = std::ldexp(1.f, e);
                q.offset[a] = static_cast<f32>(k * step);
                break;
            }
        }
    }
    return q;
}

bool build_meshlets(const MeshletSource& s, const MeshletBuildOptions& o, MeshletMesh& out, std::string* error) {
    out = MeshletMesh{};
    // ---- validation -----------------------------------------------------------------------------
    if (o.max_vertices < 3u || o.max_vertices > kMeshletFormatMaxVertices || o.max_triangles < 4u ||
        o.max_triangles > kMeshletFormatMaxTriangles || o.max_triangles % 4u != 0u) {
        return set_error(error, "limits out of range (3..255 vertices, 4..252 triangles in steps of 4)");
    }
    if (s.positions == nullptr || s.indices == nullptr || s.vertex_count == 0u || s.index_count == 0u) {
        return set_error(error, "no geometry");
    }
    if (s.index_count % 3u != 0u) {
        return set_error(error, "index count is not a multiple of 3");
    }
    const usize vc = s.vertex_count;
    if (!all_finite(s.positions, vc * 3u) || (s.normals != nullptr && !all_finite(s.normals, vc * 3u)) ||
        (s.uvs != nullptr && !all_finite(s.uvs, vc * 2u)) || (s.tangents != nullptr && !all_finite(s.tangents, vc * 4u))) {
        return set_error(error, "non-finite vertex attribute");
    }
    if (s.uvs != nullptr) {
        for (usize i = 0; i < vc * 2u; ++i) {
            if (std::fabs(s.uvs[i]) > 65504.f) {
                return set_error(error, "uv outside the half-float range");
            }
        }
    }
    for (u32 i = 0; i < s.index_count; ++i) {
        if (s.indices[i] >= s.vertex_count) {
            return set_error(error, "index " + std::to_string(i) + " out of range");
        }
    }
    std::vector<MeshletSourceSubmesh> submeshes = s.submeshes;
    if (submeshes.empty()) {
        submeshes.push_back({0u, s.index_count, 0u});
    }
    if (submeshes.size() > 0x10000u) {
        return set_error(error, "more than 65536 submeshes");
    }
    for (const MeshletSourceSubmesh& sub : submeshes) {
        if (sub.index_count % 3u != 0u || static_cast<u64>(sub.index_offset) + sub.index_count > s.index_count) {
            return set_error(error, "submesh index range invalid");
        }
    }

    // ---- attributes -----------------------------------------------------------------------------
    u32 normalsFromSource = 0;
    const std::vector<f32> normals = resolve_normals(s, normalsFromSource);
    const std::vector<f32> tangents = resolve_tangents(s, normals);

    // ---- meshlets per submesh (source vertex numbering) ------------------------------------------
    std::vector<u32> mvrtSource; // meshlet vertex -> source vertex
    for (u32 si = 0; si < submeshes.size(); ++si) {
        const MeshletSourceSubmesh& sub = submeshes[si];
        SubmeshRange range{};
        range.meshlet_offset = static_cast<u32>(out.meshlets.size());
        range.material_index = sub.material_index;
        range.triangle_count = sub.index_count / 3u;
        if (sub.index_count > 0u) {
            std::vector<u32> idx(s.indices + sub.index_offset, s.indices + sub.index_offset + sub.index_count);
            meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), vc);
            const usize bound = meshopt_buildMeshletsBound(idx.size(), o.max_vertices, o.max_triangles);
            std::vector<meshopt_Meshlet> ml(bound);
            std::vector<u32> mv(bound * o.max_vertices);
            std::vector<u8> mt(bound * o.max_triangles * 3u);
            const usize count = meshopt_buildMeshlets(ml.data(), mv.data(), mt.data(), idx.data(), idx.size(), s.positions,
                                                      vc, sizeof(f32) * 3u, o.max_vertices, o.max_triangles, o.cone_weight);
            for (usize m = 0; m < count; ++m) {
                const meshopt_Meshlet& src = ml[m];
                meshopt_optimizeMeshlet(&mv[src.vertex_offset], &mt[src.triangle_offset], src.triangle_count, src.vertex_count);
                MeshletRecord rec{};
                rec.vertex_offset = static_cast<u32>(mvrtSource.size());
                rec.triangle_offset = static_cast<u32>(out.meshlet_triangles.size());
                rec.vertex_count = src.vertex_count;
                rec.triangle_count = src.triangle_count;
                rec.submesh = si;
                mvrtSource.insert(mvrtSource.end(), mv.begin() + src.vertex_offset,
                                  mv.begin() + src.vertex_offset + src.vertex_count);
                for (u32 t = 0; t < src.triangle_count; ++t) {
                    const u8* tri = &mt[src.triangle_offset + t * 3u];
                    out.meshlet_triangles.push_back(pack_triangle(tri[0], tri[1], tri[2]));
                }
                out.meshlets.push_back(rec);
            }
        }
        range.meshlet_count = static_cast<u32>(out.meshlets.size()) - range.meshlet_offset;
        out.submeshes.push_back(range);
    }

    // ---- vertex fetch order: renumber vertices in meshlet first-use order -----------------------
    std::vector<u32> flat;
    flat.reserve(out.meshlet_triangles.size() * 3u);
    for (const MeshletRecord& rec : out.meshlets) {
        for (u32 t = 0; t < rec.triangle_count; ++t) {
            const u32 packed = out.meshlet_triangles[rec.triangle_offset + t];
            for (u32 c = 0; c < 3u; ++c) {
                flat.push_back(mvrtSource[rec.vertex_offset + triangle_index(packed, c)]);
            }
        }
    }
    std::vector<u32> remap(vc);
    const usize used = meshopt_optimizeVertexFetchRemap(remap.data(), flat.data(), flat.size(), vc);
    const u32 n = static_cast<u32>(used);
    out.meshlet_vertices.resize(mvrtSource.size());
    for (usize i = 0; i < mvrtSource.size(); ++i) {
        out.meshlet_vertices[i] = remap[mvrtSource[i]];
    }
    std::vector<u32> sourceOf(n);
    for (u32 v = 0; v < s.vertex_count; ++v) {
        if (remap[v] != ~0u) {
            sourceOf[remap[v]] = v;
        }
    }

    // ---- gather float streams in cooked order, quantisation range --------------------------------
    std::vector<f32> pos(static_cast<usize>(n) * 3u), nrm(static_cast<usize>(n) * 3u), tng(static_cast<usize>(n) * 4u),
        uv(static_cast<usize>(n) * 2u, 0.f);
    f32 lo[3] = {0.f, 0.f, 0.f};
    f32 hi[3] = {0.f, 0.f, 0.f};
    for (u32 v = 0; v < n; ++v) {
        const u32 src = sourceOf[v];
        for (u32 k = 0; k < 3u; ++k) {
            pos[v * 3u + k] = s.positions[src * 3u + k];
            nrm[v * 3u + k] = normals[src * 3u + k];
            lo[k] = v == 0u ? pos[k] : std::min(lo[k], pos[v * 3u + k]);
            hi[k] = v == 0u ? pos[k] : std::max(hi[k], pos[v * 3u + k]);
        }
        for (u32 k = 0; k < 4u; ++k) {
            tng[v * 4u + k] = tangents[src * 4u + k];
        }
        if (s.uvs != nullptr) {
            uv[v * 2u] = s.uvs[src * 2u];
            uv[v * 2u + 1u] = s.uvs[src * 2u + 1u];
        }
    }
    out.quant = compute_quant_params(lo, hi);

    // ---- encode (kernel) ------------------------------------------------------------------------
    out.positions.resize(static_cast<usize>(n) * 4u);
    out.normals.resize(n);
    out.tangents.resize(n);
    out.uvs.resize(n);
    vertex_codec::EncodeParams ep{};
    ep.positions = cspan(pos);
    ep.normals = cspan(nrm);
    ep.tangents = cspan(tng);
    ep.uvs = cspan(uv);
    ep.quant = out.quant;
    ep.out_pos = mspan(out.positions);
    ep.out_nrm = mspan(out.normals);
    ep.out_tan = mspan(out.tangents);
    ep.out_uv = mspan(out.uvs);
    if (!kernel::launch(o.backend, vertex_codec::make_encode_launch(n), vertex_codec::EncodeKernel{}, ep).ok) {
        out = MeshletMesh{};
        return set_error(error, "vertex encode launch failed");
    }
    if (o.keep_source_vertex_map) {
        out.source_vertices = sourceOf;
    }
    out.flags = (normalsFromSource == s.vertex_count ? kMeshletFlagNormalsFromSource : 0u) |
                (s.tangents != nullptr ? kMeshletFlagTangentsFromSource : 0u) |
                (s.uvs != nullptr ? kMeshletFlagUvFromSource : 0u);
    out.max_vertices = o.max_vertices;
    out.max_triangles = o.max_triangles;
    out.source_hash = o.source_hash;

    // ---- bounds on decoded positions --------------------------------------------------------------
    DecodedVertices decoded;
    decode_vertices(out, decoded, o.backend);
    std::vector<u8> localTris(static_cast<usize>(o.max_triangles) * 3u);
    for (MeshletRecord& rec : out.meshlets) {
        for (u32 t = 0; t < rec.triangle_count; ++t) {
            const u32 packed = out.meshlet_triangles[rec.triangle_offset + t];
            for (u32 c = 0; c < 3u; ++c) {
                localTris[t * 3u + c] = static_cast<u8>(triangle_index(packed, c));
            }
        }
        const meshopt_Bounds b = meshopt_computeMeshletBounds(&out.meshlet_vertices[rec.vertex_offset], localTris.data(),
                                                              rec.triangle_count, decoded.positions.data(), n,
                                                              sizeof(f32) * 3u);
        for (u32 k = 0; k < 3u; ++k) {
            rec.center[k] = b.center[k];
            rec.cone_apex[k] = b.cone_apex[k];
            rec.cone_axis[k] = b.cone_axis[k];
            rec.cone_axis_s8[k] = static_cast<i8>(b.cone_axis_s8[k]);
        }
        rec.radius = b.radius;
        rec.cone_cutoff = b.cone_cutoff;
        rec.cone_cutoff_s8 = static_cast<i8>(b.cone_cutoff_s8);
    }
    bounds_kernel::Params bp{};
    bp.meshlets = mspan(out.meshlets);
    bp.meshlet_vertices = cspan(out.meshlet_vertices);
    bp.positions = cspan(decoded.positions);
    if (!kernel::launch(o.backend, bounds_kernel::make_launch(static_cast<u32>(out.meshlets.size())), bounds_kernel::Kernel{}, bp).ok) {
        out = MeshletMesh{};
        return set_error(error, "meshlet bounds launch failed");
    }

    std::string why;
    if (!validate_meshlet_mesh(out, &why)) {
        out = MeshletMesh{};
        return set_error(error, "internal: built mesh failed validation: " + why);
    }
    return true;
}

void decode_vertices(const MeshletMesh& mesh, DecodedVertices& out, kernel::Backend backend) {
    const u32 n = mesh.vertex_count();
    out.positions.assign(static_cast<usize>(n) * 3u, 0.f);
    out.normals.assign(static_cast<usize>(n) * 3u, 0.f);
    out.tangents.assign(static_cast<usize>(n) * 4u, 0.f);
    out.uvs.assign(static_cast<usize>(n) * 2u, 0.f);
    vertex_codec::DecodeParams dp{};
    dp.pos = cspan(mesh.positions);
    dp.nrm = cspan(mesh.normals);
    dp.tan = cspan(mesh.tangents);
    dp.uv = cspan(mesh.uvs);
    dp.quant = mesh.quant;
    dp.out_positions = mspan(out.positions);
    dp.out_normals = mspan(out.normals);
    dp.out_tangents = mspan(out.tangents);
    dp.out_uvs = mspan(out.uvs);
    (void)kernel::launch(backend, vertex_codec::make_decode_launch(n), vertex_codec::DecodeKernel{}, dp);
}

void cull_meshlets(const MeshletMesh& mesh, const cull_kernel::CullView& view, std::vector<u32>& out, kernel::Backend backend) {
    out.assign(mesh.meshlets.size(), 0u);
    cull_kernel::Params cp{};
    cp.meshlets = cspan(mesh.meshlets);
    cp.view = view;
    cp.out = mspan(out);
    (void)kernel::launch(backend, cull_kernel::make_launch(static_cast<u32>(mesh.meshlets.size())), cull_kernel::Kernel{}, cp);
}

} // namespace fuse::renderer::geometry
