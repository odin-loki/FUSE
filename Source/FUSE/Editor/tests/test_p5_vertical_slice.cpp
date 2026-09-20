#include <fuse/core/init.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/hierarchy_model.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR ""
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::filesystem::path findDemo3dEmpty() {
    const std::filesystem::path relative =
        std::filesystem::path("Samples") / "unification" / "demo_3d_empty";

    const std::filesystem::path fromMacro = std::filesystem::path(FUSE_SOURCE_DIR) / relative;
    if (!std::string(FUSE_SOURCE_DIR).empty() && std::filesystem::exists(fromMacro / "project.json")) {
        return fromMacro;
    }

    std::filesystem::path dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        const std::filesystem::path candidate = dir / relative;
        if (std::filesystem::exists(candidate / "project.json")) {
            return candidate;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }

    return {};
}

std::filesystem::path makeTempProjectDir() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "fuse_editor_p5_slice";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

bool copyDemoToTemp(const std::filesystem::path& source, const std::filesystem::path& dest) {
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        std::fprintf(stderr, "create temp project dir failed: %s\n", ec.message().c_str());
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
        if (ec) {
            std::fprintf(stderr, "iterate demo_3d_empty failed: %s\n", ec.message().c_str());
            return false;
        }
        const std::filesystem::path destPath = dest / entry.path().filename();
        std::filesystem::copy(entry.path(), destPath,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            std::fprintf(stderr, "copy demo_3d_empty failed: %s\n", ec.message().c_str());
            return false;
        }
    }

    return std::filesystem::exists(dest / "project.json");
}

void postProjectOpen(fuse::editor::EditorHost& host, const std::string& projectRoot,
                     const std::string& projectName) {
    fuse::editor::EditorCommand rootCmd;
    rootCmd.kind = fuse::editor::CommandKind::SetProperty;
    rootCmd.propertyName = "project.root";
    rootCmd.propertyValue = projectRoot;
    host.postFromUi(std::move(rootCmd));

    fuse::editor::EditorCommand projectCmd;
    projectCmd.kind = fuse::editor::CommandKind::SetProperty;
    projectCmd.propertyName = "project";
    projectCmd.propertyValue = projectName;
    host.postFromUi(std::move(projectCmd));
}

fuse::u32 findEntityIndexByName(const fuse::scene::Scene& scene, const char* name) {
    const auto& entities = scene.entities();
    for (fuse::u32 i = 0; i < static_cast<fuse::u32>(entities.size()); ++i) {
        if (entities[i].name == name) {
            return i;
        }
    }
    return ~0u;
}

