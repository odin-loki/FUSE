#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>
#include <fuse/project/loader.hpp>

#include "Cook/fuselevel_cook_stub.hpp"

#include <fuse/cook/audio_cook.hpp>
#include <fuse/cook/collision_cook.hpp>

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
                 "  fuse_cook --mesh --input <path> --output <path>   Mesh cook (.fusemesh FMSH; see mesh options)\n"
                 "  fuse_cook --texture --input <path> --output <path> Texture cook (.png/.tga/.jpg/.hdr/.ktx2 in;\n"
                 "             .fusetex out, or .ktx2 out for KTX2 transport export)\n"
                 "  fuse_cook --audio --input <path> --output <path>  Audio cook (.wav/.flac/.ogg in; .fuseaudio out)\n"
                 "  fuse_cook --collision --input <mesh.fusemesh> --output <mesh.fusecol>  Collision cook (E18)\n"
                 "             [--collision-mode hull|mesh|both] [--hull-per-submesh]: quickhull hulls and/or the\n"
                 "             static triangle mesh + BVH, loaded by fuse::physics::loadCollisionAssetFile\n"
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
                 "  --quantize     FMSH v2 quantised positions (unorm16) and normals (oct16)\n"
                 "  --compress     Same as --quantize (the import descriptor's `compress`)\n"
                 "  --lods <N>     Discrete LOD chain of N levels including LOD 0 (meshoptimizer)\n"
                 "  --meshlets     FMSH v2 meshlet table (renderer WP-1.2)\n"
                 "  --dag          FMSH v2 cluster DAG (renderer WP-5.2; implies --meshlets)\n"
                 "  --pages        Also write <output>.fusepages, the WP-5.3 cluster page file (implies --dag)\n"
                 "  --page-bytes <N>  Page payload capacity for --pages (multiple of 16, >= 1024; default 65536)\n"
                 "Audio options (MP-B7.9-AUDIO-IMPORT / AP-W8.3; any of these cooks directly, without the cook cache):\n"
                 "  --bed | --oneshot  Sound class: -23 LUFS + Vorbis q4, or -16 LUFS + q5 (default: by length)\n"
                 "  --loop             Seamless loop (zero-crossing crossfade, loop points in the header)\n"
                 "  --crossfade-ms <N> Loop crossfade length (default 50)\n"
                 "  --rate <Hz>        Target sample rate (default 48000; 0 keeps the source rate)\n"
                 "  --mono             Downmix to mono\n"
                 "  --lufs <L>         Integrated loudness target (EBU R128 / BS.1770)\n"
                 "  --peak-normalise   Peak-normalise to -1 dBFS instead of loudness\n"
                 "  --no-normalise     Keep the source level\n"
                 "  --no-trim          Keep leading/trailing silence\n"
                 "  --quality <q>      Vorbis VBR quality -0.1 .. 1.0 (q4 = 0.4)\n"
                 "  --pcm              FUSEAUDIO_PCM_F32 instead of Ogg Vorbis\n");
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
    bool compress = false;
    fuse::u32 lodCount = 0;
    bool meshlets = false;
    bool clusterDag = false;
    bool clusterPages = false;
    fuse::u32 pageBytes = 64u * 1024u;
    fuse::cook::AudioCookOptions audioOptions;
    bool audioOptionsGiven = false;
    bool collisionCook = false;
    fuse::cook::CollisionCookOptions collisionOptions;

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
        } else if (arg == "--compress") {
            compress = true;
        } else if ((arg == "--lods" || arg == "--page-bytes") && i + 1 < argc) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(argv[++i], &end, 10);
            if (end == nullptr || *end != '\0' || value > 0xFFFFFFFFul) {
                std::fprintf(stderr, "fuse_cook: %s expects an unsigned integer\n", arg.c_str());
                fuse::core::shutdown();
                return EXIT_FAILURE;
            }
            (arg == "--lods" ? lodCount : pageBytes) = static_cast<fuse::u32>(value);
        } else if (arg == "--meshlets") {
            meshlets = true;
        } else if (arg == "--dag") {
            clusterDag = true;
        } else if (arg == "--pages") {
            clusterPages = true;
        } else if (arg == "--bed" || arg == "--oneshot") {
            audioOptions.audio_class = arg == "--bed" ? fuse::cook::AudioClass::Bed : fuse::cook::AudioClass::OneShot;
            audioOptionsGiven = true;
        } else if (arg == "--loop") {
            audioOptions.make_loop = true;
            audioOptionsGiven = true;
        } else if (arg == "--mono") {
            audioOptions.force_mono = true;
            audioOptionsGiven = true;
        } else if (arg == "--pcm") {
            audioOptions.format = fuse::cook::AudioCookFormat::PcmF32;
            audioOptionsGiven = true;
        } else if (arg == "--peak-normalise") {
            audioOptions.normalise = fuse::cook::AudioNormalise::Peak;
            audioOptionsGiven = true;
        } else if (arg == "--no-normalise") {
            audioOptions.normalise = fuse::cook::AudioNormalise::None;
            audioOptionsGiven = true;
        } else if (arg == "--no-trim") {
            audioOptions.trim_silence = false;
            audioOptionsGiven = true;
        } else if ((arg == "--rate" || arg == "--crossfade-ms") && i + 1 < argc) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(argv[++i], &end, 10);
            if (end == nullptr || *end != '\0' || value > 768000ul) {
                std::fprintf(stderr, "fuse_cook: %s expects an unsigned integer\n", arg.c_str());
                fuse::core::shutdown();
                return EXIT_FAILURE;
            }
            (arg == "--rate" ? audioOptions.target_sample_rate : audioOptions.loop_crossfade_ms) =
                static_cast<fuse::u32>(value);
            audioOptionsGiven = true;
        } else if ((arg == "--lufs" || arg == "--quality") && i + 1 < argc) {
            char* end = nullptr;
            const double value = std::strtod(argv[++i], &end);
            if (end == nullptr || *end != '\0' || !(value > -100.0 && value < 100.0)) {
                std::fprintf(stderr, "fuse_cook: %s expects a number\n", arg.c_str());
                fuse::core::shutdown();
                return EXIT_FAILURE;
            }
            (arg == "--lufs" ? audioOptions.target_lufs : audioOptions.ogg_quality) = static_cast<float>(value);
            audioOptionsGiven = true;
        } else if (arg == "--collision") {
            collisionCook = true;
        } else if (arg == "--collision-mode" && i + 1 < argc) {
            if (!fuse::cook::parse_collision_cook_mode(argv[++i], collisionOptions.mode)) {
                std::fprintf(stderr, "fuse_cook: unknown --collision-mode %s (hull|mesh|both)\n", argv[i]);
                fuse::core::shutdown();
                return EXIT_FAILURE;
            }
        } else if (arg == "--hull-per-submesh") {
            collisionOptions.hull_per_submesh = true;
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

    if (collisionCook) {
        if (inputPath.empty() || outputPath.empty()) {
            printUsage();
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }
        fuse::cook::CollisionCookReport report;
        const fuse::cook::CookStubWriteResult cooked =
            fuse::cook::cook_collision_file(inputPath, outputPath, collisionOptions, &report);
        std::printf("  [collision] %s -> %s (%s) %s\n", inputPath.c_str(), outputPath.c_str(),
                    cooked.ok ? "cooked" : fuse::cook::cookFailureName(cooked.failure), cooked.note.c_str());
        if (cooked.ok) {
            std::printf("fuse_cook: collision %u hull(s) (%u vertices, %u faces), %u mesh triangles, BVH %u nodes "
                        "depth %u, %u bytes\n",
                        report.hull_count, report.hull_vertices, report.hull_faces, report.mesh_triangles,
                        report.bvh_nodes, report.bvh_depth, report.bytes);
        }
        fuse::core::shutdown();
        return cooked.ok ? EXIT_SUCCESS : EXIT_FAILURE;
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

        if (audioCook && audioOptionsGiven) {
            // Explicit audio options go straight to the cook chain (AudioImportDesc carries only the
            // defaults through AssetCooker).
            fuse::cook::AudioCookReport report;
            const fuse::cook::CookStubWriteResult cooked =
                fuse::cook::cook_audio_file(inputPath, outputPath, audioOptions, &report);
            std::printf("  [audio] %s -> %s (%s) %s\n", inputPath.c_str(), outputPath.c_str(),
                        cooked.ok ? "cooked" : fuse::cook::cookFailureName(cooked.failure), cooked.note.c_str());
            if (cooked.ok) {
                std::printf("fuse_cook: audio %s %u Hz x%u, %u frames, source %.2f LUFS -> %.2f LUFS (gain %.2f dB, "
                            "peak %.2f dBFS%s)%s\n",
                            fuse::cook::audio_class_name(report.resolved_class), report.output_rate,
                            report.output_channels, report.output_frames, report.source_lufs, report.output_lufs,
                            report.gain_db, static_cast<double>(report.output_peak_dbfs),
                            report.peak_limited ? ", limited" : "",
                            report.looped ? (report.loop_seam_click ? ", loop seam CLICK" : ", loop seam ok") : "");
            }
            fuse::core::shutdown();
            return cooked.ok && !report.loop_seam_click ? EXIT_SUCCESS : EXIT_FAILURE;
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
            desc.compress = compress;
            desc.generate_lods = lodCount > 1u;
            if (lodCount > 1u) {
                desc.lod_count = lodCount;
            }
            desc.meshlets = meshlets;
            desc.cluster_dag = clusterDag;
            desc.cluster_pages = clusterPages;
            desc.page_bytes = pageBytes;
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
