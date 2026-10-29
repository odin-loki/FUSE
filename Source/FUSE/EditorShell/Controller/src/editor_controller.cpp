#include <fuse/editor/editor_controller.hpp>
#include <fuse/project/loader.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_console_commands.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/viewport_scene_view.hpp>
#include <fuse/project/manifest.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fuse::editor {

namespace {

ecs::EntityID toId(EntityRef r) { return ecs::EntityID{r.index, r.generation}; }
EntityRef toRef(ecs::EntityID id) { return EntityRef{id.index, id.generation}; }
Handle<Object> toHandle(EntityRef r) { return r.valid() ? Handle<Object>(r.index, r.generation) : Handle<Object>::invalid(); }

u8 entityKind(const ecs::Registry& registry, ecs::EntityID id) {
    if (registry.has<ecs::SDFObject>(id)) return 2;
    if (registry.has<ecs::Mesh>(id)) return 1;
    if (registry.has<ecs::PointLight>(id) || registry.has<ecs::DirectionalLight>(id) || registry.has<ecs::SpotLight>(id)) return 3;
    if (registry.has<ecs::Camera>(id)) return 4;
    return 0;
}

std::string entityLabel(const ecs::Registry& registry, ecs::EntityID id) {
    const char* kind = "Entity";
    if (registry.has<ecs::SDFObject>(id)) kind = "SDF Object";
    else if (registry.has<ecs::Mesh>(id)) kind = "Mesh";
    else if (registry.has<ecs::PointLight>(id)) kind = "Point Light";
    else if (registry.has<ecs::DirectionalLight>(id)) kind = "Directional Light";
    else if (registry.has<ecs::SpotLight>(id)) kind = "Spot Light";
    else if (registry.has<ecs::Camera>(id)) kind = "Camera";
    return std::string(kind) + " #" + std::to_string(id.index);
}

bool containsCi(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize k = 0;
        while (k < needle.size() && lower(hay[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string absolutePath(const std::string& p) {
    std::error_code ec;
    const auto a = std::filesystem::absolute(p, ec);
    return ec ? p : a.lexically_normal().string();
}

} // namespace

EditorController::EditorController(EditorHost& host, ShellServices& services)
    : EditorController(host, services, Options{}) {}

EditorController::EditorController(EditorHost& host, ShellServices& services, Options options)
    : m_host(host), m_services(services), m_options(std::move(options)), m_sceneView(std::make_unique<ViewportSceneView>()) {
    registerEditorComponentTypes();
    registerEditorConsoleCommands(m_console, m_host);
    m_viewport.setDimensions(640, 360);
    loadSettings_();
    refresh();
}

EditorController::~EditorController() { stopGameLoop(); }

// ---- game thread ----------------------------------------------------------------------------------------------------

void EditorController::startGameLoop(f32 hz) {
    if (m_thread.joinable()) return;
    m_stop = false;
    const auto period = std::chrono::duration<double>(1.0 / std::max(hz, 1.f));
    m_thread = std::thread([this, period] {
        auto next = std::chrono::steady_clock::now();
        while (!m_stop.load()) {
            const auto t0 = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_host.gameTick();
            }
            m_lastTickMs.store(std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - t0).count());
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;
            std::this_thread::sleep_until(next);
        }
    });
}

void EditorController::stopGameLoop() {
    if (!m_thread.joinable()) return;
    m_stop = true;
    m_thread.join();
}

void EditorController::tick(u32 frames) {
    for (u32 i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_host.gameTick();
        m_lastTickMs.store(std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    drainConsole_();
}

void EditorController::drainConsole_() {
    usize n = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        n = m_host.drainConsoleOutput(m_console);
    }
    if (n > 0) ++m_consoleVersion;
}

// ---- view state -----------------------------------------------------------------------------------------------------

std::string EditorController::sceneDisplayName_() const {
    const std::string& path = m_host.currentScenePath();
    if (!path.empty()) return std::filesystem::path(path).stem().string();
    const std::string& name = m_host.runtimeScene().name();
    return name.empty() ? std::string("Untitled") : name;
}

bool EditorController::refresh() {
    drainConsole_();
    EditorViewState v;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncUndoJournal_();
        const EditorState& state = m_host.editorState();
        v.sceneName = sceneDisplayName_();
        v.scenePath = m_host.currentScenePath();
        v.dirty = m_host.isSceneDirty();
        v.hasProject = m_host.hasProject();
        if (v.hasProject) v.projectName = m_host.projectManifest().name;
        v.transport = !state.playing ? TransportState::Stopped : state.paused ? TransportState::Paused : TransportState::Playing;
        v.canUndo = m_host.undoStack().canUndo() || m_host.commandStack().canUndo();
        v.canRedo = m_host.undoStack().canRedo() || m_host.commandStack().canRedo();
        if (v.canUndo) v.undoLabel = !m_undoJournal.empty() && m_undoJournal.back() == UndoSource::Property ? "Undo property edit" : "Undo scene edit";
        if (v.canRedo) v.redoLabel = !m_redoJournal.empty() && m_redoJournal.back() == UndoSource::Property ? "Redo property edit" : "Redo scene edit";
        v.gameTicks = m_host.gameTickCount();
        for (const ecs::EntityID id : state.selectedEntities) v.selection.push_back(toRef(id));
        v.primary = toRef(state.primarySelection);

        // hierarchy: parent-before-child (depth-first), sorted by index within a level, flat while filtering
        ecs::Registry& registry = m_host.editorScene().registry();
        struct Row { ecs::EntityID id, parent; std::string label; };
        std::vector<Row> rows;
        registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) { rows.push_back({id, t.parent, entityLabel(registry, id)}); });
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id.index < b.id.index; });
        v.entityCount = u32(rows.size());
        const auto isSel = [&](ecs::EntityID id) { return std::find(state.selectedEntities.begin(), state.selectedEntities.end(), id) != state.selectedEntities.end(); };
        const std::string filter = trim(m_filter);
        if (!filter.empty()) {
            for (const Row& r : rows)
                if (containsCi(r.label, filter)) v.hierarchy.push_back({toRef(r.id), toRef(r.parent), r.label, 0, isSel(r.id)});
        } else {
            std::map<u32, std::vector<usize>> children;
            std::vector<usize> roots;
            std::map<u32, usize> byIndex;
            for (usize i = 0; i < rows.size(); ++i) byIndex[rows[i].id.index] = i;
            for (usize i = 0; i < rows.size(); ++i) {
                const auto p = rows[i].parent.valid() ? byIndex.find(rows[i].parent.index) : byIndex.end();
                if (p != byIndex.end() && p->second != i) children[rows[p->second].id.index].push_back(i);
                else roots.push_back(i);
            }
            std::vector<bool> seen(rows.size(), false);
            const auto walk = [&](auto&& self, usize i, u32 depth) -> void {
                if (seen[i]) return;
                seen[i] = true;
                v.hierarchy.push_back({toRef(rows[i].id), toRef(rows[i].parent), rows[i].label, depth, isSel(rows[i].id)});
                if (auto c = children.find(rows[i].id.index); c != children.end())
                    for (const usize k : c->second) self(self, k, depth + 1);
            };
            for (const usize r : roots) walk(walk, r, 0);
            for (usize i = 0; i < rows.size(); ++i) walk(walk, i, 0); // cycles: show what is left at the top
        }

        // inspector
        m_inspector.sync(state, m_host.editorScene());
        if (m_inspector.hasSelection()) {
            const ecs::EntityID target = m_inspector.target();
            for (const PropertyInspector::ComponentSection& section : m_inspector.sections()) {
                InspectorSectionView sv;
                sv.componentName = section.componentName;
                sv.displayName = section.componentName;
                if (const ComponentKindDesc* kind = findComponentKind(section.componentName)) {
                    sv.typed = true;
                    sv.removable = kind->removable;
                    if (kind->displayName && *kind->displayName) sv.displayName = kind->displayName;
                    for (const PropertyFieldDesc& f : kind->fields) {
                        InspectorFieldView fv;
                        fv.label = f.label;
                        fv.propertyName = f.propertyName;
                        fv.type = f.type;
                        fv.minValue = f.minValue;
                        fv.maxValue = f.maxValue;
                        fv.step = f.step;
                        for (u32 e = 0; e < f.enumCount; ++e) fv.enumNames.emplace_back(f.enumNames[e]);
                        if (!readComponentProperty(registry, target, f.propertyName, fv.value)) continue;
                        sv.fields.push_back(std::move(fv));
                    }
                } else {
                    for (const PropertyInspector::Field& f : section.fields) {
                        InspectorFieldView fv;
                        fv.label = f.name;
                        fv.value = f.value;
                        sv.fields.push_back(std::move(fv));
                    }
                }
                v.inspector.push_back(std::move(sv));
            }
            for (const ComponentKindDesc& kind : editorComponentKinds())
                if (kind.addable && kind.has != nullptr && !kind.has(registry, target)) v.addableComponents.emplace_back(kind.name);
        }
    }
    v.recentFiles = m_recent;
    v.title = v.sceneName + (v.dirty ? "*" : "");
    if (!v.projectName.empty()) v.title += " - " + v.projectName;
    if (v.transport != TransportState::Stopped) v.title += v.transport == TransportState::Paused ? " [Paused]" : " [Playing]";
    v.title += " - FUSE Editor";
    v.status = (v.transport == TransportState::Stopped ? std::string("Edit") : v.transport == TransportState::Paused ? std::string("Paused") : std::string("Playing")) +
               " | " + std::to_string(v.entityCount) + " entities | " + std::to_string(v.selection.size()) + " selected | tick " +
               std::to_string(v.gameTicks);

    // change key (everything but the tick counter / status, so an idle editor stays idle)
    std::string key = v.title + "|" + v.scenePath + "|" + std::to_string(int(v.transport)) + std::to_string(v.canUndo) + std::to_string(v.canRedo) +
                      v.undoLabel + v.redoLabel + "|" + std::to_string(v.primary.index) + ":" + std::to_string(v.primary.generation);
    for (const auto& r : v.hierarchy) key += "|h" + std::to_string(r.entity.index) + "." + std::to_string(r.depth) + (r.selected ? "s" : "") + r.label;
    for (const auto& s : v.inspector) {
        key += "|#" + s.componentName;
        for (const auto& f : s.fields) key += "," + f.label + "=" + f.value;
    }
    for (const auto& a : v.addableComponents) key += "|+" + a;
    for (const auto& r : v.recentFiles) key += "|r" + r;
    m_view = std::move(v);
    if (key == m_viewKey) return false;
    m_viewKey = std::move(key);
    ++m_version;
    return true;
}

