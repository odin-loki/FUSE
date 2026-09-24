// WP-5.2 cluster DAG builder (see cluster_dag.hpp for the algorithm and the crack-free argument).
//
// Modelled on meshoptimizer's demo/clusterlod.h (vendored with WP-1.2 for this package; MIT,
// Arseny Kapoulkine) with these deliberate differences:
//   * level 0 is the WP-1.2 meshlet table, and simplification runs on the *decoded* cooked
//     positions, so every LOD cluster indexes the same cooked vertex streams (no new vertices);
//   * vertices of clusters left in a terminal ("stuck") group stay locked for every later level
//     (clusterlod only locks boundaries between the groups of the current level, so a later group
//     next to a stuck one could move their shared border);
//   * vertices shared between submeshes are locked for the whole build (the DAG is per submesh);
//   * group spheres are grown in f64 to contain every member sphere with a relative slack and the
//     error with a relative slack, both rounded up to f32, so the cut kernel's float evaluation is
//     monotone up the DAG (clusterlod relies on meshopt_computeSphereBounds being conservative);
//   * no sloppy / permissive fallback: a group that does not simplify becomes terminal, which keeps
//     the topology-preserving guarantees the crack test depends on.

#include <fuse/renderer/geometry/dag/cluster_dag.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/geometry/meshlet_bounds_kernel.hpp>

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::geometry::dag {

namespace {

bool set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = "cluster dag build: " + message;
    }
    return false;
}

f32 round_up_f32(f64 x) {
    f32 r = static_cast<f32>(x);
    while (static_cast<f64>(r) < x) {
        r = std::nextafter(r, 3.0e38f);
    }
    return r;
}

struct Builder {
    Builder(const MeshletMesh& b, const DagBuildOptions& opts, ClusterDag& dst) : base(b), o(opts), out(dst) {}

    const MeshletMesh& base;
    const DagBuildOptions& o;
    ClusterDag& out;

    u32 n = 0;                       ///< cooked vertex count
    std::vector<f32> positions;      ///< decoded xyz
    std::vector<u32> remap;          ///< vertex -> canonical vertex with the same position
    std::vector<u8> permanentLock;   ///< per canonical vertex: shared between submeshes
    std::vector<u8> frozen;          ///< per canonical vertex: used by a cluster of a terminal group
    std::vector<std::vector<u32>> indices; ///< per cluster id: triangle list (cooked vertex ids)
    std::vector<DagLodBounds> self;  ///< per cluster id: LOD bounds of its producer (leaf: sphere, 0)

    void leaf_indices(u32 id) {
        const MeshletRecord& r = base.meshlets[id];
        std::vector<u32>& idx = indices[id];
        idx.resize(static_cast<usize>(r.triangle_count) * 3u);
        for (u32 t = 0; t < r.triangle_count; ++t) {
            const u32 packed = base.meshlet_triangles[r.triangle_offset + t];
            for (u32 c = 0; c < 3u; ++c) {
                idx[t * 3u + c] = base.meshlet_vertices[r.vertex_offset + triangle_index(packed, c)];
            }
        }
    }

    std::vector<std::vector<u32>> partition(const std::vector<u32>& pending) const {
        if (pending.size() <= o.group_size) {
            return {pending};
        }
        std::vector<u32> flat;
        std::vector<u32> counts(pending.size());
        for (usize i = 0; i < pending.size(); ++i) {
            const std::vector<u32>& idx = indices[pending[i]];
            counts[i] = static_cast<u32>(idx.size());
            for (u32 v : idx) {
                flat.push_back(remap[v]);
            }
        }
        std::vector<u32> part(pending.size());
        const usize count = meshopt_partitionClusters(part.data(), flat.data(), flat.size(), counts.data(), counts.size(),
                                                      positions.data(), n, sizeof(f32) * 3u, o.group_size);
        std::vector<std::vector<u32>> groups(count);
        for (usize i = 0; i < pending.size(); ++i) {
            groups[part[i]].push_back(pending[i]);
        }
        groups.erase(std::remove_if(groups.begin(), groups.end(), [](const std::vector<u32>& g) { return g.empty(); }),
                     groups.end());
        return groups;
    }

