#include <fuse/cook/collision_cook.hpp>

#include <fuse/cook/mesh_cook.hpp>

#include <fstream>
#include <utility>
#include <vector>

namespace fuse::cook {

namespace {

physics::vec3 position(const CookedMesh& mesh, u32 vertex) {
    return {mesh.positions[vertex * 3u], mesh.positions[vertex * 3u + 1u], mesh.positions[vertex * 3u + 2u]};
}

} // namespace

const char* collision_cook_mode_name(CollisionCookMode mode) {
    switch (mode) {
    case CollisionCookMode::Hull:
        return "hull";
    case CollisionCookMode::TriMesh:
        return "mesh";
    case CollisionCookMode::Both:
        return "both";
    }
    return "unknown";
}

bool parse_collision_cook_mode(const std::string& text, CollisionCookMode& out) {
    if (text == "hull") {
        out = CollisionCookMode::Hull;
    } else if (text == "mesh" || text == "trimesh") {
        out = CollisionCookMode::TriMesh;
    } else if (text == "both") {
        out = CollisionCookMode::Both;
    } else {
        return false;
    }
    return true;
}

bool cook_collision(const CookedMesh& mesh, const CollisionCookOptions& options, physics::CollisionAsset& out,
                    CollisionCookReport* report, std::string* error) {
    out = physics::CollisionAsset{};
    CollisionCookReport local{};
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        out = physics::CollisionAsset{};
        return false;
    };
    const u32 vertexCount = mesh.vertex_count();
    if (vertexCount == 0u || mesh.indices.size() < 3u) {
        return fail("mesh has no triangles");
    }

    if (options.mode != CollisionCookMode::TriMesh) {
        physics::QuickHullOptions hullOptions{};
        hullOptions.distanceTolerance = options.hull_tolerance;
        // Point sets: every vertex, or the vertices each submesh's triangles use.
        std::vector<std::vector<physics::vec3>> sets;
        if (options.hull_per_submesh && !mesh.submeshes.empty()) {
            for (const CookedMesh::Submesh& sub : mesh.submeshes) {
                std::vector<u8> used(vertexCount, 0u);
                std::vector<physics::vec3> points;
                for (u32 i = sub.index_offset; i < sub.index_offset + sub.index_count && i < mesh.indices.size(); ++i) {
                    const u32 v = mesh.indices[i]; // FMSH indices are global
                    if (v < vertexCount && used[v] == 0u) {
                        used[v] = 1u;
                        points.push_back(position(mesh, v));
                    }
                }
                sets.push_back(std::move(points));
            }
        } else {
            std::vector<physics::vec3> points;
            points.reserve(vertexCount);
            for (u32 v = 0; v < vertexCount; ++v) {
                points.push_back(position(mesh, v));
            }
            sets.push_back(std::move(points));
        }
        for (usize s = 0; s < sets.size(); ++s) {
            physics::ConvexHull hull;
            std::string why;
            if (!physics::buildConvexHull(sets[s], hull, hullOptions, &why)) {
                return fail("hull " + std::to_string(s) + ": " + why);
            }
            local.hull_vertices += static_cast<u32>(hull.vertices.size());
            local.hull_faces += static_cast<u32>(hull.faces.size());
            out.hulls.push_back(std::move(hull));
        }
        local.hull_count = static_cast<u32>(out.hulls.size());
    }

    if (options.mode != CollisionCookMode::Hull) {
        std::vector<physics::vec3> vertices;
        vertices.reserve(vertexCount);
        for (u32 v = 0; v < vertexCount; ++v) {
            vertices.push_back(position(mesh, v));
        }
        // FMSH indices are global vertex indices (every submesh).
        const std::vector<u32>& indices = mesh.indices;
        physics::TriMesh triMesh;
        std::string why;
        if (!triMesh.build(vertices, indices, &why)) {
            return fail("triangle mesh: " + why);
        }
        local.mesh_triangles = triMesh.triangleCount();
        local.bvh_nodes = static_cast<u32>(triMesh.nodes().size());
        local.bvh_depth = triMesh.maxDepth();
        out.meshes.push_back(std::move(triMesh));
    }
    if (report != nullptr) {
        *report = local;
    }
    return true;
}

CookStubWriteResult cook_collision_file(const std::string& input_path, const std::string& output_path,
                                        const CollisionCookOptions& options, CollisionCookReport* report) {
    CookStubWriteResult result{};
    CookedMesh mesh;
    std::string error;
    if (!load_cooked_mesh(input_path, mesh, &error)) {
        result.failure = CookFailure::MalformedSource;
        result.note = "collision cook: " + error;
        return result;
    }
    physics::CollisionAsset asset;
    CollisionCookReport local{};
    if (!cook_collision(mesh, options, asset, &local, &error)) {
        result.failure = CookFailure::InvalidGeometry;
        result.note = "collision cook: " + error;
        return result;
    }
    const std::vector<u8> bytes = physics::serializeCollisionAsset(asset);
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        result.failure = CookFailure::WriteFailed;
        result.note = "collision cook: cannot write " + output_path;
        return result;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        result.failure = CookFailure::WriteFailed;
        result.note = "collision cook: write failed " + output_path;
        return result;
    }
    local.bytes = static_cast<u32>(bytes.size());
    if (report != nullptr) {
        *report = local;
    }
    result.ok = true;
    result.byteCount = local.bytes;
    result.note = std::string("collision (") + collision_cook_mode_name(options.mode) + ")";
    return result;
}

} // namespace fuse::cook