// ---- unified undo (Q2: the Qt MainWindow journal, moved here) -------------------------------------------------------

void EditorController::syncUndoJournal_() {
    const auto reconcile = [this](UndoSource source, u32 now, u32& seen, bool redoEmpty) {
        if (now > seen) {
            m_undoJournal.insert(m_undoJournal.end(), now - seen, source);
        } else {
            for (u32 drop = seen - now; drop > 0u; --drop) {
                auto it = std::find(m_undoJournal.rbegin(), m_undoJournal.rend(), source);
                if (it == m_undoJournal.rend()) break;
                m_undoJournal.erase(std::next(it).base());
            }
        }
        if (redoEmpty) m_redoJournal.erase(std::remove(m_redoJournal.begin(), m_redoJournal.end(), source), m_redoJournal.end());
        seen = now;
    };
    reconcile(UndoSource::Scene, m_host.undoStack().undoCount(), m_seenSceneUndo, !m_host.undoStack().canRedo());
    reconcile(UndoSource::Property, m_host.commandStack().undoDepth(), m_seenPropertyUndo, !m_host.commandStack().canRedo());
}

void EditorController::undo() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncUndoJournal_();
        if (m_undoJournal.empty() && (m_host.undoStack().canUndo() || m_host.commandStack().canUndo()))
            m_undoJournal.push_back(m_host.undoStack().canUndo() ? UndoSource::Scene : UndoSource::Property);
        if (!m_undoJournal.empty()) {
            const UndoSource source = m_undoJournal.back();
            m_undoJournal.pop_back();
            m_redoJournal.push_back(source);
            if (source == UndoSource::Scene) {
                m_host.undoStack().undo();
                m_seenSceneUndo = m_host.undoStack().undoCount();
            } else {
                m_host.undoPropertyEdit();
                m_seenPropertyUndo = m_host.commandStack().undoDepth();
            }
        }
    }
    refresh();
}

