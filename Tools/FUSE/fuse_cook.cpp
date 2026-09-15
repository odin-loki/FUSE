#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>
#include <fuse/project/loader.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "fuse_cook — FUSE offline asset cook dry-run (B7.9)\n"
                 "Usage:\n"
                 "  fuse_cook --project <dir> [--dry-run]     Plan default cook manifest for project\n"
                 "  fuse_cook --manifest <cook.json> [--dry-run]  Load cook_manifest.json and plan/cook\n"
                 "  fuse_cook --mesh --input <path> --output <path>   Stub mesh cook\n"
                 "  fuse_cook --texture --input <path> --output <path> Stub texture cook\n"
                 "  fuse_cook --audio --input <path> --output <path>  Stub audio cook\n");
}

int printCookResult(const fuse::project::CookBatchResult& result) {
    for (const fuse::project::CookRecord& record : result.records) {
        std::printf("  [%s] %s -> %s (%s) %s\n",
                    fuse::project::cookAssetKindName(record.kind),
                    record.source_path.c_str(),
                    record.output_path.c_str(),
                    fuse::project::cookStatusName(record.status),
                    record.note.c_str());
    }

    std::printf("fuse_cook: %s\n", result.summary.c_str());
    return result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();

    std::string projectDir;
    std::string manifestPath;
    std::string inputPath;
    std::string outputPath;
    bool dryRun = false;
    bool meshCook = false;
    bool textureCook = false;
    bool audioCook = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (arg == "--manifest" && i + 1 < argc) {
            manifestPath = argv[++i];
        } else if (arg == "--input" && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--mesh") {
            meshCook = true;
        } else if (arg == "--texture") {
            textureCook = true;
        } else if (arg == "--audio") {
            audioCook = true;
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

    if (meshCook || textureCook || audioCook) {
        if (inputPath.empty() || outputPath.empty()) {
            printUsage();
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        fuse::project::AssetCooker cooker;
        fuse::project::CookRecord record;
        if (meshCook) {
            fuse::project::MeshImportDesc desc;
            desc.input_path = inputPath;
            desc.output_path = outputPath;
            record = cooker.cook_mesh(desc);
        } else if (textureCook) {
            fuse::project::TextureImportDesc desc;
            desc.input_path = inputPath;
            desc.output_path = outputPath;
            record = cooker.cook_texture(desc);
        } else {
            fuse::project::AudioImportDesc desc;
            desc.input_path = inputPath;
            desc.output_path = outputPath;
            record = cooker.cook_audio(desc);
        }

        fuse::project::CookBatchResult result;
        result.records.push_back(record);
        result.ok = record.ok;
        result.summary = record.ok ? "single asset cook stub ok" : "single asset cook stub failed";
        const int exitCode = printCookResult(result);
        fuse::core::shutdown();
        return exitCode;
    }

    if (!manifestPath.empty()) {
        const fuse::project::CookManifestLoadResult loaded = fuse::project::loadCookManifestFromFile(manifestPath);
        if (loaded.status != fuse::project::CookManifestLoadStatus::Ok) {
            std::fprintf(stderr, "fuse_cook: failed to load manifest: %s\n", loaded.error.c_str());
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        fuse::project::ImportPipeline pipeline;
        pipeline.set_project_root(loaded.manifest.project_root);
        pipeline.plan_from_manifest(loaded.manifest);
        const fuse::project::CookBatchResult result = pipeline.execute(dryRun);
        const int exitCode = printCookResult(result);
        fuse::core::shutdown();
        return exitCode;
    }

    if (!projectDir.empty()) {
        const fuse::project::LoadResult loadResult = fuse::project::loadFromDirectory(projectDir);
        if (loadResult.status != fuse::project::LoadStatus::Ok) {
            std::fprintf(stderr, "fuse_cook: failed to load project: %s\n", loadResult.error.c_str());
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        fuse::log::info("fuse_cook: loaded project '%s' from %s",
                        loadResult.manifest.name.c_str(),
                        projectDir.c_str());

        fuse::project::CookManifest manifest = fuse::project::makeDefaultCookManifest(projectDir);
        if (!loadResult.manifest.defaultWorld3D.empty()) {
            fuse::project::CookManifestEntry levelEntry;
            levelEntry.kind = fuse::project::CookAssetKind::Mesh;
            levelEntry.source_path = loadResult.manifest.defaultWorld3D;
            levelEntry.output_path = "cooked/" + loadResult.manifest.defaultWorld3D + ".fusecook";
            manifest.assets.push_back(levelEntry);
        }

        fuse::project::ImportPipeline pipeline;
        pipeline.set_project_root(projectDir);
        pipeline.plan_from_manifest(manifest);
        const fuse::project::CookBatchResult result = pipeline.execute(dryRun);
        const int exitCode = printCookResult(result);
        fuse::core::shutdown();
        return exitCode;
    }

    printUsage();
    fuse::core::shutdown();
    return EXIT_FAILURE;
}
