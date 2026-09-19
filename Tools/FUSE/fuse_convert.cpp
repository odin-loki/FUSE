#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/world_converter.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "fuse_convert — FUSE legacy world converter (U7)\n"
                 "Usage:\n"
                 "  fuse_convert --mis <file.mis> --output <world.fuselevel>\n"
                 "  fuse_convert --module <file.cs> --output <world.fuselevel>\n"
                 "  fuse_convert --project <dir> [--output-dir <dir>]\n");
}

std::string joinPath(const std::string& root, const std::string& relative) {
    if (root.empty()) {
        return relative;
    }

    std::string path = root;
    if (path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += relative;
    return path;
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();

    std::string projectDir;
    std::string missionPath;
    std::string modulePath;
    std::string outputPath;
    std::string outputDir;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--project" && i + 1 < argc) {
            projectDir = argv[++i];
        } else if (arg == "--mis" && i + 1 < argc) {
            missionPath = argv[++i];
        } else if (arg == "--module" && i + 1 < argc) {
            modulePath = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--output-dir" && i + 1 < argc) {
            outputDir = argv[++i];
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

    if (!missionPath.empty()) {
        if (outputPath.empty()) {
            std::fprintf(stderr, "fuse_convert: --output is required with --mis\n");
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        const fuse::project::ConvertResult result =
            fuse::project::convertT3DMissionToFuselevel(missionPath, outputPath);
        std::printf("fuse_convert: %s\n", result.note.c_str());
        fuse::core::shutdown();
        return result.status == fuse::project::ConvertStatus::Ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (!modulePath.empty()) {
        if (outputPath.empty()) {
            std::fprintf(stderr, "fuse_convert: --output is required with --module\n");
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }

        const fuse::project::ConvertResult result =
            fuse::project::convertT2DModuleToFuselevel(modulePath, outputPath);
        std::printf("fuse_convert: %s\n", result.note.c_str());
        fuse::core::shutdown();
        return result.status == fuse::project::ConvertStatus::Ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (projectDir.empty()) {
        printUsage();
        fuse::core::shutdown();
        return EXIT_FAILURE;
    }

    const fuse::project::LoadResult loadResult = fuse::project::loadFromDirectory(projectDir);
    if (loadResult.status != fuse::project::LoadStatus::Ok) {
        std::fprintf(stderr, "fuse_convert: failed to load project: %s\n", loadResult.error.c_str());
        fuse::core::shutdown();
        return EXIT_FAILURE;
    }

    const std::vector<fuse::project::ConvertResult> results =
        fuse::project::convertManifestWorlds(loadResult.manifest, outputDir);

    bool ok = !results.empty();
    for (const fuse::project::ConvertResult& result : results) {
        std::printf("  %s\n", result.note.c_str());
        ok = ok && result.status == fuse::project::ConvertStatus::Ok;
    }

    if (results.empty()) {
        std::fprintf(stderr, "fuse_convert: no convertible legacy worlds in project manifest\n");
        ok = false;
    }

    fuse::core::shutdown();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