void EditorController::redo() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncUndoJournal_();
        if (!m_redoJournal.empty()) {
            const UndoSource source = m_redoJournal.back();
            m_redoJournal.pop_back();
            m_undoJournal.push_back(source);
            if (source == UndoSource::Scene) {
                m_host.undoStack().redo();
                m_seenSceneUndo = m_host.undoStack().undoCount();
            } else {
                m_host.redoPropertyEdit();
                m_seenPropertyUndo = m_host.commandStack().undoDepth();
            }
        }
    }
    refresh();
}

// ---- files (Q3 flows) -----------------------------------------------------------------------------------------------

bool EditorController::maybeSave_() {
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        dirty = m_host.isSceneDirty();
    }
    if (!dirty) return true;
    const PromptAnswer a = m_services.askSaveDiscardCancel("The scene has unsaved changes. Save them?");
    if (a == PromptAnswer::Save) return fileSave();
    return a == PromptAnswer::Discard;
}

bool EditorController::fileNew() {
    if (!maybeSave_()) return false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_host.newScene(m_host.currentSceneDimension());
        syncUndoJournal_();
        m_lastError = ok ? std::string() : m_host.lastFileResult().error;
    }
    refresh();
    return ok;
}

bool EditorController::fileOpenPath(const std::string& path) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_host.openScene(path);
        syncUndoJournal_();
        m_lastError = ok ? std::string() : m_host.lastFileResult().error;
    }
    if (ok) addRecent_(path);
    else m_services.warn("Open Scene", m_lastError);
    refresh();
    return ok;
}

bool EditorController::fileOpen() {
    if (!maybeSave_()) return false;
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_host.currentScenePath().empty()) dir = std::filesystem::path(m_host.currentScenePath()).parent_path().string();
        else if (m_host.hasProject()) dir = m_host.projectManifest().projectRoot;
    }
    const auto path = m_services.pickOpenFile("Open Scene", "FUSE level (*.fuselevel)", dir);
    if (!path || path->empty()) return false;
    return fileOpenPath(*path);
}

bool EditorController::fileOpenRecent(u32 index) {
    if (index >= m_recent.size() || !maybeSave_()) return false;
    const std::string path = m_recent[index];
    return fileOpenPath(path);
}

bool EditorController::fileSave() {
    bool hasPath = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        hasPath = !m_host.currentScenePath().empty();
    }
    if (!hasPath) return fileSaveAs();
    bool ok = false;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_host.saveScene();
        path = m_host.currentScenePath();
        m_lastError = ok ? std::string() : m_host.lastFileResult().error;
    }
    if (ok) addRecent_(path);
    else m_services.warn("Save Scene", m_lastError);
    refresh();
    return ok;
}

bool EditorController::fileSaveAs() {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_host.currentScenePath().empty()) dir = std::filesystem::path(m_host.currentScenePath()).parent_path().string();
        else if (m_host.hasProject()) dir = m_host.projectManifest().projectRoot;
    }
    auto path = m_services.pickSaveFile("Save Scene As", "FUSE level (*.fuselevel)", dir);
    if (!path || path->empty()) return false;
    if (std::filesystem::path(*path).extension().empty()) *path += ".fuselevel";
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_host.saveSceneAs(*path);
        m_lastError = ok ? std::string() : m_host.lastFileResult().error;
    }
    if (ok) addRecent_(*path);
    else m_services.warn("Save Scene As", m_lastError);
    refresh();
    return ok;
}

bool EditorController::requestClose() { return maybeSave_(); }

