#pragma once

#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/feature_pane_bridge.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/profiler_panel.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>

#include <QColor>
#include <QPointer>
#include <QWidget>

#include <mutex>
#include <string>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace fuse::editor::qt {

class PropertyPaneWidget;

/// B6.5 Scene Hierarchy dock — a QTreeWidget over the `EditorScene` registry (parent links from
/// `ecs::Transform::parent`), selection mirrored into `EditorState`, right-click through
/// `EntityContextMenu::openAt`, search through a case-insensitive filter.
class HierarchyWidget final : public QWidget {
    Q_OBJECT

public:
    HierarchyWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent = nullptr);

    void refresh();
    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    [[nodiscard]] int entityRowCount() const;
    [[nodiscard]] static QString entityLabel(const ecs::Registry& registry, ecs::EntityID id);
    [[nodiscard]] EntityContextMenu& contextMenuModel() { return m_contextMenu; }

signals:
    void sceneEdited();
    void selectionChanged();

private:
    void onItemSelectionChanged();
    void onContextMenuRequested(const QPoint& pos);

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_tree = nullptr;
    EntityContextMenu m_contextMenu;
    bool m_syncing = false;
};

/// B6.6 Inspector dock — the WP-08 property pane (name / position / PIE) plus every ECS component
/// section `PropertyInspector` reports for the primary selection.
class InspectorWidget final : public QWidget {
    Q_OBJECT

public:
    InspectorWidget(FeaturePaneBridge& bridge, std::mutex& sceneMutex, QWidget* parent = nullptr);

    void refresh();
    [[nodiscard]] PropertyPaneWidget* propertyPane() const { return m_pane; }
    [[nodiscard]] QTreeWidget* sectionTree() const { return m_sections; }

private:
    FeaturePaneBridge& m_bridge;
    std::mutex& m_sceneMutex;
    PropertyInspector m_ecsInspector;
    PropertyPaneWidget* m_pane = nullptr;
    QTreeWidget* m_sections = nullptr;
    std::vector<std::string> m_lastSignature;
};

/// B6.11 Console dock — `ConsolePanel` log buffer + command line. FUSE log messages (any thread)
/// are queued by a logger sink and drained on the UI thread.
///
/// MP-B6-EDITOR-SCRIPT-PIE / UNI-U6-CON-1: bound to an `EditorHost`, the command line runs the
/// editor console commands (`registerEditorConsoleCommands`: open / save / new / play / stop /
/// cvar / stat ...) and sends any other line to the game thread's Lua REPL; the host's console
/// output queue is drained with the log. Lines are coloured by level, the level buttons and the
/// text box filter the view, repeated lines show "(xN)", and Up / Down in the command line recall
/// the command history.
class ConsoleWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleWidget(QWidget* parent = nullptr);
    explicit ConsoleWidget(EditorHost* host, QWidget* parent = nullptr);
    ~ConsoleWidget() override;

    ConsolePanel& panel() { return m_panel; }
    [[nodiscard]] QPlainTextEdit* logView() const { return m_log; }
    [[nodiscard]] QLineEdit* input() const { return m_input; }
    [[nodiscard]] QLineEdit* textFilter() const { return m_filter; }
    /// Checkable filter button for Trace / Debug / Info / Warn / Error (nullptr for other levels).
    [[nodiscard]] QToolButton* levelButton(fuse::log::Level level) const;
    /// Text colour used for `level` lines.
    [[nodiscard]] static QColor levelColor(fuse::log::Level level);

    /// Move queued log lines (logger sink + host console output) into the panel and re-render the
    /// filtered view if it changed.
    void drainLog();
    void rerender();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static void logSink(fuse::log::Level level, const char* message, void* userData);
    void onSubmit();
    void onFiltersChanged();
    void refreshLevelButtons();
    void appendLine(const ConsolePanel::LogLine& line);

    EditorHost* m_host = nullptr;
    ConsolePanel m_panel;
    QPlainTextEdit* m_log = nullptr;
    QLineEdit* m_input = nullptr;
    QLineEdit* m_filter = nullptr;
    QToolButton* m_levelButtons[5] = {};
    std::mutex m_queueMutex;
    std::vector<std::pair<fuse::log::Level, std::string>> m_queue;
    usize m_renderedLines = 0;
    u32 m_renderedTailRepeat = 0;
};

/// B6.9 Asset Browser dock — `AssetBrowser` entries of the open project.
class AssetBrowserWidget final : public QWidget {
    Q_OBJECT

public:
    explicit AssetBrowserWidget(QWidget* parent = nullptr);
    void setProjectRoot(const QString& root);
    AssetBrowser& model() { return m_model; }
    [[nodiscard]] QListWidget* list() const { return m_list; }

private:
    void rebuild();

    AssetBrowser m_model;
    QLineEdit* m_filter = nullptr;
    QListWidget* m_list = nullptr;
};

/// B6.10 Profiler dock — `ProfilerPanel` ring buffer, fed with the editor's own frame timings.
class ProfilerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProfilerWidget(QWidget* parent = nullptr);
    ProfilerPanel& panel() { return m_panel; }
    void pushFrame(const ProfilerPanel::FrameProfileData& data);
    void refresh();

private:
    ProfilerPanel m_panel;
    QLabel* m_summary = nullptr;
};

/// B6.7 Material Editor dock — `MaterialEditorPanel` selection + PBR scalars.
class MaterialEditorWidget final : public QWidget {
    Q_OBJECT

public:
    MaterialEditorWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent = nullptr);
    void refresh();
    MaterialEditorPanel& panel() { return m_panel; }

private:
    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    MaterialEditorPanel m_panel;
    QLabel* m_summary = nullptr;
    QDoubleSpinBox* m_roughness = nullptr;
    QDoubleSpinBox* m_metallic = nullptr;
};

/// B6.8 SDF Sculpt dock — `SdfSculptPanel` brush settings.
class SdfSculptWidget final : public QWidget {
    Q_OBJECT

public:
    SdfSculptWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent = nullptr);
    void refresh();
    SdfSculptPanel& panel() { return m_panel; }

private:
    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    SdfSculptPanel m_panel;
    QComboBox* m_op = nullptr;
    QDoubleSpinBox* m_radius = nullptr;
    QDoubleSpinBox* m_alpha = nullptr;
    QLabel* m_summary = nullptr;
};

} // namespace fuse::editor::qt
