// fuse_project_tool: project.json maintenance.
//   fuse_project_tool --migrate <project dir | project.json> [--dry-run]   schema 1 -> 2 (prints the new text)
//   fuse_project_tool --validate <project dir | project.json>              load + schema 2 rules, prints the plan
//   fuse_project_tool --new-app <dir> [--name=<name>]                      "Application (FUSE UI)" template
#include <fuse/project/loader.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::fprintf(stderr, "usage: fuse_project_tool --migrate <path> [--dry-run] | --validate <path> | "
                         "--new-app <dir> [--name=<name>]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string path;
    std::string name;
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--migrate" || arg == "--validate" || arg == "--new-app") {
            mode = arg;
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg.rfind("--name=", 0) == 0) {
            name = arg.substr(7);
        } else if (!arg.empty() && arg[0] != '-') {
            path = arg;
        } else {
            return usage();
        }
    }
    if (mode.empty() || path.empty()) {
        return usage();
    }
    if (mode == "--migrate") {
        const fuse::project::MigrateResult r = fuse::project::migrateProjectFile(path, !dryRun);
        if (!r.ok) {
            std::fprintf(stderr, "fuse_project_tool: %s: %s\n", r.path.c_str(), r.error.c_str());
            return 1;
        }
        std::fputs(r.text.c_str(), stdout);
        std::fprintf(stderr, "fuse_project_tool: %s: schema %u -> %u%s\n", r.path.c_str(), r.fromVersion,
                     fuse::project::kProjectSchemaVersion,
                     !r.changed ? " (already current)" : dryRun ? " (dry run, not written)" : " (written)");
        return 0;
    }
    if (mode == "--validate") {
        std::error_code ec;
        const fuse::project::LoadResult r = std::filesystem::is_directory(path, ec)
                                                ? fuse::project::loadFromDirectory(path)
                                                : fuse::project::loadFromFile(path);
        if (r.status != fuse::project::LoadStatus::Ok) {
            std::fprintf(stderr, "fuse_project_tool: invalid: %s\n", r.error.c_str());
            return 1;
        }
        const fuse::project::InitPlan plan = fuse::project::planInit(r.manifest);
        std::printf("%s: schema %u, kind %s, startup [%s], deferred [%s]\n", r.manifest.name.c_str(),
                    r.manifest.schemaVersion, std::string(fuse::project::toString(r.manifest.kind)).c_str(),
                    fuse::project::hostFeatureList(plan.startup).c_str(),
                    fuse::project::hostFeatureList(plan.deferred).c_str());
        return 0;
    }
    const fuse::project::SaveResult r = fuse::project::createAppProject(path, name);
    if (!r.ok) {
        std::fprintf(stderr, "fuse_project_tool: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("created %s\n", r.path.c_str());
    return 0;
}