bool EditorController::newProject(const std::string& directory, const std::string& name, u32 dimensionFlags) {
    return newProject(directory, name, dimensionFlags, project::ModuleSettings{});
}

bool EditorController::newProject(const std::string& directory, const std::string& name, u32 dimensionFlags,
                                  const project::ModuleSettings& modules) {
    if (!maybeSave_()) return false;
    bool ok = false;
    std::string scene;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_host.newProject(directory, name, dimensionFlags);
        if (ok) {
            project::ProjectManifest manifest = m_host.projectManifest();
            const project::ModuleSettings& old = manifest.modules;
            if (old.ai != modules.ai || old.cinematics != modules.cinematics || old.fx != modules.fx ||
                old.mechanics != modules.mechanics || old.adventure != modules.adventure) {
                // same as the Qt shell: write the toggles through the manifest writer, reopen so the host matches
                manifest.modules = modules;
                const std::string root = manifest.projectRoot;
                ok = project::saveToDirectory(manifest, root).ok && m_host.openProject(root);
            }
        }
        syncUndoJournal_();
        m_lastError = ok ? std::string() : m_host.lastFileResult().error;
        scene = m_host.currentScenePath();
    }
    if (ok && !scene.empty()) addRecent_(scene);
    if (ok) m_viewport.setProjectLabel(name);
    refresh();
    return ok;
}

bool EditorController::openProject(const std::string& directory) {
    if (!maybeSave_()) return false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_host.openProject(directory);
        syncUndoJournal_();
        m_lastError = ok ? std::string() : m_host.lastFileResult().error;
    }
    if (ok) m_viewport.setProjectLabel(std::filesystem::path(directory).filename().string());
    else m_services.warn("Open Project", m_lastError);
    refresh();
    return ok;
}

void EditorController::addRecent_(const std::string& path) {
    if (path.empty()) return;
    const std::string abs = absolutePath(path);
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), abs), m_recent.end());
    m_recent.insert(m_recent.begin(), abs);
    if (m_recent.size() > m_options.maxRecentFiles) m_recent.resize(m_options.maxRecentFiles);
    saveSettings_();
}

// Settings file (R6: FUSE formats, not QSettings): {"recentScenes": ["...", ...]}
void EditorController::loadSettings_() {
    if (m_options.settingsPath.empty()) return;
    std::ifstream in(m_options.settingsPath);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const usize key = text.find("\"recentScenes\"");
    if (key == std::string::npos) return;
    usize i = text.find('[', key);
    const usize end = text.find(']', key);
    while (i != std::string::npos && i < end) {
        const usize q0 = text.find('"', i + 1);
        if (q0 == std::string::npos || q0 > end) break;
        std::string s;
        usize q = q0 + 1;
        for (; q < text.size() && text[q] != '"'; ++q) {
            if (text[q] == '\\' && q + 1 < text.size()) ++q;
            s += text[q];
        }
        if (!s.empty() && m_recent.size() < m_options.maxRecentFiles) m_recent.push_back(s);
        i = q + 1;
    }
}

void EditorController::saveSettings_() const {
    if (m_options.settingsPath.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_options.settingsPath).parent_path(), ec);
    std::ofstream out(m_options.settingsPath, std::ios::trunc);
    out << "{\n  \"recentScenes\": [";
    for (usize i = 0; i < m_recent.size(); ++i) out << (i ? ", " : "") << '"' << jsonEscape(m_recent[i]) << '"';
    out << "]\n}\n";
}

// ---- selection / hierarchy / inspector ------------------------------------------------------------------------------

void EditorController::select(std::span<const EntityRef> entities, EntityRef primary) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        EditorState& state = m_host.editorState();
        state.selectedEntities.clear();
        for (const EntityRef& e : entities)
            if (e.valid()) state.selectedEntities.push_back(toId(e));
        const bool inSel = std::find(entities.begin(), entities.end(), primary) != entities.end();
        state.primarySelection = state.selectedEntities.empty() ? ecs::EntityID::null()
                                 : inSel && primary.valid()     ? toId(primary)
                                                                : state.selectedEntities.front();
    }
    refresh();
}

void EditorController::selectOnly(EntityRef entity) {
    if (entity.valid()) select(std::span<const EntityRef>(&entity, 1), entity);
    else select({}, {});
}

u32 EditorController::reparent(std::span<const EntityRef> entities, EntityRef newParent) {
    u32 posted = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const ecs::Registry& registry = m_host.editorScene().registry();
        const auto parentOf = [&](ecs::EntityID id) {
            const ecs::Transform* t = registry.alive(id) && registry.has<ecs::Transform>(id) ? registry.get<ecs::Transform>(id) : nullptr;
            return t != nullptr ? t->parent : ecs::EntityID::null();
        };
        for (const EntityRef& e : entities) {
            const ecs::EntityID id = toId(e);
            if (!e.valid() || e == newParent || parentOf(id) == toId(newParent)) continue;
            bool cycle = false; // newParent may not be a descendant of the moved entity
            for (ecs::EntityID p = toId(newParent); p.valid(); p = parentOf(p))
                if (p == id) { cycle = true; break; }
            if (cycle) continue;
            m_host.postFromUi(makeReparentCommand(toHandle(e), toHandle(newParent), true));
            ++posted;
        }
    }
    return posted;
}

void EditorController::setHierarchyFilter(std::string_view text) {
    m_filter.assign(text);
    refresh();
}

