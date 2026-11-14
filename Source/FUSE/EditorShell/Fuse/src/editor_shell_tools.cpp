// UI9: File > Project Settings / Build / Package dialogs and the hierarchy rename row (Qt ProjectSettingsDialog,
// BuildPackageDialog, HierarchyWidget::beginRename parity) on the toolkit-free project tools.
#include "fuse/editor/fui/editor_shell.hpp"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>

namespace fuse::editor::fui {

namespace w = fuse::ui::widgets;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;

namespace {
NodeHandle formRow(UiContext& ui, NodeHandle body, std::string_view label, f32 labelWidth = 130.f) {
    const NodeHandle r = w::panel(ui, body);
    ui.addClass(r, "row");
    ui.setCss(r, "gap", "6px");
    ui.setCss(r, "align-items", "center");
    const NodeHandle l = w::label(ui, r, label);
    ui.setCss(l, "min-width", std::to_string(int(labelWidth)) + "px");
    return r;
}
NodeHandle buttonRow(UiContext& ui, NodeHandle body) {
    const NodeHandle r = w::panel(ui, body);
    ui.addClass(r, "row");
    ui.setCss(r, "gap", "6px");
    ui.setCss(r, "justify-content", "flex-end");
    return r;
}
std::string trimmed(std::string_view s) {
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}
} // namespace

// ---- Project Settings -----------------------------------------------------------------------------------------------
void EditorShellWindow::openProjectSettings() {
    if (projectSettingsOpen()) return;
    const std::string root = m_c.projectRoot();
    if (root.empty()) {
        m_c.services().warn("Project Settings", "Open a project first.");
        return;
    }
    const ProjectSettingsLoad load = loadProjectSettings(root);
    if (!load.loaded) {
        m_c.services().warn("Project Settings", "Cannot read project.json in " + root);
        return;
    }
    m_projBase = load.manifest;
    const project::ProjectManifest& m = m_projBase;
    m_projSettings = w::dialog(m_ui, "Project Settings", "projset");
    const NodeHandle body = w::dialogBody(m_ui, m_projSettings);
    m_ui.setCss(body, "gap", "4px");
    NodeHandle r = formRow(m_ui, body, "Name");
    m_ui.setCss(w::lineEdit(m_ui, r, m.name, "projset.name"), "min-width", "260px");
    r = formRow(m_ui, body, "Default 3D world");
    m_ui.setCss(w::lineEdit(m_ui, r, m.defaultWorld3D, "projset.world3d"), "min-width", "260px");
    r = formRow(m_ui, body, "Default 2D world");
    m_ui.setCss(w::lineEdit(m_ui, r, m.defaultWorld2D, "projset.world2d"), "min-width", "260px");
    r = formRow(m_ui, body, "Worker threads (0 = Auto)");
    w::spinBox(m_ui, r, 0.f, 256.f, f32(m.workerCap), 1.f, "projset.workers");
    r = formRow(m_ui, body, "Dimensions");
    w::checkBox(m_ui, r, "3D", m.dimensions.enable3D, "projset.enable3d");
    w::checkBox(m_ui, r, "2D", m.dimensions.enable2D, "projset.enable2d");
    w::checkBox(m_ui, r, "UI", m.dimensions.enableUI, "projset.enableui");
    r = formRow(m_ui, body, "Modules");
    w::checkBox(m_ui, r, "AI", m.modules.ai, "projset.mod_ai");
    w::checkBox(m_ui, r, "Cinematics", m.modules.cinematics, "projset.mod_cinematics");
    w::checkBox(m_ui, r, "FX", m.modules.fx, "projset.mod_fx");
    w::checkBox(m_ui, r, "Mechanics", m.modules.mechanics, "projset.mod_mechanics");
    w::checkBox(m_ui, r, "Adventure", m.modules.adventure, "projset.mod_adventure");
    const NodeHandle err = w::label(m_ui, body, "", "projset.error");
    m_ui.addClass(err, "ed-dim");
    const NodeHandle buttons = buttonRow(m_ui, body);
    const NodeHandle ok = w::button(m_ui, buttons, "OK", "projset.ok");
    const NodeHandle cancel = w::button(m_ui, buttons, "Cancel", "projset.cancel");
    m_ui.connect(cancel, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { closeProjectSettings_(); });
    m_ui.connect(ok, SignalKind::Clicked, [this, err](UiContext& u, NodeHandle, const SignalArgs&) {
        const std::string root = m_c.projectRoot();
        project::ProjectManifest next = m_projBase;
        next.name = trimmed(u.text(u.find("projset.name")));
        next.defaultWorld3D = trimmed(u.text(u.find("projset.world3d")));
        next.defaultWorld2D = trimmed(u.text(u.find("projset.world2d")));
        next.workerCap = u32(std::max(0.f, u.value(u.find("projset.workers"))));
        next.dimensions = {u.checked(u.find("projset.enable3d")), u.checked(u.find("projset.enable2d")), u.checked(u.find("projset.enableui"))};
        next.modules = {u.checked(u.find("projset.mod_ai")), u.checked(u.find("projset.mod_cinematics")), u.checked(u.find("projset.mod_fx")),
                        u.checked(u.find("projset.mod_mechanics")), u.checked(u.find("projset.mod_adventure"))};
        std::string error;
        if (!saveProjectSettings(root, next, &error)) {
            u.setText(err, error); // stays open (Qt shows a warning and keeps the dialog)
            return;
        }
        closeProjectSettings_();
        m_c.console().addLog(fuse::log::Level::Info, ("Project settings saved; reopening " + root).c_str());
        m_c.reopenProject();
        sync();
    });
    w::openDialog(m_ui, m_projSettings);
}

void EditorShellWindow::closeProjectSettings_() {
    if (!projectSettingsOpen()) return;
    const NodeHandle d = m_projSettings;
    m_projSettings = {};
    w::closeDialog(m_ui, d);
    m_ui.destroy(m_ui.parent(d)); // the popup scope (it carries the id) and the card
}

// ---- Build / Package ------------------------------------------------------------------------------------------------
std::string EditorShellWindow::packageKey_() const {
    // remembered per project (Qt QSettings group package/<hash>)
    std::error_code ec;
    const std::string abs = std::filesystem::absolute(m_packageRoot, ec).lexically_normal().string();
    return "package." + std::to_string(std::hash<std::string>{}(abs)) + ".";
}

void EditorShellWindow::openBuildPackage() {
    if (buildPackageOpen()) return;
    m_packageRoot = m_c.projectRoot();
    if (m_packageRoot.empty()) {
        m_c.services().warn("Build / Package", "Open a project first.");
        return;
    }
    if (!m_c.requestClose()) return; // unsaved changes first (Qt maybeSaveChanges)
    m_package = {};
    const fuse::app::Settings& st = m_win.app().settings();
    const std::string key = packageKey_();
    std::string defaultExcludes;
    for (const std::string& p : defaultPackageExcludes()) defaultExcludes += (defaultExcludes.empty() ? "" : "; ") + p;
    const std::string output = std::string(st.get(key + "output", (std::filesystem::path(m_packageRoot).parent_path() / "packages").string()));

    m_packageDialog = w::dialog(m_ui, "Build / Package", "package");
    const NodeHandle body = w::dialogBody(m_ui, m_packageDialog);
    m_ui.setCss(body, "gap", "4px");
    NodeHandle r = formRow(m_ui, body, "Output folder");
    const NodeHandle out = w::lineEdit(m_ui, r, output, "package.output");
    m_ui.setCss(out, "min-width", "320px");
    const NodeHandle browse = w::button(m_ui, r, "Browse...", "package.browse");
    m_ui.connect(browse, SignalKind::Clicked, [this, out](UiContext& u, NodeHandle, const SignalArgs&) {
        if (const auto d = m_c.services().pickDirectory("Output Folder", u.text(out)); d && !d->empty()) u.setText(out, *d);
    });
    r = formRow(m_ui, body, "Runtime player");
    m_ui.setCss(w::lineEdit(m_ui, r, "", "package.runtime"), "min-width", "320px");
    const NodeHandle rtBrowse = w::button(m_ui, r, "Browse...", "package.runtime_browse");
    m_ui.connect(rtBrowse, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        if (const auto f = m_c.services().pickOpenFile("Runtime Player", "Executables (*)", {}); f && !f->empty())
            u.setText(u.find("package.runtime"), *f);
    });
    r = formRow(m_ui, body, "");
    w::checkBox(m_ui, r, "Cook models / textures / audio with fuse_cook", st.flag(key + "cook", true), "package.cook");
    r = formRow(m_ui, body, "");
    w::checkBox(m_ui, r, "Clean output first", true, "package.clean");
    r = formRow(m_ui, body, "Configuration");
    const bool shipping = st.get(key + "configuration", "Development") == "Shipping";
    const NodeHandle cfg = w::comboBox(m_ui, r, "Development\nShipping", shipping ? 1 : 0, "package.config");
    w::setTooltip(m_ui, cfg, "Shipping also leaves out editor-only sidecars (prefab links, variant snapshots, backups)");
    r = formRow(m_ui, body, "Exclude");
    const NodeHandle ex = w::lineEdit(m_ui, r, st.get(key + "excludes", defaultExcludes), "package.excludes");
    m_ui.setCss(ex, "min-width", "320px");
    w::setTooltip(m_ui, ex, "Semicolon-separated wildcards matched against the project-relative path or file name");
    r = formRow(m_ui, body, "");
    w::checkBox(m_ui, r, "Also write a .tar.gz archive", st.flag(key + "archive", false), "package.archive");
    const NodeHandle log = w::listView(m_ui, body, "", -1, "package.log");
    m_ui.setCss(log, "min-height", "160px");
    m_ui.setCss(log, "min-width", "520px");
    const NodeHandle buttons = buttonRow(m_ui, body);
    const NodeHandle build = w::button(m_ui, buttons, "Build", "package.build");
    const NodeHandle open = w::button(m_ui, buttons, "Open Folder", "package.open_folder");
    m_ui.setDisabled(open, true);
    const NodeHandle close = w::button(m_ui, buttons, "Close", "package.close");
    m_ui.connect(build, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { runPackage(); });
    m_ui.connect(open, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        if (!m_package.packageDir.empty()) m_c.services().openExternal(m_package.packageDir);
    });
    m_ui.connect(close, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { closeBuildPackage_(); });
    w::openDialog(m_ui, m_packageDialog);
}

