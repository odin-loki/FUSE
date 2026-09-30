#include <fuse/editor/editor_host.hpp>

#include <fuse/editor/console_panel.hpp>

#include "editor_scripting.hpp"

#include <fuse/config/cvar.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/project_io.hpp>

#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>

namespace fuse::editor {

namespace {

constexpr f32 kStepDt = 1.f / 60.f;
/// PIE script hot-reload poll period in game ticks (~0.25 s at 60 Hz; each poll stats the files).
constexpr u32 kHotReloadPollTicks = 15;

const char* dimensionLabel(scene::SceneDimension dimension) {
    return dimension == scene::SceneDimension::World2D ? "2D" : "3D";
}

std::string trimCopy(const std::string& text) {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Splits "word rest..." (rest trimmed).
void splitFirstWord(const std::string& line, std::string& word, std::string& rest) {
    const usize space = line.find_first_of(" \t");
    if (space == std::string::npos) {
        word = line;
        rest.clear();
        return;
    }
    word = line.substr(0, space);
    rest = trimCopy(line.substr(space + 1u));
}

std::string lowerCopy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool ensureParentDirectory(const std::string& filePath, std::string& error) {
    const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        error = "unable to create directory " + parent.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

} // namespace

EditorHost::EditorHost() = default;

EditorHost::~EditorHost() {
    if (m_initialized) {
        // PIE behaviours run on_destroy against the play registry: stop before it goes away.
        if (m_playSession.isActive()) {
            m_playSession.stop(m_editorScene, m_runtimeScene, m_state, m_physics);
        }
        m_scripting.reset();
        m_editorScene.destroy();
    }
}

EditorScene& EditorHost::editorScene() {
    ensureInitialized_();
    return m_editorScene;
}

const EditorScene& EditorHost::editorScene() const {
    return const_cast<EditorHost*>(this)->editorScene();
}

void EditorHost::ensureInitialized_() {
    if (m_initialized) {
        return;
    }

    m_editorScene.init();
    m_runtimeScene = scene::Scene("EditorHostScene");
    m_initialized = true;
}

void EditorHost::postFromUi(EditorCommand command) {
    m_queue.post(std::move(command));
}

// ---- project / scene files ------------------------------------------------------------------------

bool EditorHost::isSceneDirty() const {
    return m_undoStack.isDirty() || m_commandStack.isDirty() || m_state.sceneModified;
}

void EditorHost::markSceneClean_() {
    m_undoStack.set_baseline_state();
    m_commandStack.set_baseline_state();
    m_state.sceneModified = false;
}

void EditorHost::resetForSceneSwitch_() {
    // Stops PIE, drops undo / command history and selection, empties registry + runtime scene.
    resetSceneForProjectOpen();
}

bool EditorHost::fileFail_(CommandKind kind, const std::string& path, std::string error) {
    m_lastFileResult.ok = false;
    m_lastFileResult.command = kind;
    m_lastFileResult.path = path;
    m_lastFileResult.error = std::move(error);
    postConsoleOutput(fuse::log::Level::Error, m_lastFileResult.error);
    return false;
}

bool EditorHost::fileOk_(CommandKind kind, const std::string& path, const std::string& message) {
    m_lastFileResult.ok = true;
    m_lastFileResult.command = kind;
    m_lastFileResult.path = path;
    m_lastFileResult.error.clear();
    postConsoleOutput(fuse::log::Level::Info, message);
    return true;
}

bool EditorHost::newScene(scene::SceneDimension dimension, std::string name) {
    ensureInitialized_();
    resetForSceneSwitch_();
    m_runtimeScene = scene::Scene(name.empty() ? std::string("Untitled") : std::move(name));
    if (dimension == scene::SceneDimension::World2D) {
        // 2D worlds look down -Z at the XY plane.
        fuse::Camera& camera = m_runtimeScene.camera();
        camera.positionX = 0.f;
        camera.positionY = 0.f;
        camera.positionZ = 10.f;
        camera.yawDeg = 0.f;
        camera.pitchDeg = 0.f;
        camera.update();
    }
    m_sceneDimension = dimension;
    m_currentScenePath.clear();
    markSceneClean_();
    return fileOk_(CommandKind::NewScene, {},
                   std::string("new ") + dimensionLabel(dimension) + " scene '" + m_runtimeScene.name() + "'");
}

bool EditorHost::openScene(const std::string& path) {
    ensureInitialized_();
    if (path.empty()) {
        return fileFail_(CommandKind::OpenScene, path, "open scene: no path given");
    }

    // Parse into scratch objects first: a failed load leaves the open scene untouched.
    scene::Scene loadedScene;
    ecs::Registry loadedRegistry;
    scene::SceneFileInfo info;
    const scene::SerialiseResult loaded =
        scene::SceneSerialiser::loadWithRegistry(path, loadedScene, loadedRegistry, &info);
    if (loaded.status != scene::SerialiseStatus::Ok) {
        return fileFail_(CommandKind::OpenScene, path, "open scene " + path + " failed: " + loaded.error);
    }

    resetForSceneSwitch_();
    m_runtimeScene = std::move(loadedScene);
    m_editorScene.registry() = std::move(loadedRegistry);
    m_sceneDimension = info.dimension;
    m_currentScenePath = path;
    markSceneClean_();
    ++m_sceneLoadCount;
    return fileOk_(CommandKind::OpenScene, path,
                   "opened " + std::string(dimensionLabel(info.dimension)) + " scene " + path + " (v" +
                       std::to_string(info.version) + ", " + std::to_string(m_runtimeScene.entityCount()) +
                       " scene entities, " + std::to_string(info.ecsEntityCount) + " ECS entities" +
                       (info.hasEcsBlock ? ")" : ", rebuilt from transforms)"));
}

bool EditorHost::saveSceneAs(const std::string& path) {
    ensureInitialized_();
    if (path.empty()) {
        return fileFail_(CommandKind::SaveSceneAs, path, "save scene: no path given");
    }
    if (m_playSession.isActive()) {
        return fileFail_(CommandKind::SaveSceneAs, path, "save scene: stop Play first (the play registry is not the edit scene)");
    }
    std::string error;
    if (!ensureParentDirectory(path, error)) {
        return fileFail_(CommandKind::SaveSceneAs, path, "save scene: " + error);
    }
    const scene::SerialiseResult saved =
        scene::SceneSerialiser::saveWithRegistry(m_runtimeScene, m_editorScene.registry(), path, m_sceneDimension);
    if (saved.status != scene::SerialiseStatus::Ok) {
        return fileFail_(CommandKind::SaveSceneAs, path, "save scene " + path + " failed: " + saved.error);
    }
    m_currentScenePath = path;
    markSceneClean_();
    ++m_sceneSaveCount;
    return fileOk_(CommandKind::SaveSceneAs, path,
                   "saved scene " + path + " (" + std::to_string(m_editorScene.registry().count()) + " ECS entities)");
}

bool EditorHost::saveScene() {
    ensureInitialized_();
    if (m_currentScenePath.empty()) {
        return fileFail_(CommandKind::SaveScene, {}, "save scene: the scene has no file yet (use Save As)");
    }
    const bool ok = saveSceneAs(m_currentScenePath);
    m_lastFileResult.command = CommandKind::SaveScene;
    return ok;
}

bool EditorHost::openProject(const std::string& directory) {
    ensureInitialized_();
    if (directory.empty()) {
        return fileFail_(CommandKind::OpenProject, directory, "open project: no directory given");
    }
    const std::string lower = lowerCopy(directory);
    const bool isJson = lower.size() >= 5u && lower.compare(lower.size() - 5u, 5u, ".json") == 0;
    const project::LoadResult load = isJson ? project::loadFromFile(directory) : project::loadFromDirectory(directory);
    if (load.status != project::LoadStatus::Ok) {
        return fileFail_(CommandKind::OpenProject, directory, "open project " + directory + " failed: " + load.error);
    }

    m_projectManifest = load.manifest;
    m_hasProject = true;
    // Label only: RuntimeViewportHook::setProjectLabel would also rename every scene to the project.
    setLoadedProject(m_projectManifest.name);
    m_playSession.setScriptRoot(m_projectManifest.projectRoot);

    const bool use3D = m_projectManifest.dimensions.enable3D && !m_projectManifest.defaultWorld3D.empty();
    const bool use2D = !use3D && m_projectManifest.dimensions.enable2D && !m_projectManifest.defaultWorld2D.empty();
    bool sceneOk = true;
    std::string worldPath;
    if (use3D || use2D) {
        worldPath = use3D ? scene::resolveDefaultWorldPath(m_projectManifest)
                          : scene::resolveDefaultWorld2DPath(m_projectManifest);
        const scene::SceneDimension dimension =
            use3D ? scene::SceneDimension::World3D : scene::SceneDimension::World2D;
        if (fileExists(worldPath)) {
            sceneOk = openScene(worldPath);
        } else {
            // Default world not written yet: an empty scene that Save writes there.
            newScene(dimension, m_projectManifest.name);
            m_currentScenePath = worldPath;
        }
    } else {
        newScene(m_projectManifest.dimensions.enable3D || !m_projectManifest.dimensions.enable2D
                     ? scene::SceneDimension::World3D
                     : scene::SceneDimension::World2D,
                 m_projectManifest.name);
    }
    if (!sceneOk) {
        return fileFail_(CommandKind::OpenProject, directory,
                         "open project " + m_projectManifest.name + ": default world failed to load: " +
                             m_lastFileResult.error);
    }
    return fileOk_(CommandKind::OpenProject, m_projectManifest.projectRoot,
                   "opened project '" + m_projectManifest.name + "' (" + m_projectManifest.projectRoot + ")" +
                       (worldPath.empty() ? std::string() : ", world " + worldPath));
}

bool EditorHost::newProject(const std::string& directory, const std::string& name, u32 dimensionFlags) {
    ensureInitialized_();
    if (directory.empty()) {
        return fileFail_(CommandKind::NewProject, directory, "new project: no directory given");
    }
    const std::string jsonPath = (std::filesystem::path(directory) / "project.json").string();
    if (fileExists(jsonPath)) {
        return fileFail_(CommandKind::NewProject, directory, "new project: " + jsonPath + " already exists");
    }

    project::ProjectManifest manifest;
    manifest.schemaVersion = project::kProjectSchemaVersion;
    manifest.name = name.empty() ? std::filesystem::path(directory).filename().string() : name;
    if (manifest.name.empty()) {
        manifest.name = "Untitled";
    }
    manifest.dimensions.enable3D = (dimensionFlags & kProjectEnable3D) != 0u;
    manifest.dimensions.enable2D = (dimensionFlags & kProjectEnable2D) != 0u;
    manifest.dimensions.enableUI = (dimensionFlags & kProjectEnableUI) != 0u;
    if (!manifest.dimensions.enable3D && !manifest.dimensions.enable2D) {
        manifest.dimensions.enable3D = true;
    }
    if (manifest.dimensions.enable3D) {
        manifest.defaultWorld3D = "worlds/main3d.fuselevel";
    }
    if (manifest.dimensions.enable2D) {
        manifest.defaultWorld2D = "worlds/main2d.fuselevel";
    }

    const project::SaveResult written = project::saveToDirectory(manifest, directory);
    if (!written.ok) {
        return fileFail_(CommandKind::NewProject, directory, "new project: " + written.error);
    }

    // An empty world per enabled dimension, so the project opens as-is in the editor and runtime.
    manifest.projectRoot = directory;
    const auto writeEmptyWorld = [&](const std::string& worldPath, scene::SceneDimension dimension,
                                     std::string& error) {
        if (!ensureParentDirectory(worldPath, error)) {
            return false;
        }
        scene::Scene world(manifest.name);
        ecs::Registry registry;
        registry.init();
        const scene::SerialiseResult saved = scene::SceneSerialiser::saveWithRegistry(world, registry, worldPath, dimension);
        if (saved.status != scene::SerialiseStatus::Ok) {
            error = saved.error;
            return false;
        }
        return true;
    };
    std::string error;
    if (manifest.dimensions.enable3D &&
        !writeEmptyWorld(scene::resolveDefaultWorldPath(manifest), scene::SceneDimension::World3D, error)) {
        return fileFail_(CommandKind::NewProject, directory, "new project: 3D world: " + error);
    }
    if (manifest.dimensions.enable2D &&
        !writeEmptyWorld(scene::resolveDefaultWorld2DPath(manifest), scene::SceneDimension::World2D, error)) {
        return fileFail_(CommandKind::NewProject, directory, "new project: 2D world: " + error);
    }

    if (!openProject(directory)) {
        m_lastFileResult.command = CommandKind::NewProject;
        return false;
    }
    return fileOk_(CommandKind::NewProject, m_projectManifest.projectRoot,
                   "created project '" + manifest.name + "' at " + written.path);
}

// ---- command dispatch (game thread) ---------------------------------------------------------------

bool EditorHost::applyHostCommand_(const EditorCommand& command) {
    switch (command.kind) {
    case CommandKind::NewProject:
        newProject(command.propertyValue, command.propertyName,
                   command.flags != 0u ? command.flags : (kProjectEnable3D | kProjectEnable2D | kProjectEnableUI));
        return true;
    case CommandKind::OpenProject:
        openProject(command.propertyValue);
        return true;
    case CommandKind::NewScene:
        newScene((command.flags & kScene2D) != 0u ? scene::SceneDimension::World2D : scene::SceneDimension::World3D,
                 command.propertyName);
        return true;
    case CommandKind::OpenScene:
        openScene(command.propertyValue);
        return true;
    case CommandKind::SaveScene:
        saveScene();
        return true;
    case CommandKind::SaveSceneAs:
        saveSceneAs(command.propertyValue);
        return true;
    case CommandKind::StepPlay:
        if (!m_playSession.stepPaused(kStepDt, m_editorScene, m_physics)) {
            postConsoleOutput(fuse::log::Level::Warn, "step: Play is not paused");
        }
        return true;
    case CommandKind::ConsoleExec:
        executeConsoleLine(command.propertyValue);
        return true;
    default:
        return false;
    }
}

void EditorHost::pollHotReload_() {
    if (!m_playSession.scriptsLive() || (m_gameTickCount % kHotReloadPollTicks) != 0u) {
        return;
    }
    std::vector<std::string> reloaded;
    const u32 count = m_playSession.pollScriptHotReload(&reloaded);
    // A failed reload keeps the running version; surface each new script error once.
    if (!m_playSession.scriptLastError().empty() && m_lastReportedScriptError != m_playSession.scriptLastError()) {
        m_lastReportedScriptError = m_playSession.scriptLastError();
        postConsoleOutput(fuse::log::Level::Error, "script error: " + m_lastReportedScriptError);
    }
    if (count == 0u) {
        return;
    }
    m_scriptHotReloadCount += count;
    std::string names;
    for (const std::string& module : reloaded) {
        names += names.empty() ? module : ", " + module;
    }
    postConsoleOutput(fuse::log::Level::Info,
                      "script hot-reload: " + std::to_string(count) + " module(s) reloaded" +
                          (names.empty() ? std::string() : " (" + names + ")"));
}

// ---- console (game thread) ------------------------------------------------------------------------

void EditorHost::postConsoleOutput(fuse::log::Level level, std::string text) {
    std::lock_guard<std::mutex> lock(m_consoleMutex);
    if (m_consoleOutput.size() >= 4096u) {
        m_consoleOutput.erase(m_consoleOutput.begin()); // bounded: the UI stopped draining
    }
    m_consoleOutput.push_back({level, std::move(text)});
}

usize EditorHost::drainConsoleOutput(std::vector<EditorConsoleLine>& out) {
    std::lock_guard<std::mutex> lock(m_consoleMutex);
    const usize count = m_consoleOutput.size();
    if (count == 0u) {
        return 0;
    }
    for (EditorConsoleLine& line : m_consoleOutput) {
        out.push_back(std::move(line));
    }
    m_consoleOutput.clear();
    return count;
}

usize EditorHost::drainConsoleOutput(ConsolePanel& console) {
    std::vector<EditorConsoleLine> lines;
    const usize count = drainConsoleOutput(lines);
    for (const EditorConsoleLine& line : lines) {
        console.addLog(line.level, line.text.c_str());
    }
    return count;
}

bool EditorHost::executeConsoleLine(const std::string& rawLine) {
    ensureInitialized_();
    ++m_consoleLinesExecuted;
    const std::string line = trimCopy(rawLine);
    if (line.empty()) {
        return false;
    }

    std::string word;
    std::string rest;
    splitFirstWord(line, word, rest);
    const std::string command = lowerCopy(word);

    if (command == "cvar") {
        config::CVarRegistry& cvars = config::CVarRegistry::global();
        if (rest.empty()) {
            postConsoleOutput(fuse::log::Level::Error, "usage: cvar <name> [value] | cvar list [prefix]");
            return false;
        }
        std::string name;
        std::string value;
        splitFirstWord(rest, name, value);
        if (name == "list") {
            const std::vector<std::string> names = cvars.complete(value);
            for (const std::string& entryName : names) {
                if (const config::CVarEntry* entry = cvars.find(entryName)) {
                    postConsoleOutput(fuse::log::Level::Info, entryName + " = " + entry->value_text());
                }
            }
            postConsoleOutput(fuse::log::Level::Info, std::to_string(names.size()) + " cvar(s)");
            return true;
        }
        config::CVarEntry* entry = cvars.find(name);
        if (entry == nullptr) {
            postConsoleOutput(fuse::log::Level::Error, "unknown cvar: " + name);
            return false;
        }
        if (!value.empty()) {
            if (value.size() >= 2u && value.front() == '"' && value.back() == '"') {
                value = value.substr(1u, value.size() - 2u);
            }
            const config::CVarResult result = cvars.set(*entry, value, config::CVarSource::Runtime);
            if (result != config::CVarResult::Ok) {
                postConsoleOutput(fuse::log::Level::Error,
                                  "cvar " + name + ": " + config::to_string(result));
                return false;
            }
        }
        postConsoleOutput(fuse::log::Level::Info, name + " = " + entry->value_text());
        return true;
    }

    if (command == "stat") {
        const ecs::Registry& registry = m_editorScene.registry();
        postConsoleOutput(fuse::log::Level::Info,
                          "game ticks " + std::to_string(m_gameTickCount) + " | commands applied " +
                              std::to_string(m_queue.appliedCount()) + " | ECS entities " +
                              std::to_string(registry.count()) + " in " + std::to_string(registry.archetype_count()) +
                              " archetypes | scene entities " + std::to_string(m_runtimeScene.entityCount()));
        postConsoleOutput(fuse::log::Level::Info,
                          std::string("scene ") + (m_currentScenePath.empty() ? "(unsaved)" : m_currentScenePath) +
                              " | " + dimensionLabel(m_sceneDimension) + " | dirty " +
                              (isSceneDirty() ? "yes" : "no") + " | undo " + std::to_string(m_undoStack.undoCount()) +
                              " | saves " + std::to_string(m_sceneSaveCount) + " | loads " +
                              std::to_string(m_sceneLoadCount));
        const char* pie = m_playSession.isPaused() ? "paused" : (m_playSession.isPlaying() ? "playing" : "stopped");
        postConsoleOutput(fuse::log::Level::Info,
                          std::string("PIE ") + pie + " | steps " + std::to_string(m_playSession.sessionTickCount()) +
                              " | scripts " + std::to_string(m_playSession.scriptAttachedCount()) +
                              " attached | script reloads " + std::to_string(m_scriptHotReloadCount) +
                              " | contacts dispatched " + std::to_string(m_playSession.scriptContactDispatchCount()));
        return true;
    }

    std::string source = line;
    bool forceLua = false;
    if (command == "lua") {
        if (rest.empty()) {
            postConsoleOutput(fuse::log::Level::Error, "usage: lua <code>");
            return false;
        }
        source = rest;
        forceLua = true;
    }

    if (m_scripting == nullptr) {
        m_scripting = std::make_unique<EditorScripting>();
    }
    if (!m_scripting->ready()) {
        std::string error;
        if (!m_scripting->init(m_editorScene.registry(), error)) {
            postConsoleOutput(fuse::log::Level::Error, "console: " + error);
            return false;
        }
    }

    std::vector<EditorConsoleLine> output;
    const usize entitiesBefore = m_editorScene.registry().count();
    bool ok = false;
    if (!forceLua && m_scripting->isConsoleCommand(source)) {
        ok = m_scripting->runConsoleCommand(source, output);
    } else {
        script::ScriptVM* vm = m_playSession.scriptVm();
        if (vm == nullptr) {
            vm = m_scripting->vm();
        }
        ok = vm != nullptr && EditorScripting::runLua(*vm, source, output);
        if (vm == nullptr) {
            output.push_back({fuse::log::Level::Error, "console: no script VM"});
        }
    }
    if (!m_playSession.isActive() && m_editorScene.registry().count() != entitiesBefore) {
        m_state.sceneModified = true; // the REPL created / destroyed entities in the edit scene
    }
    for (EditorConsoleLine& out : output) {
        postConsoleOutput(out.level, std::move(out.text));
    }
    return ok;
}

} // namespace fuse::editor