void EditorController::setProperty(EntityRef entity, std::string_view propertyName, std::string_view value) {
    if (!entity.valid()) return;
    std::string v(value);
    if (v.empty()) {
        if (const PropertyFieldDesc* f = findComponentProperty(propertyName))
            if (f->type == PropertyFieldType::String || f->type == PropertyFieldType::AssetPath) v = kPropertyEmptyString;
    }
    m_host.postFromUi(makeSetPropertyCommand(toHandle(entity), std::string(propertyName), std::move(v)));
}

void EditorController::addComponent(EntityRef entity, std::string_view componentName) {
    if (entity.valid()) m_host.postFromUi(makeAddComponentCommand(toHandle(entity), std::string(componentName)));
}

void EditorController::removeComponent(EntityRef entity, std::string_view componentName) {
    if (entity.valid()) m_host.postFromUi(makeRemoveComponentCommand(toHandle(entity), std::string(componentName)));
}

bool EditorController::runEntityAction(ContextMenuAction action) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        EditorState& state = m_host.editorState();
        const bool needsSelection = action == ContextMenuAction::Duplicate || action == ContextMenuAction::Delete;
        if (needsSelection && !state.primarySelection.valid()) return false;
        m_entityMenu.openAt(state.primarySelection, ecs::vec3{0.f, 0.f, 0.f}, state);
        changed = m_entityMenu.activate(action, m_host.editorScene(), state, m_host.undoStack());
        m_lastCreated = toRef(m_entityMenu.lastCreated());
        m_entityMenu.close();
    }
    if (changed) refresh();
    return changed;
}

void EditorController::openContextMenu(f32 x, f32 y) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entityMenu.openInViewport(m_host.editorScene(), *m_sceneView, m_viewport, x, y, m_host.editorState());
}

bool EditorController::activateContextMenu(ContextMenuAction action) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_entityMenu.isOpen()) return false;
        changed = m_entityMenu.activate(action, m_host.editorScene(), m_host.editorState(), m_host.undoStack());
        m_lastCreated = toRef(m_entityMenu.lastCreated());
        m_entityMenu.close();
    }
    if (changed) refresh();
    return changed;
}

// ---- console --------------------------------------------------------------------------------------------------------

void EditorController::submitConsole(std::string_view line) {
    const std::string text(line);
    m_console.executeCommand(text.c_str());
    ++m_consoleVersion;
}

// ---- viewport (Q5: camera rules in the API, shells map their events only) ------------------------------------------

void EditorController::viewportResize(u32 width, u32 height) {
    if (width > 0 && height > 0 && (width != m_viewport.width() || height != m_viewport.height())) m_viewport.setDimensions(width, height);
}

void EditorController::viewportKey(ViewportKey key, bool down) { m_keys[static_cast<usize>(key)] = down; }

void EditorController::viewportPointer(const ViewportPointerEvent& e) {
    using K = ViewportPointerEvent::Kind;
    using B = ViewportPointerEvent::Button;
    if (e.button == B::Right) {
        if (e.kind == K::Down) {
            m_rmbHeld = true;
            m_rmbDragged = false;
            m_downX = m_lastX = e.x;
            m_downY = m_lastY = e.y;
            return;
        }
        if (e.kind == K::Up && m_rmbHeld) {
            m_rmbHeld = false;
            if (!m_rmbDragged) openContextMenu(e.x, e.y);
            m_rmbDragged = false;
            return;
        }
    }
    if (e.kind == K::Move && m_rmbHeld) {
        if (std::fabs(e.x - m_downX) + std::fabs(e.y - m_downY) > 3.f) m_rmbDragged = true;
        if (m_rmbDragged) {
            m_input.mouseDeltaX += e.x - m_lastX;
            m_input.mouseDeltaY += e.y - m_lastY;
        }
        m_lastX = e.x;
        m_lastY = e.y;
        return;
    }
    if (e.button == B::Left && e.kind == K::Down && gizmoPress_(e.x, e.y)) return;
    if (m_gizmo.dragging()) {
        if (e.kind == K::Move) gizmoMove_(e.x, e.y);
        else if (e.kind == K::Up && e.button == B::Left) gizmoRelease_();
        return;
    }
    if (e.button == B::Left && e.kind == K::Up) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sceneView->pickAndSelect(m_host.editorScene(), m_viewport, e.x, e.y, m_host.editorState());
        }
        refresh();
    }
}

// ---- transform gizmo ----
void EditorController::setTransformTool(TransformTool tool) {
    if (m_gizmo.dragging()) gizmoRelease_();
    m_gizmo.settings().tool = tool;
    ++m_gizmoVersion;
}

bool EditorController::refreshGizmo_() {
    const ecs::EntityID entity = m_host.editorState().primarySelection;
    ecs::Registry& registry = m_host.editorScene().registry();
    if (entity == ecs::EntityID::null() || !registry.alive(entity) || registry.get<ecs::Transform>(entity) == nullptr) {
        m_gizmo.clearLayout();
        m_gizmoEntity = ecs::EntityID::null();
        return false;
    }
    m_gizmoEntity = entity;
    const ecs::Transform& transform = *registry.get<ecs::Transform>(entity);
    const ecs::Transform* parent = (transform.parent != ecs::EntityID::null() && registry.alive(transform.parent))
                                       ? registry.get<ecs::Transform>(transform.parent)
                                       : nullptr;
    if (parent) {
        ecs::TransformSystem::update(registry); // parent matrices may be a tick stale after an edit
        m_gizmo.setParentFrame(parent->local_to_world, parent->world_to_local);
    } else {
        m_gizmo.clearParentFrame();
    }
    return m_gizmo.layout(m_viewport, transform);
}

