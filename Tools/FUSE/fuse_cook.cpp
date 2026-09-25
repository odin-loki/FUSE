#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>
#include <fuse/project/loader.hpp>

#include "Cook/fuselevel_cook_stub.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "fuse_cook — FUSE offline asset cook dry-run (B7.9)\n"
                 "Usage:\n"
                 "  fuse_cook --project <dir> [--dry-run]     Plan default cook manifest for project\n"
                 "  fuse_cook --manifest <cook.json> [--dry-run]  Load cook_manifest.json and plan/cook\n"
                 "  fuse_cook --mesh --input <path> --output <path>   Stub mesh cook\n"
                 "  fuse_cook --texture --input <path> --output <path> Texture cook (.png/.tga/.jpg/.hdr/.ktx2 in;\n"
                 "             .fusetex out, or .ktx2 out for KTX2 transport export)\n"
                 "  fuse_cook --audio --input <path> --output <path>  Stub audio cook\n"
                 "  fuse_cook --fuselevel --mis <file.mis> --output <world.fuselevel>\n"
                 "  fuse_cook --fuselevel --module <file.cs> --output <world.fuselevel>\n"
                 "Options:\n"
                 "  --lenient  Cook undecodable mesh/texture sources to labelled placeholder stubs instead\n"
                 "             of failing. By default cooks are strict: malformed meshes and corrupt,\n"
                 "             zero-size or oversize textures fail (exit 1) with a specific status\n"
                 "  --strict   Accepted for compatibility (strict is the default)\n"
                 "Texture options (asset plan W0.3/W0.4):\n"
                 "  --format <BC1|BC4|BC5|BC6H|BC7>  Block format (default BC7)\n"
                 "  --normal-map   Tangent-space normal map: BC5, linear, renormalised mips\n"
                 "  --hdr          HDR source: BC6H\n"
                 "  --linear       Data texture (not sRGB)\n"
                 "  --no-mips      Level 0 only\n"
                 "Mesh options (asset plan W0.1):\n"
                 "  --fmsh-v2      FMSH v2 streams (tangent, uv1, colour, skin, material slots)\n"
                 "  --quantize     FMSH v2 quantised positions (unorm16) and normals (oct16)\n");
}

/// Manifest paths are relative to the manifest's directory, not the caller's working directory.
void resolveManifestPaths(fuse::project::CookManifest& manifest) {
    if (manifest.project_root.empty()) {
        return;
    }
    const std::filesystem::path root(manifest.project_root);
    auto resolve = [&root](std::string& path) {
        if (!path.empty() && std::filesystem::path(path).is_relative()) {
            path = (root / path).lexically_normal().string();
        }
    };
    for (fuse::project::CookManifestEntry& entry : manifest.assets) {
        resolve(entry.source_path);
        resolve(entry.output_path);
        for (std::string& dependency : entry.dependencies) {
            resolve(dependency);
        }
    }
}