void testOpenSelectTweakSaveReload() {
    const std::filesystem::path demoSource = findDemo3dEmpty();
    expectTrue(!demoSource.empty(), "Samples/unification/demo_3d_empty located");
    if (demoSource.empty()) {
        return;
    }

    const std::filesystem::path projectDir = makeTempProjectDir();
    expectTrue(copyDemoToTemp(demoSource, projectDir), "demo_3d_empty copied to temp project");
    if (!std::filesystem::exists(projectDir / "project.json")) {
        return;
    }

    const fuse::project::LoadResult seeded = fuse::project::loadFromDirectory(projectDir.string());
    expectTrue(seeded.status == fuse::project::LoadStatus::Ok, "copied demo_3d_empty project.json loads");
    if (seeded.status != fuse::project::LoadStatus::Ok) {
        return;
    }

    fuse::editor::EditorHost host;
    postProjectOpen(host, projectDir.string(), "demo_3d_empty");
    host.gameTick();

    expectTrue(host.loadedProject() == "demo_3d_empty", "project label applied on game thread");
    expectTrue(host.runtimeViewport().embedSession().worldLoaded,
               "open project loaded defaultWorld3D from demo_3d_empty");
    expectTrue(host.runtimeScene().entityCount() > 0u, "opened world has mission objects");
    if (host.runtimeScene().entityCount() == 0u) {
        return;
    }

    fuse::editor::HierarchyModel hierarchy;
    hierarchy.setScene(&host.runtimeScene());
    expectTrue(!hierarchy.flatNodes().empty(), "hierarchy lists runtimeScene entities");

    fuse::editor::HierarchyNode row;
    const bool listedFloor = hierarchy.findNamed("Floor", row);
    if (!listedFloor) {
        row = hierarchy.flatNodes().front();
    }
    expectTrue(listedFloor || row.name == host.runtimeScene().entities().front().name,
               "hierarchy row matches Floor or first mission object");
    expectTrue(row.handle.isValid(), "hierarchy row is selectable");
    expectTrue(row.handle.index() < host.runtimeScene().entityCount(),
               "hierarchy row index maps to runtimeScene entity");

    const fuse::u32 targetIndex = row.handle.index();
    const std::string originalName = host.runtimeScene().entities()[targetIndex].name;
    expectTrue(!originalName.empty(), "selected object has a name");
    expectTrue(row.name == originalName, "hierarchy name matches runtimeScene entity");

    fuse::editor::EditorCommand select;
    select.kind = fuse::editor::CommandKind::SelectEntity;
    select.target = row.handle;
    host.postFromUi(std::move(select));
    host.gameTick();

    expectTrue(host.editorState().primarySelection.index == targetIndex,
               "SelectEntity targets opened object");
    expectTrue(host.editorState().selectedEntities.size() == 1u, "selection contains opened object");

    fuse::editor::PropertyInspector inspector;
    inspector.syncRuntime(host.editorState(), host.runtimeScene());
    expectTrue(inspector.hasSelection(), "inspector bound to runtimeScene selection");
    expectTrue(!inspector.sections().empty(), "inspector exposes name section");

    std::string inspectorName;
    expectTrue(inspector.getName(host.runtimeScene(), inspectorName), "inspector reads selected name");
    expectTrue(inspectorName == originalName, "inspector name matches hierarchy row");

    const std::string tweakedName = originalName + "_p5";
    expectTrue(inspector.setName(tweakedName, host.runtimeScene(), host.commandQueue()),
               "inspector setName posts SetProperty through EditorHost queue");

    std::string inspectorNameAfterSet;
    expectTrue(inspector.getName(host.runtimeScene(), inspectorNameAfterSet),
               "inspector getName after setName");
    expectTrue(inspectorNameAfterSet == tweakedName, "inspector setName writes runtimeScene");

    host.gameTick();

    expectTrue(host.runtimeScene().entities()[targetIndex].name == tweakedName,
               "SetProperty name mutates runtimeScene");
    expectTrue(host.editorState().sceneModified, "scene marked modified after name tweak");

    const std::filesystem::path tempWorld = projectDir / "worlds" / "p5_slice.fuselevel";
    std::error_code worldEc;
    std::filesystem::create_directories(tempWorld.parent_path(), worldEc);
    expectTrue(fuse::scene::SceneSerialiser::save(host.runtimeScene(), tempWorld.string()).status ==
                   fuse::scene::SerialiseStatus::Ok,
               "serialiser writes temp world file");

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectDir.string());
    expectTrue(fuse::scene::saveForProject(host.runtimeScene(), project.manifest).status ==
                   fuse::scene::SerialiseStatus::Ok,
               "saveForProject writes tweaked .fuselevel");

    fuse::scene::Scene reloaded;
    expectTrue(fuse::scene::SceneSerialiser::load(tempWorld.string(), reloaded).status ==
                   fuse::scene::SerialiseStatus::Ok,
               "fresh Scene loads temp world file");
    const fuse::u32 reloadedIndex = findEntityIndexByName(reloaded, tweakedName.c_str());
    expectTrue(reloadedIndex != ~0u, "tweaked name survived serialiser reload");
    expectTrue(reloaded.entityCount() == host.runtimeScene().entityCount(), "reloaded entity count");

    fuse::scene::Scene reloadedProject;
    expectTrue(fuse::scene::loadForProject(reloadedProject, project).status == fuse::scene::SerialiseStatus::Ok,
               "fresh Scene loads saved project world");
    expectTrue(findEntityIndexByName(reloadedProject, tweakedName.c_str()) != ~0u,
               "tweaked name survived project_io reload");

    fuse::editor::EditorHost reopened;
    postProjectOpen(reopened, projectDir.string(), "demo_3d_empty");
    reopened.gameTick();

    expectTrue(reopened.runtimeViewport().embedSession().worldLoaded, "reload host opened saved world");
    expectTrue(reopened.runtimeScene().entityCount() == host.runtimeScene().entityCount(),
               "reload host entity count");
    expectTrue(findEntityIndexByName(reopened.runtimeScene(), tweakedName.c_str()) != ~0u,
               "tweaked name survived EditorHost reload");
}

} // namespace

int main() {
    fuse::core::initialize();
    testOpenSelectTweakSaveReload();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_p5_slice: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_p5_slice: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