EditorController::GizmoView EditorController::viewportGizmo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    GizmoView v;
    v.tool = m_gizmo.settings().tool;
    if (v.tool == TransformTool::Select) return v;
    if (!m_gizmo.dragging() && !refreshGizmo_()) return v;
    v.visible = m_gizmo.visible();
    v.handles = m_gizmo.handles();
    v.dragAxis = m_gizmo.dragAxis();
    return v;
}

bool EditorController::gizmoPress_(f32 px, f32 py) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gizmo.settings().tool == TransformTool::Select || !refreshGizmo_()) return false;
    const int axis = m_gizmo.hitTest(px, py);
    if (axis < 0) return false;
    ecs::Registry& registry = m_host.editorScene().registry();
    const ecs::Transform* transform = registry.get<ecs::Transform>(m_gizmoEntity);
    if (!transform || !m_gizmo.beginDrag(axis, px, py, *transform)) return false;
    m_gizmoGroup.clear();
    const std::vector<ecs::EntityID>& selection = m_host.editorState().selectedEntities;
    auto selectedAncestor = [&](ecs::EntityID id) {
        for (int guard = 0; guard < 64; ++guard) {
            const ecs::Transform* t = registry.get<ecs::Transform>(id);
            if (!t || !t->parent.valid()) return false;
            id = t->parent;
            if (std::find(selection.begin(), selection.end(), id) != selection.end()) return true;
        }
        return false;
    };
    for (ecs::EntityID other : selection) {
        if (other == m_gizmoEntity || !registry.alive(other) || selectedAncestor(other)) continue;
        if (const ecs::Transform* ot = registry.get<ecs::Transform>(other))
            m_gizmoGroup.push_back({other, TransformCommand::capture(*ot), ot->parent == transform->parent});
    }
    static const char* kNames[] = {"Move", "Rotate", "Scale"};
    const int tool = int(m_gizmo.settings().tool) - 1;
    m_host.undoStack().beginMacro(std::string(kNames[std::clamp(tool, 0, 2)]) + " entity");
    m_gizmoMacroOpen = true;
    ++m_gizmoVersion;
    return true;
}

void EditorController::gizmoMove_(f32 px, f32 py) {
    const TransformCommand::State after = m_gizmo.dragTo(px, py);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const ecs::Transform* transform = registry.get<ecs::Transform>(m_gizmoEntity);
        if (!transform) return;
        const TransformCommand::State before = TransformCommand::capture(*transform);
        m_host.undoStack().execute(std::make_unique<TransformCommand>(registry, m_gizmoEntity, before, after));
        const TransformCommand::State& start = m_gizmo.dragStart();
        for (const GizmoGroupStart& g : m_gizmoGroup) {
            const ecs::Transform* ot = registry.get<ecs::Transform>(g.id);
            if (!ot) continue;
            const TransformCommand::State ob = TransformCommand::capture(*ot);
            const TransformCommand::State oa =
                TransformGizmo::applyGroupDelta(m_gizmo.settings().tool, start, after, g.state, g.sameFrame);
            m_host.undoStack().execute(std::make_unique<TransformCommand>(registry, g.id, ob, oa));
        }
    }
    ++m_gizmoVersion;
    refresh();
}

void EditorController::gizmoRelease_() {
    m_gizmo.endDrag();
    m_gizmoGroup.clear();
    if (m_gizmoMacroOpen) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_host.undoStack().endMacro();
        m_gizmoMacroOpen = false;
    }
    ++m_gizmoVersion;
    refresh();
}

void EditorController::viewportWheel(f32 steps) {
    ViewportCamera& cam = m_viewport.camera();
    if (viewportFlying()) {
        cam.moveSpeed = std::clamp(cam.moveSpeed * std::pow(1.2f, steps), 0.5f, 1000.f);
        return;
    }
    const ecs::vec3 fwd = m_viewport.forward();
    const f32 d = steps * cam.moveSpeed * 0.25f;
    cam.positionX += fwd.x * d;
    cam.positionY += fwd.y * d;
    cam.positionZ += fwd.z * d;
}

bool EditorController::viewportTick(f32 dt) {
    const bool anyKey = m_keys[0] || m_keys[1] || m_keys[2] || m_keys[3] || m_keys[4] || m_keys[5];
    const bool look = viewportFlying();
    if (!anyKey && !look && m_input.mouseDeltaX == 0.f && m_input.mouseDeltaY == 0.f) return false;
    ViewportInput in{};
    in.lookHeld = look;
    in.mouseDeltaX = m_input.mouseDeltaX;
    in.mouseDeltaY = m_input.mouseDeltaY;
    // WASDQE move only while flying (RMB held), like the Qt viewport (ViewportPanel::applyInput).
    in.moveForward = m_keys[0];
    in.moveLeft = m_keys[1];
    in.moveBack = m_keys[2];
    in.moveRight = m_keys[3];
    in.moveDown = m_keys[4];
    in.moveUp = m_keys[5];
    in.fast = m_keys[6];
    m_input = {};
    m_viewport.submitInput(in);
    m_viewport.tick(std::clamp(dt, 0.f, 0.1f));
    return true;
}

