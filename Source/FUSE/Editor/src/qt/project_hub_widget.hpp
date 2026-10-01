#pragma once

#include <fuse/editor/command_queue.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/types.hpp>

#include <QString>
#include <QStringList>
#include <QWidget>
#include <QWizard>

#include <functional>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

namespace fuse::editor::qt {

/// What the New Project wizard asks the editor to create (UNI-U6-FILE-1 / MP-B6-QT-SCENE-FILES).
struct NewProjectRequest {
    QString directory; ///< project folder (created; must not hold a project.json yet)
    QString name;
    u32 dimensionFlags = kProjectEnable3D | kProjectEnable2D | kProjectEnableUI; ///< kProject* bits
    project::ModuleSettings modules;
};

/// New Project wizard: page 1 name + location, page 2 dimension (3D / 2D / hybrid), UI layer and
/// module toggles. `request()` is the result once accepted.
class NewProjectWizard final : public QWizard {
    Q_OBJECT

public:
    enum class Dimension { World3D, World2D, Hybrid };

    explicit NewProjectWizard(const QString& defaultLocation, QWidget* parent = nullptr);

    [[nodiscard]] NewProjectRequest request() const;

    // Page widgets (the tests fill them like a user would).
    [[nodiscard]] QLineEdit* nameEdit() const { return m_name; }
    [[nodiscard]] QLineEdit* locationEdit() const { return m_location; }
    [[nodiscard]] QRadioButton* dimensionButton(Dimension dimension) const;
    [[nodiscard]] QCheckBox* uiCheck() const { return m_ui; }
    /// Module toggle: "ai", "cinematics", "fx", "mechanics" or "adventure".
    [[nodiscard]] QCheckBox* moduleCheck(const QString& module) const;

private:
    QLineEdit* m_name = nullptr;
    QLineEdit* m_location = nullptr;
    QRadioButton* m_dim3D = nullptr;
    QRadioButton* m_dim2D = nullptr;
    QRadioButton* m_dimHybrid = nullptr;
    QCheckBox* m_ui = nullptr;
    QCheckBox* m_ai = nullptr;
    QCheckBox* m_cinematics = nullptr;
    QCheckBox* m_fx = nullptr;
    QCheckBox* m_mechanics = nullptr;
    QCheckBox* m_adventure = nullptr;
};

/// Desktop project hub — lists discovered `project.json` folders (no scene pointers) and starts the
/// New Project wizard.
class ProjectHubWidget final : public QWidget {
    Q_OBJECT

public:
    /// Runs instead of `QWizard::exec` (tests): fill the wizard's pages, return true to accept.
    using WizardHook = std::function<bool(NewProjectWizard& wizard)>;

    explicit ProjectHubWidget(QWidget* parent = nullptr);

    void setSamplesRoot(const QString& samplesRoot);
    QString selectedProjectPath() const;
    /// Lists `directory` (a project created or opened elsewhere) and selects it.
    void addProjectDirectory(const QString& directory);
    void setWizardHook(WizardHook hook) { m_wizardHook = std::move(hook); }
    [[nodiscard]] QPushButton* newProjectButton() const { return m_newButton; }

public slots:
    /// Shows the wizard; on accept emits `newProjectRequested`.
    void runNewProjectWizard();

signals:
    void projectOpenRequested(const QString& projectDirectory);
    void newProjectRequested(const fuse::editor::qt::NewProjectRequest& request);

private:
    void refreshProjectList();
    void onOpenClicked();

    QString m_samplesRoot;
    QStringList m_extraProjects;
    QListWidget* m_projectList = nullptr;
    QPushButton* m_openButton = nullptr;
    QPushButton* m_newButton = nullptr;
    WizardHook m_wizardHook;
};

QStringList discoverProjectDirectories(const QString& samplesRoot);

} // namespace fuse::editor::qt