PackageOptions EditorShellWindow::packageOptions() const {
    PackageOptions o;
    if (!buildPackageOpen()) return o;
    o.projectRoot = m_packageRoot;
    o.outputDir = trimmed(m_ui.text(m_ui.find("package.output")));
    o.runtimePath = trimmed(m_ui.text(m_ui.find("package.runtime")));
    o.cookAssets = m_ui.checked(m_ui.find("package.cook"));
    o.clean = m_ui.checked(m_ui.find("package.clean"));
    o.configuration = std::string(w::comboText(m_ui, m_ui.find("package.config")));
    const std::string ex(m_ui.text(m_ui.find("package.excludes")));
    for (std::size_t b = 0; b <= ex.size();) {
        const std::size_t e = ex.find_first_of(";,", b);
        const std::string p = trimmed(std::string_view(ex).substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (!p.empty()) o.excludePatterns.push_back(p);
        if (e == std::string::npos) break;
        b = e + 1;
    }
    o.archive = m_ui.checked(m_ui.find("package.archive"));
    return o;
}

bool EditorShellWindow::runPackage() {
    if (!buildPackageOpen()) return false;
    const NodeHandle build = m_ui.find("package.build"), open = m_ui.find("package.open_folder"), log = m_ui.find("package.log");
    m_ui.setDisabled(build, true);
    m_ui.setDisabled(open, true);
    m_ui.setText(log, "");
    const PackageOptions o = packageOptions();
    {
        fuse::app::Settings& st = m_win.app().settings();
        const std::string key = packageKey_();
        st.set(key + "output", o.outputDir);
        st.set(key + "configuration", o.configuration);
        st.set(key + "excludes", std::string(m_ui.text(m_ui.find("package.excludes"))));
        st.set(key + "archive", o.archive ? 1.0 : 0.0);
        st.set(key + "cook", o.cookAssets ? 1.0 : 0.0);
    }
    std::string lines;
    const auto append = [&](const std::string& line) {
        lines += (lines.empty() ? "" : "\n") + line;
        m_ui.setText(log, lines);
        m_win.app().step(0.f); // live log (Qt processEvents)
    };
    m_package = packageProject(o, append);
    if (!m_package.ok && !m_package.error.empty()) append("ERROR: " + m_package.error);
    if (m_package.ok)
        append("Done: " + std::to_string(m_package.copiedFiles) + " file(s), " + std::to_string(m_package.cookedAssets) + " cooked, " +
               std::to_string(m_package.excludedFiles) + " excluded" + (m_package.archivePath.empty() ? "" : ", archive " + m_package.archivePath));
    std::error_code ec;
    m_ui.setDisabled(open, !(m_package.ok && std::filesystem::exists(m_package.packageDir, ec)));
    m_ui.setDisabled(build, false);
    return m_package.ok;
}

void EditorShellWindow::closeBuildPackage_() {
    if (!buildPackageOpen()) return;
    const NodeHandle d = m_packageDialog;
    m_packageDialog = {};
    w::closeDialog(m_ui, d);
    m_ui.destroy(m_ui.parent(d)); // the popup scope (it carries the id) and the card
}

// ---- hierarchy rename / activate ------------------------------------------------------------------------------------
void EditorShellWindow::beginRename(EntityRef entity) {
    if (!entity.valid() || !m_hierRenameRow.valid()) return;
    std::string label;
    for (const HierarchyRowView& h : m_c.viewState().hierarchy)
        if (h.entity == entity) label = h.label;
    m_renaming = entity;
    m_ui.setText(m_hierRename, label);
    m_ui.setCss(m_hierRenameRow, "display", "flex");
    m_ui.setFocus(m_hierRename);
}

bool EditorShellWindow::activateHierarchyRow(u32 line) {
    if (line >= m_hierRows.size()) return false;
    const bool ok = m_c.activateEntity(m_hierRows[line]);
    sync();
    return ok;
}

// ---- toolbar icons --------------------------------------------------------------------------------------------------
std::string EditorShellOptions::defaultIconPack() {
#ifdef FUSE_EDITOR_ICON_PACK
    std::error_code ec;
    if (std::filesystem::exists(FUSE_EDITOR_ICON_PACK, ec)) return FUSE_EDITOR_ICON_PACK;
#endif
    const std::filesystem::path beside = std::filesystem::path(executableDir()) / "fuse_ui_icons.fuseicons";
    std::error_code ec2;
    return std::filesystem::exists(beside, ec2) ? beside.string() : std::string();
}

std::string_view EditorShellWindow::toolIcon(ActionId id) {
    switch (id) {
    case ActionId::FileNewScene: return "file-plus";
    case ActionId::FileOpenScene: return "folder-open";
    case ActionId::FileSave: return "save";
    case ActionId::EditUndo: return "undo-2";
    case ActionId::EditRedo: return "redo-2";
    case ActionId::PlayPlay: return "play";
    case ActionId::PlayResume: return "play";
    case ActionId::PlayPause: return "pause";
    case ActionId::PlayStop: return "square";
    default: return {};
    }
}

bool EditorShellWindow::hasToolIcon(ActionId id) const {
    const std::string_view name = toolIcon(id);
    return !name.empty() && m_icons && m_icons->find(name) >= 0;
}

} // namespace fuse::editor::fui