bool EditorController::projectPoint(f32 x, f32 y, f32 z, f32& px, f32& py) const {
    ecs::vec3 w{};
    w.x = x;
    w.y = y;
    w.z = z;
    return m_viewport.worldToScreen(w, px, py);
}

void EditorController::applyPreferences(const EditorPreferences& p) {
    m_prefs = p;
    setEditorPreviewSun(p.previewSun);
    m_viewport.camera().moveSpeed = f32(p.cameraSpeed);
    m_viewport.camera().lookSensitivity = f32(p.lookSensitivity);
    ++m_gizmoVersion;
}

void EditorController::setViewMode(EditorViewMode mode) {
    setEditorViewMode(mode);
    ++m_gizmoVersion;
}
void EditorController::setShowMeshBounds(bool on) {
    m_showMeshBounds = on;
    ++m_gizmoVersion;
}

std::vector<EditorController::ViewportSegment> EditorController::viewportSegments(MeshEdgesFn edges, void* user, usize budget) {
    std::vector<ViewportSegment> out;
    const bool wire = editorViewModeWire(editorViewMode());
    if (!wire && !m_showMeshBounds) return out;
    std::lock_guard<std::mutex> lock(m_mutex);
    ecs::Registry& registry = m_host.editorScene().registry();
    ecs::TransformSystem::update(registry);
    const EditorState& state = m_host.editorState();
    const auto selected = [&](ecs::EntityID id) {
        return std::find(state.selectedEntities.begin(), state.selectedEntities.end(), id) != state.selectedEntities.end();
    };
    const auto push = [&](const ecs::Transform& t, const f32 a[3], const f32 b[3], u8 role) {
        const auto& m = t.local_to_world.data;
        const auto xf = [&](const f32* q) {
            ecs::vec3 v{};
            v.x = m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12];
            v.y = m[1] * q[0] + m[5] * q[1] + m[9] * q[2] + m[13];
            v.z = m[2] * q[0] + m[6] * q[1] + m[10] * q[2] + m[14];
            return v;
        };
        ViewportSegment s;
        s.role = role;
        if (m_viewport.worldToScreen(xf(a), s.x0, s.y0) && m_viewport.worldToScreen(xf(b), s.x1, s.y1)) out.push_back(s);
    };
    if (wire && edges != nullptr) {
        // real triangle edges (MeshRegistry keeps the decoded edges), scaled for SDF proxies like the Qt overlay
        const auto meshWire = [&](ecs::EntityID id, const ecs::Transform& t, u32 engineId, const f32 scale[3]) {
            const std::vector<f32>* e = edges(user, engineId);
            if (e == nullptr) return;
            const u8 role = selected(id) ? 4 : 3;
            for (usize k = 0; k + 6u <= e->size() && budget > 0; k += 6u, --budget) {
                const f32 a[3] = {(*e)[k] * scale[0], (*e)[k + 1] * scale[1], (*e)[k + 2] * scale[2]};
                const f32 b[3] = {(*e)[k + 3] * scale[0], (*e)[k + 4] * scale[1], (*e)[k + 5] * scale[2]};
                push(t, a, b, role);
            }
        };
        static constexpr f32 kOne[3] = {1.f, 1.f, 1.f};
        registry.each<ecs::Transform, ecs::Mesh>([&](ecs::EntityID id, ecs::Transform& t, ecs::Mesh& mesh) {
            if (mesh.visible && mesh.vertex_buffer.isValid()) meshWire(id, t, mesh.vertex_buffer.index(), kOne);
        });
        // SDF objects render through proxy meshes (GpuSceneEcsExtractorDesc defaults: 0 = cube, 1 = sphere)
        registry.each<ecs::Transform, ecs::SDFObject>([&](ecs::EntityID id, ecs::Transform& t, ecs::SDFObject& sdf) {
            if (!sdf.visible || sdf.op == ecs::SDFCsgOp::Subtract) return;
            const bool box = sdf.type == ecs::SDFPrimitive::Box;
            const f32 r = sdf.params.x > 0.f ? sdf.params.x : 1.f;
            const f32 sc[3] = {box ? sdf.params.x : r, box ? sdf.params.y : r, box ? sdf.params.z : r};
            meshWire(id, t, box ? 0u : 1u, sc);
        });
    }
    if (m_showMeshBounds) {
        registry.each<ecs::Transform, ecs::Mesh>([&](ecs::EntityID id, ecs::Transform& t, ecs::Mesh& mesh) {
            const f32 lo[3] = {mesh.aabb_min.x, mesh.aabb_min.y, mesh.aabb_min.z};
            const f32 hi[3] = {mesh.aabb_max.x, mesh.aabb_max.y, mesh.aabb_max.z};
            if (!(hi[0] > lo[0] || hi[1] > lo[1] || hi[2] > lo[2])) return;
            const u8 role = selected(id) ? 1 : 0;
            for (int a = 0; a < 3; ++a) {
                const int u = (a + 1) % 3, v = (a + 2) % 3;
                for (int s = 0; s < 4; ++s) {
                    f32 p0[3], p1[3];
                    p0[a] = lo[a];
                    p1[a] = hi[a];
                    p0[u] = p1[u] = (s & 1) ? hi[u] : lo[u];
                    p0[v] = p1[v] = (s & 2) ? hi[v] : lo[v];
                    push(t, p0, p1, role);
                }
            }
        });
    }
    return out;
}