int printCookResult(const fuse::project::CookBatchResult& result,
                    const fuse::project::CookCacheStats* cacheStats = nullptr) {
    fuse::u32 cacheHits = 0;
    for (const fuse::project::CookRecord& record : result.records) {
        if (record.cache_hit) {
            ++cacheHits;
        }
        const char* cacheTag = record.cache_hit ? " [cache hit, skipped re-cook]" : "";
        std::printf("  [%s] %s -> %s (%s) %s%s\n",
                    fuse::project::cookAssetKindName(record.kind),
                    record.source_path.c_str(),
                    record.output_path.c_str(),
                    fuse::project::cookStatusName(record.status),
                    record.note.c_str(),
                    cacheTag);
    }

    std::printf("fuse_cook: %s", result.summary.c_str());
    if (cacheHits > 0) {
        std::printf(" (%u cache hit%s, skipped re-cook)", cacheHits, cacheHits == 1 ? "" : "s");
    }
    if (cacheStats != nullptr && (cacheStats->hits > 0 || cacheStats->misses > 0)) {
        std::printf(" [cache stats: hits=%llu misses=%llu]",
                    static_cast<unsigned long long>(cacheStats->hits),
                    static_cast<unsigned long long>(cacheStats->misses));
    }
    std::printf("\n");
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
    fuse::project::ImportValidation validation = fuse::project::ImportValidation::Strict;
    bool meshCook = false;
    bool textureCook = false;
    bool audioCook = false;
    bool fuselevelCook = false;
    std::string missionPath;
    std::string modulePath;
    std::string textureFormat;
    bool normalMap = false;
    bool hdrTexture = false;
    bool linearTexture = false;
    bool noMips = false;
    bool fmshV2 = false;
    bool quantize = false;

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
        } else if (arg == "--strict") {
            validation = fuse::project::ImportValidation::Strict;
        } else if (arg == "--lenient") {
            validation = fuse::project::ImportValidation::Lenient;
        } else if (arg == "--mesh") {
            meshCook = true;
        } else if (arg == "--texture") {
            textureCook = true;
        } else if (arg == "--audio") {
            audioCook = true;
        } else if (arg == "--fuselevel") {
            fuselevelCook = true;
        } else if (arg == "--mis" && i + 1 < argc) {
            missionPath = argv[++i];
        } else if (arg == "--module" && i + 1 < argc) {
            modulePath = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            textureFormat = argv[++i];
        } else if (arg == "--normal-map") {
            normalMap = true;
        } else if (arg == "--hdr") {
            hdrTexture = true;
        } else if (arg == "--linear") {
            linearTexture = true;
        } else if (arg == "--no-mips") {
            noMips = true;
        } else if (arg == "--fmsh-v2") {
            fmshV2 = true;
        } else if (arg == "--quantize") {
            quantize = true;
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

    if (fuselevelCook) {
        if (outputPath.empty() || (missionPath.empty() && modulePath.empty())) {
            printUsage();
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        const fuse::cook::FuselevelCookResult cooked =
            !missionPath.empty() ? fuse::cook::cookFuselevelFromMis(missionPath, outputPath)
                                 : fuse::cook::cookFuselevelFromModule(modulePath, outputPath);
        std::printf("fuse_cook: %s (%u entities)\n", cooked.note.c_str(), cooked.entityCount);
        fuse::core::shutdown();
        return cooked.status == fuse::cook::FuselevelCookStatus::Ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (meshCook || textureCook || audioCook) {
        if (inputPath.empty() || outputPath.empty()) {
            printUsage();
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        fuse::project::AssetCooker cooker;
        cooker.set_import_validation(validation);
        const std::string cachePath = fuse::project::defaultCookCachePath(".");
        cooker.cache().load(cachePath);

        fuse::project::CookRecord record;
        if (meshCook) {
            fuse::project::MeshImportDesc desc;
            desc.input_path = inputPath;
            desc.output_path = outputPath;
            desc.fmsh_v2_streams = fmshV2;
            desc.quantize_vertices = quantize;
            record = cooker.cook_mesh(desc);
        } else if (textureCook) {
            fuse::project::TextureImportDesc desc;
            desc.input_path = inputPath;
            desc.output_path = outputPath;
            desc.is_normal_map = normalMap;
            desc.is_hdr = hdrTexture;
            desc.generate_mipmaps = !noMips;
            if (linearTexture) {
                desc.color_space = fuse::project::TextureImportDesc::ColorSpace::Linear;
            }
            if (!textureFormat.empty()) {
                using Compression = fuse::project::TextureImportDesc::Compression;
                if (textureFormat == "BC1" || textureFormat == "bc1") {
                    desc.compression = Compression::BC1;
                } else if (textureFormat == "BC4" || textureFormat == "bc4") {
                    desc.compression = Compression::BC4;
                } else if (textureFormat == "BC5" || textureFormat == "bc5") {
                    desc.compression = Compression::BC5;
                } else if (textureFormat == "BC6H" || textureFormat == "bc6h") {
                    desc.is_hdr = true;
                } else if (textureFormat == "BC7" || textureFormat == "bc7") {
                    desc.compression = Compression::BC7;
                } else {
                    std::fprintf(stderr, "fuse_cook: unknown --format %s\n", textureFormat.c_str());
                    fuse::core::shutdown();
                    return EXIT_FAILURE;
                }
            }
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
        result.summary = record.ok ? "single asset cook ok" : "single asset cook failed";
        cooker.cache().save(cachePath);
        const int exitCode = printCookResult(result, &cooker.cache().stats());
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

        fuse::project::CookManifest manifest = loaded.manifest;
        resolveManifestPaths(manifest);

        fuse::project::ImportPipeline pipeline;
        pipeline.set_project_root(manifest.project_root);
        pipeline.cooker().set_import_validation(validation);
        const std::string cachePath =
            fuse::project::defaultCookCachePath(manifest.project_root);
        if (!dryRun) {
            pipeline.load_cook_cache(cachePath);
            if (pipeline.cooker().would_reconcile_invalidation(manifest)) {
                const fuse::u32 invalidated =
                    pipeline.cooker().invalidate_stale_dependency_hashes(manifest);
                std::printf("fuse_cook: reconciled %u stale cache entries\n", invalidated);
            }
        }
        pipeline.plan_from_manifest(manifest);
        const fuse::project::CookBatchResult result = pipeline.execute(dryRun);
        if (!dryRun) {
            pipeline.save_cook_cache(cachePath);
        }
        const int exitCode =
            printCookResult(result, dryRun ? nullptr : &pipeline.cooker().cache().stats());
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
        pipeline.cooker().set_import_validation(validation);
        const std::string cachePath = fuse::project::defaultCookCachePath(projectDir);
        if (!dryRun) {
            pipeline.load_cook_cache(cachePath);
            if (pipeline.cooker().would_reconcile_invalidation(manifest)) {
                const fuse::u32 invalidated =
                    pipeline.cooker().invalidate_stale_dependency_hashes(manifest);
                std::printf("fuse_cook: reconciled %u stale cache entries\n", invalidated);
            }
        }
        pipeline.plan_from_manifest(manifest);
        const fuse::project::CookBatchResult result = pipeline.execute(dryRun);
        if (!dryRun) {
            pipeline.save_cook_cache(cachePath);
        }
        const int exitCode =
            printCookResult(result, dryRun ? nullptr : &pipeline.cooker().cache().stats());
        fuse::core::shutdown();
        return exitCode;
    }

    printUsage();
    fuse::core::shutdown();
    return EXIT_FAILURE;
}