    /// Sphere containing every member's LOD sphere (with slack), max member error (with slack).
    DagLodBounds merge_bounds(const std::vector<u32>& members) const {
        std::vector<f32> centers(members.size() * 3u);
        std::vector<f32> radii(members.size());
        f32 maxError = 0.f;
        for (usize i = 0; i < members.size(); ++i) {
            const DagLodBounds& b = self[members[i]];
            for (u32 a = 0; a < 3u; ++a) {
                centers[i * 3u + a] = b.center[a];
            }
            radii[i] = b.radius;
            maxError = std::max(maxError, b.error);
        }
        DagLodBounds out{};
        if (members.size() == 1u) {
            for (u32 a = 0; a < 3u; ++a) {
                out.center[a] = centers[a];
            }
        } else {
            const meshopt_Bounds mb = meshopt_computeSphereBounds(centers.data(), members.size(), sizeof(f32) * 3u,
                                                                  radii.data(), sizeof(f32));
            for (u32 a = 0; a < 3u; ++a) {
                out.center[a] = std::isfinite(mb.center[a]) ? mb.center[a] : centers[a];
            }
        }
        f64 reach = 0.0;
        for (usize i = 0; i < members.size(); ++i) {
            f64 d2 = 0.0;
            for (u32 a = 0; a < 3u; ++a) {
                const f64 d = static_cast<f64>(out.center[a]) - static_cast<f64>(centers[i * 3u + a]);
                d2 += d * d;
            }
            reach = std::max(reach, std::sqrt(d2) + static_cast<f64>(radii[i]));
        }
        out.radius = round_up_f32(reach * (1.0 + static_cast<f64>(o.sphere_slack)));
        out.error = maxError > 0.f ? round_up_f32(static_cast<f64>(maxError) * (1.0 + static_cast<f64>(o.error_slack))) : 0.f;
        return out;
    }

    /// Append the group record and point its members at it.
    u32 emit_group(const std::vector<u32>& members, const DagLodBounds& bounds, u32 depth, u32 submesh, u32 childOffset,
                   u32 childCount) {
        const u32 g = static_cast<u32>(out.groups.size());
        DagGroup grp{};
        grp.bounds = bounds;
        grp.member_offset = static_cast<u32>(out.group_members.size());
        grp.member_count = static_cast<u32>(members.size());
        grp.child_offset = childCount > 0u ? childOffset : 0u;
        grp.child_count = childCount;
        grp.depth = depth;
        grp.submesh = submesh;
        out.groups.push_back(grp);
        for (u32 c : members) {
            out.group_members.push_back(c);
            out.links[c].group = g;
            out.links[c].parent = bounds;
        }
        return g;
    }