std::vector<EditorController::ViewportMarker> EditorController::viewportMarkers() {
    std::vector<ViewportMarker> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    ecs::Registry& registry = m_host.editorScene().registry();
    const EditorState& state = m_host.editorState();
    const ecs::vec3 cam = m_viewport.position();
    registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
        // world position: walk the parents (positions only; enough for the marker overlay)
        ecs::vec3 p = t.position;
        for (ecs::EntityID parent = t.parent; parent.valid() && registry.alive(parent) && registry.has<ecs::Transform>(parent);) {
            const ecs::Transform* pt = registry.get<ecs::Transform>(parent);
            p.x += pt->position.x;
            p.y += pt->position.y;
            p.z += pt->position.z;
            parent = pt->parent;
        }
        ViewportMarker m;
        if (!m_viewport.worldToScreen(p, m.x, m.y)) return;
        m.entity = toRef(id);
        const f32 dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
        m.depth = std::sqrt(dx * dx + dy * dy + dz * dz);
        m.kind = entityKind(registry, id);
        m.selected = std::find(state.selectedEntities.begin(), state.selectedEntities.end(), id) != state.selectedEntities.end();
        out.push_back(m);
    });
    std::sort(out.begin(), out.end(), [](const ViewportMarker& a, const ViewportMarker& b) { return a.depth > b.depth; });
    return out;
}

// ---- actions --------------------------------------------------------------------------------------------------------

bool EditorController::actionEnabled(ActionId id) const {
    const EditorViewState& v = m_view;
    const bool playing = v.transport != TransportState::Stopped;
    switch (id) {
    case ActionId::FileSave:
    case ActionId::FileSaveAs: return !playing;
    case ActionId::EditUndo: return v.canUndo;
    case ActionId::EditRedo: return v.canRedo;
    case ActionId::EntityCreateEmpty:
    case ActionId::EntityCreateCube:
    case ActionId::EntityCreateSdfSphere:
    case ActionId::EntityCreateSdfBox:
    case ActionId::EntityCreatePointLight:
    case ActionId::EntityCreateDirectionalLight:
    case ActionId::EntityCreateCamera: return !playing;
    case ActionId::EntityDuplicate:
    case ActionId::EntityDelete: return !playing && v.primary.valid();
    case ActionId::PlayPlay: return !playing;
    case ActionId::PlayPause: return v.transport == TransportState::Playing;
    case ActionId::PlayResume:
    case ActionId::PlayStep: return v.transport == TransportState::Paused;
    case ActionId::PlayStop: return playing;
    default: return true;
    }
}

bool EditorController::trigger(ActionId id) {
    if (!actionEnabled(id)) return false;
    if (m_shellHook != nullptr && m_shellHook(m_shellUser, id)) return true;
    const auto transport = [this](CommandKind k) {
        m_host.postFromUi(makeTransportCommand(k));
        return true;
    };
    switch (id) {
    case ActionId::FileNewScene: return fileNew();
    case ActionId::FileOpenScene: return fileOpen();
    case ActionId::FileSave: return fileSave();
    case ActionId::FileSaveAs: return fileSaveAs();
    case ActionId::FileOpenProject: {
        if (!maybeSave_()) return false;
        const auto dir = m_services.pickDirectory("Open Project", {});
        return dir && !dir->empty() && openProject(*dir);
    }
    case ActionId::EditUndo: undo(); return true;
    case ActionId::EditRedo: redo(); return true;
    case ActionId::EntityCreateEmpty: return runEntityAction(ContextMenuAction::CreateEmpty);
    case ActionId::EntityCreateCube: return runEntityAction(ContextMenuAction::CreateCube);
    case ActionId::EntityCreateSdfSphere: return runEntityAction(ContextMenuAction::CreateSdfSphere);
    case ActionId::EntityCreateSdfBox: return runEntityAction(ContextMenuAction::CreateSdfBox);
    case ActionId::EntityCreatePointLight: return runEntityAction(ContextMenuAction::CreatePointLight);
    case ActionId::EntityCreateDirectionalLight: return runEntityAction(ContextMenuAction::CreateDirectionalLight);
    case ActionId::EntityCreateCamera: return runEntityAction(ContextMenuAction::CreateCamera);
    case ActionId::EntityDuplicate: return runEntityAction(ContextMenuAction::Duplicate);
    case ActionId::EntityDelete: return runEntityAction(ContextMenuAction::Delete);
    case ActionId::PlayPlay: return transport(CommandKind::StartPlay);
    case ActionId::PlayPause: return transport(CommandKind::PausePlay);
    case ActionId::PlayResume: return transport(CommandKind::ResumePlay);
    case ActionId::PlayStep: return transport(CommandKind::StepPlay);
    case ActionId::PlayStop: return transport(CommandKind::StopPlay);
    case ActionId::ViewModeLit: setViewMode(EditorViewMode::Lit); return true;
    case ActionId::ViewModeUnlit: setViewMode(EditorViewMode::Unlit); return true;
    case ActionId::ViewModeWireframe: setViewMode(EditorViewMode::Wireframe); return true;
    case ActionId::ViewModeLitWireframe: setViewMode(EditorViewMode::LitWireframe); return true;
    case ActionId::ViewShowMeshBounds: setShowMeshBounds(!m_showMeshBounds); return true;
    default: return false; // shell-owned (Quit, New Project wizard, Reset Layout) without a hook
    }
}

} // namespace fuse::editor
