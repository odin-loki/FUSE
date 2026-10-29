#include "preferences_dialog.hpp"

#include "viewport_placeholder_widget.hpp"

#include <fuse/editor/runtime_viewport.hpp>

#include <QAction>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace fuse::editor::qt {

namespace {
QSettings settings() { return QSettings(QStringLiteral("FUSE"), QStringLiteral("FUSE Editor")); }
constexpr const char* kDefaultProp = "fuseDefaultShortcut";

QString plainText(const QAction* a) {
    QString t = a->text();
    t.remove(QLatin1Char('&'));
    if (t.endsWith(QStringLiteral("..."))) t.chop(3);
    return t;
}
} // namespace

EditorPreferences EditorPreferences::load() {
    const QSettings s = settings();
    EditorPreferences p;
    p.previewSun = s.value(QStringLiteral("prefs/previewSun"), p.previewSun).toBool();
    p.cameraSpeed = s.value(QStringLiteral("prefs/cameraSpeed"), p.cameraSpeed).toDouble();
    p.lookSensitivity = s.value(QStringLiteral("prefs/lookSensitivity"), p.lookSensitivity).toDouble();
    p.restoreLayout = s.value(QStringLiteral("prefs/restoreLayout"), p.restoreLayout).toBool();
    return p;
}

void EditorPreferences::save() const {
    QSettings s = settings();
    s.setValue(QStringLiteral("prefs/previewSun"), previewSun);
    s.setValue(QStringLiteral("prefs/cameraSpeed"), cameraSpeed);
    s.setValue(QStringLiteral("prefs/lookSensitivity"), lookSensitivity);
    s.setValue(QStringLiteral("prefs/restoreLayout"), restoreLayout);
}

QHash<QString, QKeySequence> loadShortcutOverrides() {
    QSettings s = settings();
    QHash<QString, QKeySequence> out;
    s.beginGroup(QStringLiteral("shortcuts"));
    for (const QString& key : s.childKeys()) {
        out.insert(key, QKeySequence(s.value(key).toString(), QKeySequence::PortableText));
    }
    s.endGroup();
    return out;
}

QKeySequence defaultShortcut(const QAction* action) {
    const QVariant v = action->property(kDefaultProp);
    return v.isValid() ? v.value<QKeySequence>() : action->shortcut();
}

QList<QAction*> editableActions(QWidget* root) {
    QList<QAction*> out;
    for (QAction* a : root->findChildren<QAction*>()) {
        if (a->objectName().startsWith(QStringLiteral("fuseAction")) && !a->text().isEmpty() && !out.contains(a)) {
            out.push_back(a);
        }
    }
    std::sort(out.begin(), out.end(), [](const QAction* a, const QAction* b) {
        return plainText(a).compare(plainText(b), Qt::CaseInsensitive) < 0;
    });
    return out;
}

void applyShortcutOverrides(QWidget* root) {
    const QHash<QString, QKeySequence> overrides = loadShortcutOverrides();
    for (QAction* a : editableActions(root)) {
        if (!a->property(kDefaultProp).isValid()) a->setProperty(kDefaultProp, QVariant::fromValue(a->shortcut()));
        const auto it = overrides.find(a->objectName());
        a->setShortcut(it != overrides.end() ? it.value() : defaultShortcut(a));
    }
}