    /// Split `simplified` into child clusters of group `g`; returns their ids.
    std::vector<u32> clusterize(const std::vector<u32>& simplified, u32 submesh, u32 g, const DagLodBounds& bounds) {
        const usize maxV = base.max_vertices;
        const usize maxT = base.max_triangles;
        const usize bound = meshopt_buildMeshletsBound(simplified.size(), maxV, maxT);
        std::vector<meshopt_Meshlet> ml(bound);
        std::vector<u32> mv(bound * maxV);
        std::vector<u8> mt(bound * maxT * 3u);
        const usize count = meshopt_buildMeshlets(ml.data(), mv.data(), mt.data(), simplified.data(), simplified.size(),
                                                  positions.data(), n, sizeof(f32) * 3u, maxV, maxT, o.cone_weight);
        std::vector<u32> ids;
        for (usize m = 0; m < count; ++m) {
            const meshopt_Meshlet& src = ml[m];
            meshopt_optimizeMeshlet(&mv[src.vertex_offset], &mt[src.triangle_offset], src.triangle_count, src.vertex_count);
            MeshletRecord rec{};
            rec.vertex_offset = static_cast<u32>(out.lod_meshlet_vertices.size());
            rec.triangle_offset = static_cast<u32>(out.lod_meshlet_triangles.size());
            rec.vertex_count = src.vertex_count;
            rec.triangle_count = src.triangle_count;
            rec.submesh = submesh;
            out.lod_meshlet_vertices.insert(out.lod_meshlet_vertices.end(), mv.begin() + src.vertex_offset,
                                            mv.begin() + src.vertex_offset + src.vertex_count);
            const u8* tri = &mt[src.triangle_offset];
            for (u32 t = 0; t < src.triangle_count; ++t) {
                out.lod_meshlet_triangles.push_back(pack_triangle(tri[t * 3u], tri[t * 3u + 1u], tri[t * 3u + 2u]));
            }
            const meshopt_Bounds b = meshopt_computeMeshletBounds(&mv[src.vertex_offset], tri, src.triangle_count,
                                                                  positions.data(), n, sizeof(f32) * 3u);
            for (u32 k = 0; k < 3u; ++k) {
                rec.center[k] = b.center[k];
                rec.cone_apex[k] = b.cone_apex[k];
                rec.cone_axis[k] = b.cone_axis[k];
                rec.cone_axis_s8[k] = static_cast<i8>(b.cone_axis_s8[k]);
            }
            rec.radius = b.radius;
            rec.cone_cutoff = b.cone_cutoff;
            rec.cone_cutoff_s8 = static_cast<i8>(b.cone_cutoff_s8);
            out.lod_clusters.push_back(rec);

            const u32 id = out.leaf_cluster_count + static_cast<u32>(out.lod_clusters.size()) - 1u;
            DagClusterLink link{};
            link.self = bounds;
            link.refined = g;
            out.links.push_back(link);
            indices.emplace_back();
            std::vector<u32>& idx = indices.back();
            idx.resize(static_cast<usize>(src.triangle_count) * 3u);
            for (u32 t = 0; t < src.triangle_count * 3u; ++t) {
                idx[t] = mv[src.vertex_offset + tri[t]];
            }
            self.push_back(bounds);
            ids.push_back(id);
        }
        return ids;
    }

    void freeze(const std::vector<u32>& members) {
        for (u32 c : members) {
            for (u32 v : indices[c]) {
                frozen[remap[v]] = 1u;
            }
        }
    }

