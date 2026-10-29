#pragma once

// Edit > Preferences: general editor options and the keyboard shortcut editor. Values persist in
// QSettings ("FUSE" / "FUSE Editor"): `prefs/*` and `shortcuts/<action objectName>`.

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QStringList>

class QAction;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QWidget;

namespace fuse::editor::qt {

class ViewportPlaceholderWidget;

/// Editor options (read at startup, written by the dialog).
struct EditorPreferences {
    bool previewSun = true;
    double cameraSpeed = 10.0;
    double lookSensitivity = 0.2;
    bool restoreLayout = true;

    static EditorPreferences load();
    void save() const;
};

/// Persisted shortcut overrides: objectName -> sequence (empty sequence = unbound).
QHash<QString, QKeySequence> loadShortcutOverrides();
/// Applies overrides to every named action under `root`, remembering each action's default first
/// (QAction property "fuseDefaultShortcut").
void applyShortcutOverrides(QWidget* root);
/// The default shortcut of an action (before any override).
QKeySequence defaultShortcut(const QAction* action);
/// Actions with an objectName starting "fuseAction" (deduplicated, sorted by menu text).
QList<QAction*> editableActions(QWidget* root);

class PreferencesDialog final : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(QWidget* root, ViewportPlaceholderWidget* viewport, QWidget* parent = nullptr);

    [[nodiscard]] QTableWidget* shortcutTable() const { return m_table; }
    [[nodiscard]] QLabel* conflictLabel() const { return m_conflicts; }
    [[nodiscard]] QCheckBox* previewSunBox() const { return m_previewSun; }
    [[nodiscard]] QDoubleSpinBox* cameraSpeedBox() const { return m_speed; }
    /// Sets the shortcut of the row of `objectName` (as if typed); false when no such row.
    bool setShortcutFor(const QString& objectName, const QKeySequence& sequence);
    /// "key: action, action" for every shortcut shared by more than one action.
    [[nodiscard]] QStringList conflicts() const;
    /// Applies (and persists) the edited values; refuses while there are conflicts.
    bool apply();

private:
    void rebuildConflicts();
    void filterRows(const QString& text);

    QWidget* m_root = nullptr;
    ViewportPlaceholderWidget* m_viewport = nullptr;
    QList<QAction*> m_actions;
    QTableWidget* m_table = nullptr;
    QLabel* m_conflicts = nullptr;
    QLineEdit* m_filter = nullptr;
    QCheckBox* m_previewSun = nullptr;
    QCheckBox* m_restoreLayout = nullptr;
    QDoubleSpinBox* m_speed = nullptr;
    QDoubleSpinBox* m_look = nullptr;
};

} // namespace fuse::editor::qt