PreferencesDialog::PreferencesDialog(QWidget* root, ViewportPlaceholderWidget* viewport, QWidget* parent)
    : QDialog(parent), m_root(root), m_viewport(viewport) {
    setObjectName(QStringLiteral("fusePreferencesDialog"));
    setWindowTitle(tr("Preferences"));
    resize(640, 520);
    auto* tabs = new QTabWidget(this);

    // General.
    const EditorPreferences prefs = EditorPreferences::load();
    auto* general = new QWidget(tabs);
    auto* form = new QFormLayout(general);
    m_previewSun = new QCheckBox(tr("Light scenes without a directional light with a preview sun"), general);
    m_previewSun->setObjectName(QStringLiteral("fusePrefPreviewSun"));
    m_previewSun->setChecked(editorPreviewSun());
    form->addRow(tr("Viewport lighting"), m_previewSun);
    m_speed = new QDoubleSpinBox(general);
    m_speed->setObjectName(QStringLiteral("fusePrefCameraSpeed"));
    m_speed->setRange(0.5, 500.0);
    m_speed->setSuffix(tr(" m/s"));
    m_speed->setValue(viewport != nullptr ? viewport->panel().camera().moveSpeed : prefs.cameraSpeed);
    form->addRow(tr("Fly camera speed"), m_speed);
    m_look = new QDoubleSpinBox(general);
    m_look->setObjectName(QStringLiteral("fusePrefLookSensitivity"));
    m_look->setRange(0.02, 2.0);
    m_look->setSingleStep(0.02);
    m_look->setSuffix(tr(" °/px"));
    m_look->setValue(viewport != nullptr ? viewport->panel().camera().lookSensitivity : prefs.lookSensitivity);
    form->addRow(tr("Look sensitivity"), m_look);
    m_restoreLayout = new QCheckBox(tr("Restore window layout on startup"), general);
    m_restoreLayout->setChecked(prefs.restoreLayout);
    form->addRow(tr("Layout"), m_restoreLayout);
    tabs->addTab(general, tr("General"));

    // Keyboard shortcuts.
    auto* keys = new QWidget(tabs);
    auto* kl = new QVBoxLayout(keys);
    m_filter = new QLineEdit(keys);
    m_filter->setPlaceholderText(tr("Filter actions..."));
    m_filter->setClearButtonEnabled(true);
    kl->addWidget(m_filter);
    m_actions = editableActions(root);
    m_table = new QTableWidget(static_cast<int>(m_actions.size()), 3, keys);
    m_table->setObjectName(QStringLiteral("fuseShortcutTable"));
    m_table->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut"), tr("Default")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    for (int r = 0; r < m_actions.size(); ++r) {
        QAction* a = m_actions[r];
        auto* name = new QTableWidgetItem(plainText(a));
        name->setFlags(Qt::ItemIsEnabled);
        name->setData(Qt::UserRole, a->objectName());
        name->setToolTip(a->objectName());
        m_table->setItem(r, 0, name);
        auto* edit = new QKeySequenceEdit(a->shortcut(), m_table);
        edit->setClearButtonEnabled(true);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this]() { rebuildConflicts(); });
        m_table->setCellWidget(r, 1, edit);
        auto* def = new QTableWidgetItem(defaultShortcut(a).toString(QKeySequence::NativeText));
        def->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(r, 2, def);
    }
    kl->addWidget(m_table, 1);
    m_conflicts = new QLabel(keys);
    m_conflicts->setObjectName(QStringLiteral("fuseShortcutConflicts"));
    m_conflicts->setStyleSheet(QStringLiteral("color: #f0a040;"));
    m_conflicts->setWordWrap(true);
    kl->addWidget(m_conflicts);
    auto* reset = new QPushButton(tr("Reset All to Defaults"), keys);
    reset->setObjectName(QStringLiteral("fuseShortcutReset"));
    connect(reset, &QPushButton::clicked, this, [this]() {
        for (int r = 0; r < m_actions.size(); ++r) {
            static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))->setKeySequence(defaultShortcut(m_actions[r]));
        }
        rebuildConflicts();
    });
    kl->addWidget(reset, 0, Qt::AlignLeft);
    tabs->addTab(keys, tr("Keyboard Shortcuts"));
    connect(m_filter, &QLineEdit::textChanged, this, &PreferencesDialog::filterRows);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (apply()) accept();
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() { (void)apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);
    rebuildConflicts();
}

