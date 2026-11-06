#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/importer.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "fuse_import — FUSE project import dry-run (U7)\n"
                 "Usage:\n"
                 "  fuse_import --project <dir>            Load project.json and dry-run default worlds\n"
                 "  fuse_import --source <file.mis|.cs>    Dry-run a single legacy source file\n");
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();

    std::string projectDir;
    std::string sourcePath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (arg == "--source" && i + 1 < argc) {
            sourcePath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            fuse::core::shutdown();
            return EXIT_SUCCESS;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            printUsage();
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }
    }

    if (projectDir.empty() && sourcePath.empty()) {
        printUsage();
        fuse::core::shutdown();
        return EXIT_FAILURE;
    }

    fuse::project::ProjectManifest manifest;
    if (!projectDir.empty()) {
        const fuse::project::LoadResult loadResult = fuse::project::loadFromDirectory(projectDir);
        if (loadResult.status != fuse::project::LoadStatus::Ok) {
            std::fprintf(stderr, "fuse_import: failed to load project: %s\n", loadResult.error.c_str());
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }
        manifest = loadResult.manifest;
        fuse::log::info("fuse_import: loaded project '%s' from %s",
                        manifest.name.c_str(),
                        projectDir.c_str());

        const fuse::project::ProjectVfsMountResult vfsMount =
            fuse::project::mountProjectAssetRoots(manifest);
        std::printf("fuse_import: vfs %s (added=%u remapped=%u game=%s t3d=%s t2d=%s)\n",
                    vfsMount.note.c_str(),
                    vfsMount.mountsAdded,
                    vfsMount.mountsRemapped,
                    vfsMount.gameMount ? "yes" : "no",
                    vfsMount.t3dMount ? "yes" : "no",
                    vfsMount.t2dMount ? "yes" : "no");

        const char* remapSamples[] = {
            "data/materials/FloorGray.mat",
            "Data\\materials\\FloorGray.mat",
            "./game/textures/albedo.png",
            "assets/meshes/cube.dae",
            "shaders/common/screen.cs",
        };
        std::printf("fuse_import: vfs remaps:\n");
        for (const char* sample : remapSamples) {
            std::printf("  %s -> %s\n", sample, fuse::project::remapLegacyAssetPath(sample).c_str());
        }

        const std::string cachePath = fuse::project::defaultCookCachePath(manifest.projectRoot);
        if (!cachePath.empty()) {
            const bool present = std::filesystem::exists(cachePath);
            std::printf("fuse_import: cook cache %s (%s)\n",
                        cachePath.c_str(),
                        present ? "present" : "absent");
            if (present) {
                fuse::project::CookCache cache;
                if (cache.load(cachePath)) {
                    std::printf("fuse_import: cook cache loaded (%zu entries)\n",
                                static_cast<std::size_t>(cache.entry_count()));
                }
            }
        }
    }

    const fuse::project::ImportDryRunResult importResult =
        sourcePath.empty() ? fuse::project::importDryRun(manifest, "")
                           : fuse::project::importDryRun(manifest, sourcePath);

    for (const fuse::project::ImportRecord& record : importResult.worlds) {
        std::printf("  [%s] %s -> world '%s' handle=%u (%s)\n",
                    fuse::project::importSourceKindName(record.kind),
                    record.sourcePath.c_str(),
                    record.worldName.c_str(),
                    record.worldHandle.index(),
                    record.note.c_str());
    }

    std::printf("fuse_import: %s\n", importResult.summary.c_str());

    fuse::core::shutdown();
    return importResult.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