    bool build_submesh(u32 s, std::string* error) {
        const SubmeshRange& sub = base.submeshes[s];
        std::vector<u32> pending;
        for (u32 m = sub.meshlet_offset; m < sub.meshlet_offset + sub.meshlet_count; ++m) {
            pending.push_back(m);
        }
        std::vector<u8> usedBy(n); // per canonical vertex: 0 unseen, 1 one group, 2 shared (level scratch)
        std::vector<u32> lastGroup(n);
        std::vector<u8> lock(n);
        u32 depth = 0;
        while (pending.size() > 1u && depth < o.max_levels) {
            const std::vector<std::vector<u32>> groups = partition(pending);
            // Level locks: canonical vertices used by two groups of this level.
            for (usize g = 0; g < groups.size(); ++g) {
                for (u32 c : groups[g]) {
                    for (u32 v : indices[c]) {
                        const u32 r = remap[v];
                        if (usedBy[r] == 0u) {
                            usedBy[r] = 1u;
                            lastGroup[r] = static_cast<u32>(g);
                        } else if (lastGroup[r] != g) {
                            usedBy[r] = 2u;
                        }
                    }
                }
            }
            for (u32 v = 0; v < n; ++v) {
                const u32 r = remap[v];
                lock[v] = (usedBy[r] == 2u || permanentLock[r] != 0u || frozen[r] != 0u) ? u8{meshopt_SimplifyVertex_Lock} : u8{0};
            }
            std::vector<u32> next;
            std::vector<std::vector<u32>> stuckGroups;
            for (const std::vector<u32>& members : groups) {
                std::vector<u32> merged;
                for (u32 c : members) {
                    merged.insert(merged.end(), indices[c].begin(), indices[c].end());
                }
                DagLodBounds bounds = merge_bounds(members);
                const usize target = static_cast<usize>(static_cast<f64>(merged.size() / 3u) * static_cast<f64>(o.simplify_ratio)) * 3u;
                std::vector<u32> simplified(merged.size());
                f32 simplifyError = 0.f;
                simplified.resize(meshopt_simplifyWithAttributes(
                    simplified.data(), merged.data(), merged.size(), positions.data(), n, sizeof(f32) * 3u, nullptr, 0u, nullptr,
                    0u, lock.data(), target, 3.0e38f, meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &simplifyError));
                const bool stuck = simplified.empty() ||
                                   static_cast<f64>(simplified.size()) > static_cast<f64>(merged.size()) * static_cast<f64>(o.stuck_ratio);
                if (stuck || !std::isfinite(simplifyError)) {
                    bounds.error = kDagErrorTerminal;
                    emit_group(members, bounds, depth, s, 0u, 0u);
                    stuckGroups.push_back(members);
                    continue;
                }
                bounds.error = std::max(bounds.error, round_up_f32(static_cast<f64>(std::max(simplifyError, 0.f))));
                const u32 g = static_cast<u32>(out.groups.size());
                const u32 firstChild = out.leaf_cluster_count + static_cast<u32>(out.lod_clusters.size());
                const std::vector<u32> children = clusterize(simplified, s, g, bounds);
                if (children.empty()) {
                    return set_error(error, "internal: simplified group produced no clusters");
                }
                emit_group(members, bounds, depth, s, firstChild, static_cast<u32>(children.size()));
                next.insert(next.end(), children.begin(), children.end());
            }
            for (const std::vector<u32>& members : stuckGroups) {
                freeze(members);
            }
            // Level scratch reset (only the canonical vertices this level touched).
            for (u32 c : pending) {
                for (u32 v : indices[c]) {
                    usedBy[remap[v]] = 0u;
                }
            }
            for (u32 c : pending) {
                std::vector<u32>().swap(indices[c]);
            }
            pending = std::move(next);
            ++depth;
        }
        if (!pending.empty()) {
            DagLodBounds bounds = merge_bounds(pending);
            bounds.error = kDagErrorTerminal;
            emit_group(pending, bounds, depth, s, 0u, 0u);
        }
        return true;
    }
};

} // namespace