void PreferencesDialog::filterRows(const QString& text) {
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString name = m_table->item(r, 0)->text();
        const QString keys = static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))->keySequence().toString();
        m_table->setRowHidden(r, !text.isEmpty() && !name.contains(text, Qt::CaseInsensitive) &&
                                     !keys.contains(text, Qt::CaseInsensitive));
    }
}

bool PreferencesDialog::setShortcutFor(const QString& objectName, const QKeySequence& sequence) {
    for (int r = 0; r < m_actions.size(); ++r) {
        if (m_actions[r]->objectName() == objectName) {
            static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))->setKeySequence(sequence);
            rebuildConflicts();
            return true;
        }
    }
    return false;
}

namespace {
QString plainTextFor(const QList<QAction*>& actions, const QString& objectName) {
    for (QAction* a : actions) {
        if (a->objectName() == objectName) return plainText(a);
    }
    return QStringLiteral("\x01"); // never matches
}
} // namespace

QStringList PreferencesDialog::conflicts() const {
    QHash<QString, QStringList> byKey;
    for (int r = 0; r < m_actions.size(); ++r) {
        const QKeySequence k = static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))->keySequence();
        if (!k.isEmpty()) byKey[k.toString(QKeySequence::PortableText)].push_back(m_table->item(r, 0)->text());
        // Secondary (non-editable) shortcuts of the action clash too, e.g. Play's F5.
        const QList<QKeySequence> all = m_actions[r]->shortcuts();
        for (int i = 1; i < all.size(); ++i) {
            if (!all[i].isEmpty()) byKey[all[i].toString(QKeySequence::PortableText)].push_back(m_table->item(r, 0)->text());
        }
    }
    // Play / Resume share F5 by design (never enabled together).
    for (auto it = byKey.begin(); it != byKey.end(); ++it) {
        QStringList& v = it.value();
        v.removeDuplicates();
        if (v.size() == 2 && v.contains(plainTextFor(m_actions, QStringLiteral("fuseActionPlay"))) &&
            v.contains(plainTextFor(m_actions, QStringLiteral("fuseActionResume")))) {
            v.clear();
        }
    }
    QStringList out;
    for (auto it = byKey.cbegin(); it != byKey.cend(); ++it) {
        if (it.value().size() > 1) {
            out.push_back(QStringLiteral("%1: %2").arg(QKeySequence(it.key()).toString(QKeySequence::NativeText),
                                                        it.value().join(QStringLiteral(", "))));
        }
    }
    out.sort();
    return out;
}

void PreferencesDialog::rebuildConflicts() {
    const QStringList c = conflicts();
    m_conflicts->setText(c.isEmpty() ? QString() : tr("Conflicting shortcuts — %1").arg(c.join(QStringLiteral("; "))));
}

bool PreferencesDialog::apply() {
    if (!conflicts().isEmpty()) {
        rebuildConflicts();
        return false;
    }
    EditorPreferences p;
    p.previewSun = m_previewSun->isChecked();
    p.cameraSpeed = m_speed->value();
    p.lookSensitivity = m_look->value();
    p.restoreLayout = m_restoreLayout->isChecked();
    p.save();
    setEditorPreviewSun(p.previewSun);
    if (m_viewport != nullptr) {
        m_viewport->panel().camera().moveSpeed = static_cast<f32>(p.cameraSpeed);
        m_viewport->panel().camera().lookSensitivity = static_cast<f32>(p.lookSensitivity);
    }
    QSettings s = settings();
    s.beginGroup(QStringLiteral("shortcuts"));
    for (int r = 0; r < m_actions.size(); ++r) {
        QAction* a = m_actions[r];
        const QKeySequence k = static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))->keySequence();
        if (k == defaultShortcut(a)) {
            s.remove(a->objectName());
        } else {
            s.setValue(a->objectName(), k.toString(QKeySequence::PortableText));
        }
        if (!a->property(kDefaultProp).isValid()) a->setProperty(kDefaultProp, QVariant::fromValue(a->shortcut()));
        a->setShortcut(k);
    }
    s.endGroup();
    return true;
}

} // namespace fuse::editor::qt
