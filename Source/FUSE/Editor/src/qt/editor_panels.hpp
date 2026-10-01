#pragma once

#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/feature_pane_bridge.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/profiler_panel.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>

#include <QColor>
#include <QPointer>
#include <QTreeWidget>
#include <QWidget>

#include <mutex>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QToolButton;

namespace fuse::editor::qt {

class PropertyPaneWidget;

/// MP-B6-QT-INSPECTOR: hierarchy tree with internal-move drag-and-drop. A drop does not move the
/// Qt items itself: it reports the dragged rows and the new parent row (on an item = that item,
/// above / below an item = the item's parent, empty area = scene root), and the hierarchy posts
/// ReparentObject commands; the tree is rebuilt from the registry once they are applied.
class HierarchyTreeWidget final : public QTreeWidget {
    Q_OBJECT

public:
    explicit HierarchyTreeWidget(QWidget* parent = nullptr);

    /// New parent row for a drop at `pos` (viewport coordinates); null = scene root. The upper /
    /// lower quarter of a row means "next to it" (same parent), the middle "onto it".
    [[nodiscard]] QTreeWidgetItem* dropParentAt(const QPoint& pos) const;

signals:
    void rowsDropped(const QList<QTreeWidgetItem*>& dragged, QTreeWidgetItem* newParent);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    [[nodiscard]] bool acceptsDrag_(const QDropEvent* event) const;
};

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
    /// Tree row of `id` (null when not shown).
    [[nodiscard]] QTreeWidgetItem* itemFor(ecs::EntityID id) const;
    /// Posts one ReparentObject command (kReparentKeepWorldPose) per entity; entities whose
    /// ancestor is also moved stay under it, and the new parent itself is skipped. Returns the
    /// number of commands posted (applied on the next game tick, one undo step each).
    int reparentEntities(const std::vector<ecs::EntityID>& entities, ecs::EntityID newParent);

signals:
    void sceneEdited();
    void selectionChanged();

private:
    void onItemSelectionChanged();
    void onContextMenuRequested(const QPoint& pos);
    void onRowsDropped(const QList<QTreeWidgetItem*>& dragged, QTreeWidgetItem* newParent);

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    QLineEdit* m_search = nullptr;
    HierarchyTreeWidget* m_tree = nullptr;
    EntityContextMenu m_contextMenu;
    bool m_syncing = false;
};

/// B6.6 Inspector dock — the WP-08 property pane (name / position / PIE) plus one editable group
/// per ECS component section `PropertyInspector` reports for the primary selection.
///
/// MP-B6-QT-INSPECTOR / UNI-U6-INSP-1: the typed editors are generated from the component schema
/// (component_schema.hpp): float / vec3 / Euler-degree rotation / colour / enum / bool / integer /
/// asset-path fields. Every edit posts a SetProperty command through the host's CommandQueue (the
/// game tick applies it and records it on the CommandStack; a spin-box drag coalesces into one
/// undo step). "Add Component" and each section's "Remove" post AddComponent / RemoveComponent.
/// Components without a schema (module types) show read-only fields. The raw field tree of the
/// previous inspector stays available behind the "Raw" toggle.
class InspectorWidget final : public QWidget {
    Q_OBJECT

public:
    InspectorWidget(FeaturePaneBridge& bridge, std::mutex& sceneMutex, QWidget* parent = nullptr);

    void refresh();
    [[nodiscard]] PropertyPaneWidget* propertyPane() const { return m_pane; }
    [[nodiscard]] QTreeWidget* sectionTree() const { return m_sections; }

    /// Component names of the shown sections, in order.
    [[nodiscard]] QStringList componentSections() const;
    /// Editor widgets of a schema field (by its command property name, e.g. "RigidBody.mass").
    [[nodiscard]] QWidget* fieldEditor(const QString& propertyName) const;
    /// Spin box `component` (0..2) of a float / vec3 / colour / Euler / integer field.
    [[nodiscard]] QDoubleSpinBox* fieldSpin(const QString& propertyName, int component = 0) const;
    [[nodiscard]] QCheckBox* fieldCheck(const QString& propertyName) const;
    [[nodiscard]] QComboBox* fieldCombo(const QString& propertyName) const;
    [[nodiscard]] QLineEdit* fieldText(const QString& propertyName) const;
    [[nodiscard]] QMenu* addComponentMenu() const { return m_addMenu; }
    [[nodiscard]] QToolButton* removeButton(const QString& componentName) const;

    /// Post AddComponent / RemoveComponent for the inspected entity (the menu / buttons call these).
    bool addComponent(const QString& componentName);
    bool removeComponent(const QString& componentName);

signals:
    /// A field edit was posted (the viewport repaints; the value lands on the next game tick).
    void propertyEdited();
    /// An Add / Remove Component command was posted.
    void componentsChanged();

private:
    struct FieldBinding {
        const PropertyFieldDesc* desc = nullptr;
        QWidget* root = nullptr;
        QDoubleSpinBox* spins[3] = {};
        QCheckBox* check = nullptr;
        QComboBox* combo = nullptr;
        QLineEdit* text = nullptr;
    };
    struct SectionView {
        std::string componentName;
        const ComponentKindDesc* kind = nullptr;
        std::vector<PropertyInspector::Field> readOnlyFields;
    };

    void rebuildEditors(const std::vector<SectionView>& sections);
    QWidget* makeFieldEditor(const PropertyFieldDesc& desc, QWidget* parent);
    void updateEditorValues(const std::vector<std::pair<const PropertyFieldDesc*, std::string>>& values);
    void postField(const FieldBinding& binding);
    [[nodiscard]] const FieldBinding* binding(const QString& propertyName) const;
    void chooseAsset(const FieldBinding& binding);
    void chooseColor(const FieldBinding& binding);
    void clearEditors();

    FeaturePaneBridge& m_bridge;
    std::mutex& m_sceneMutex;
    PropertyInspector m_ecsInspector;
    PropertyPaneWidget* m_pane = nullptr;
    QToolButton* m_addButton = nullptr;
    QMenu* m_addMenu = nullptr;
    QToolButton* m_rawToggle = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_editorPage = nullptr;
    QVBoxLayout* m_editorLayout = nullptr;
    QTreeWidget* m_sections = nullptr;
    std::vector<FieldBinding> m_bindings;
    std::vector<std::pair<std::string, QToolButton*>> m_removeButtons;
    std::vector<std::string> m_shownSections;
    std::string m_structureKey;
    ecs::EntityID m_boundEntity = ecs::EntityID::null();
    bool m_syncing = false;
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