bool build_cluster_dag(const MeshletMesh& base, const DagBuildOptions& o, ClusterDag& out, std::string* error) {
    out = ClusterDag{};
    if (o.group_size < 2u || !(o.simplify_ratio > 0.f && o.simplify_ratio < 1.f) ||
        !(o.stuck_ratio > 0.f && o.stuck_ratio <= 1.f) || o.max_levels == 0u || o.max_levels > 0xFFFFu ||
        !(o.sphere_slack >= 0.f && o.sphere_slack < 1.f) || !(o.error_slack >= 0.f && o.error_slack < 1.f) ||
        !(o.cone_weight >= 0.f && o.cone_weight <= 1.f)) {
        return set_error(error, "options out of range");
    }
    std::string why;
    if (!validate_meshlet_mesh(base, &why)) {
        return set_error(error, "base mesh invalid: " + why);
    }
    if (base.max_triangles < 4u || base.max_triangles % 4u != 0u) {
        return set_error(error, "base max_triangles must be a multiple of 4 (meshoptimizer limit)");
    }
    Builder b(base, o, out);
    b.n = base.vertex_count();
    DecodedVertices decoded;
    decode_vertices(base, decoded, o.backend);
    b.positions = std::move(decoded.positions);
    b.remap.resize(b.n);
    if (b.n > 0u) {
        meshopt_generatePositionRemap(b.remap.data(), b.positions.data(), b.n, sizeof(f32) * 3u);
    }
    b.permanentLock.assign(b.n, 0u);
    b.frozen.assign(b.n, 0u);
    {
        std::vector<u32> firstSubmesh(b.n, kDagNoGroup);
        for (const MeshletRecord& r : base.meshlets) {
            for (u32 i = 0; i < r.vertex_count; ++i) {
                const u32 c = b.remap[base.meshlet_vertices[r.vertex_offset + i]];
                if (firstSubmesh[c] == kDagNoGroup) {
                    firstSubmesh[c] = r.submesh;
                } else if (firstSubmesh[c] != r.submesh) {
                    b.permanentLock[c] = 1u;
                }
            }
        }
    }
    const u32 leaves = static_cast<u32>(base.meshlets.size());
    out.leaf_cluster_count = leaves;
    out.links.resize(leaves);
    b.indices.resize(leaves);
    b.self.resize(leaves);
    for (u32 c = 0; c < leaves; ++c) {
        const MeshletRecord& r = base.meshlets[c];
        DagLodBounds leaf{};
        for (u32 a = 0; a < 3u; ++a) {
            leaf.center[a] = r.center[a];
        }
        leaf.radius = r.radius;
        leaf.error = 0.f;
        b.self[c] = leaf;
        out.links[c].self = leaf;
        out.links[c].refined = kDagNoGroup;
        b.leaf_indices(c);
    }
    for (u32 s = 0; s < base.submeshes.size(); ++s) {
        if (!b.build_submesh(s, error)) {
            out = ClusterDag{};
            return false;
        }
    }
    // Exact culling bounds (AABB, sphere grown to every decoded vertex) for the LOD clusters.
    if (!out.lod_clusters.empty()) {
        bounds_kernel::Params bp{};
        bp.meshlets = kernel::make_span(out.lod_clusters.data(), static_cast<u32>(out.lod_clusters.size()));
        bp.meshlet_vertices = kernel::make_span(static_cast<const u32*>(out.lod_meshlet_vertices.data()),
                                                static_cast<u32>(out.lod_meshlet_vertices.size()));
        bp.positions = kernel::make_span(static_cast<const f32*>(b.positions.data()), static_cast<u32>(b.positions.size()));
        if (!kernel::launch(o.backend, bounds_kernel::make_launch(static_cast<u32>(out.lod_clusters.size())),
                            bounds_kernel::Kernel{}, bp)
                 .ok) {
            out = ClusterDag{};
            return set_error(error, "LOD cluster bounds launch failed");
        }
    }
    u32 maxLevel = 0;
    for (u32 c = 0; c < out.cluster_count(); ++c) {
        maxLevel = std::max(maxLevel, cluster_level(out, c));
    }
    out.level_count = out.cluster_count() == 0u ? 0u : maxLevel + 1u;
    if (!validate_cluster_dag(base, out, &why)) {
        out = ClusterDag{};
        return set_error(error, "internal: built DAG failed validation: " + why);
    }
    return true;
}

bool build_cluster_dag_mesh(const MeshletSource& source, const MeshletBuildOptions& meshlet_options,
                            const DagBuildOptions& dag_options, ClusterDagMesh& out, std::string* error) {
    out = ClusterDagMesh{};
    if (!build_meshlets(source, meshlet_options, out.base, error)) {
        return false;
    }
    out.base.version_minor = kClusterDagFormatVersionMinor;
    if (!build_cluster_dag(out.base, dag_options, out.dag, error)) {
        out = ClusterDagMesh{};
        return false;
    }
    return true;
}

} // namespace fuse::renderer::geometry::dag
