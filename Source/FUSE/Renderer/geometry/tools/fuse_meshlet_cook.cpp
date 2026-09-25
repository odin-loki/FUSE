// fuse_meshlet_cook — WP-1.2 offline meshlet cook (docs/unification/RENDERER-EXECUTION.md).
//
//   fuse_meshlet_cook <file.fusemesh>...                 write <file>.fusemeshlet next to each input
//   fuse_meshlet_cook --source <model> --output <x.fusemesh>
//                                                        assimp import -> FMSH v1 + meshlet sidecar
//   fuse_meshlet_cook --verify <file.fusemeshlet>...     parse + validate, print a summary
//
// Exit code 0 on success, 1 on any failure.

#include <fuse/renderer/geometry/meshlet_cook_hook.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

namespace geo = fuse::renderer::geometry;

void print_summary(const std::string& path, const geo::MeshletMesh& m) {
    std::printf("%s: FMLT 1.%u, %u vertices, %u triangles, %zu meshlets, %zu submeshes, limits %u/%u\n", path.c_str(),
                static_cast<unsigned>(m.version_minor), m.vertex_count(), m.triangle_count(), m.meshlets.size(),
                m.submeshes.size(), m.max_vertices, m.max_triangles);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        std::fprintf(stderr,
                     "usage: fuse_meshlet_cook <file.fusemesh>...\n"
                     "       fuse_meshlet_cook --source <model> --output <file.fusemesh>\n"
                     "       fuse_meshlet_cook --verify <file.fusemeshlet>...\n");
        return argc < 2 ? 1 : 0;
    }
    int failures = 0;
    if (std::strcmp(argv[1], "--verify") == 0) {
        for (int i = 2; i < argc; ++i) {
            geo::MeshletMesh mesh;
            std::string error;
            if (!geo::load_meshlet_file(argv[i], mesh, &error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                ++failures;
                continue;
            }
            print_summary(argv[i], mesh);
        }
        return failures == 0 ? 0 : 1;
    }
    if (std::strcmp(argv[1], "--source") == 0) {
        if (argc != 5 || std::strcmp(argv[3], "--output") != 0) {
            std::fprintf(stderr, "fuse_meshlet_cook: --source <model> --output <file.fusemesh>\n");
            return 1;
        }
        const fuse::cook::CookStubWriteResult r =
            fuse::cook::cook_mesh_file(argv[2], argv[4], geo::with_meshlet_sidecar());
        std::printf("fuse_meshlet_cook: %s\n", r.note.c_str());
        return r.ok ? 0 : 1;
    }
    for (int i = 1; i < argc; ++i) {
        geo::MeshletMesh mesh;
        std::string error;
        if (!geo::cook_meshlet_sidecar(argv[i], &error, &mesh)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            ++failures;
            continue;
        }
        print_summary(geo::meshlet_sidecar_path(argv[i]), mesh);
    }
    return failures == 0 ? 0 : 1;
}
