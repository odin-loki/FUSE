#include "project_hub_widget.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWizardPage>

namespace fuse::editor::qt {

QStringList discoverProjectDirectories(const QString& samplesRoot) {
    QStringList projects;
    const QDir root(samplesRoot);
    if (!root.exists()) {
        return projects;
    }

    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        if (QFileInfo::exists(entry.absoluteFilePath() + "/project.json")) {
            projects.push_back(entry.absoluteFilePath());
        }
    }
    projects.sort(Qt::CaseInsensitive);
    return projects;
}

ProjectHubWidget::ProjectHubWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("Projects"), this);
    title->setStyleSheet("font-weight: bold;");
    layout->addWidget(title);

    m_projectList = new QListWidget(this);
    layout->addWidget(m_projectList, 1);

    auto* buttons = new QHBoxLayout();
    m_openButton = new QPushButton(tr("Open Project"), this);
    connect(m_openButton, &QPushButton::clicked, this, &ProjectHubWidget::onOpenClicked);
    buttons->addWidget(m_openButton);
    m_newButton = new QPushButton(tr("New Project..."), this);
    m_newButton->setObjectName(QStringLiteral("fuseNewProjectButton"));
    connect(m_newButton, &QPushButton::clicked, this, &ProjectHubWidget::runNewProjectWizard);
    buttons->addWidget(m_newButton);
    layout->addLayout(buttons);

    connect(m_projectList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        onOpenClicked();
    });
}

void ProjectHubWidget::setSamplesRoot(const QString& samplesRoot) {
    m_samplesRoot = samplesRoot;
    refreshProjectList();
}

QString ProjectHubWidget::selectedProjectPath() const {
    const QListWidgetItem* item = m_projectList->currentItem();
    if (item == nullptr) {
        return {};
    }
    return item->data(Qt::UserRole).toString();
}

void ProjectHubWidget::refreshProjectList() {
    m_projectList->clear();
    QStringList projects = discoverProjectDirectories(m_samplesRoot);
    for (const QString& extra : m_extraProjects) {
        if (!projects.contains(extra)) {
            projects.push_back(extra);
        }
    }
    for (const QString& path : projects) {
        const QFileInfo info(path);
        auto* item = new QListWidgetItem(info.fileName());
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        m_projectList->addItem(item);
    }
    if (m_projectList->count() > 0) {
        m_projectList->setCurrentRow(0);
    }
}

void ProjectHubWidget::addProjectDirectory(const QString& directory) {
    const QString absolute = QFileInfo(directory).absoluteFilePath();
    if (!m_extraProjects.contains(absolute)) {
        m_extraProjects.push_back(absolute);
    }
    refreshProjectList();
    for (int row = 0; row < m_projectList->count(); ++row) {
        if (m_projectList->item(row)->data(Qt::UserRole).toString() == absolute) {
            m_projectList->setCurrentRow(row);
        }
    }
}

void ProjectHubWidget::runNewProjectWizard() {
    const QString location = m_samplesRoot.isEmpty() ? QDir::homePath() : m_samplesRoot;
    NewProjectWizard wizard(location, this);
    const bool accepted = m_wizardHook != nullptr ? m_wizardHook(wizard) : wizard.exec() == QDialog::Accepted;
    if (accepted) {
        emit newProjectRequested(wizard.request());
    }
}

// ---- NewProjectWizard -----------------------------------------------------------------------------

NewProjectWizard::NewProjectWizard(const QString& defaultLocation, QWidget* parent) : QWizard(parent) {
    setObjectName(QStringLiteral("fuseNewProjectWizard"));
    setWindowTitle(tr("New FUSE Project"));

    auto* where = new QWizardPage(this);
    where->setTitle(tr("Project"));
    where->setSubTitle(tr("Name the project and choose the folder that will hold it."));
    auto* whereForm = new QFormLayout(where);
    m_name = new QLineEdit(where);
    m_name->setObjectName(QStringLiteral("fuseNewProjectName"));
    m_name->setText(tr("NewProject"));
    whereForm->addRow(tr("Name"), m_name);
    auto* locationRow = new QHBoxLayout();
    m_location = new QLineEdit(where);
    m_location->setObjectName(QStringLiteral("fuseNewProjectLocation"));
    m_location->setText(defaultLocation);
    locationRow->addWidget(m_location, 1);
    auto* browse = new QToolButton(where);
    browse->setText(QStringLiteral("\u2026"));
    connect(browse, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Project Location"), m_location->text());
        if (!dir.isEmpty()) {
            m_location->setText(dir);
        }
    });
    locationRow->addWidget(browse);
    whereForm->addRow(tr("Location"), locationRow);
    auto* pathHint = new QLabel(where);
    pathHint->setWordWrap(true);
    const auto updateHint = [this, pathHint]() {
        pathHint->setText(tr("Creates %1").arg(QDir::toNativeSeparators(request().directory)));
    };
    connect(m_name, &QLineEdit::textChanged, this, updateHint);
    connect(m_location, &QLineEdit::textChanged, this, updateHint);
    whereForm->addRow(QString(), pathHint);
    addPage(where);

    auto* features = new QWizardPage(this);
    features->setTitle(tr("Features"));
    features->setSubTitle(tr("Choose the world dimensions and the gameplay modules the project uses."));
    auto* featureLayout = new QVBoxLayout(features);
    auto* dimensionBox = new QGroupBox(tr("Dimension"), features);
    auto* dimensionLayout = new QVBoxLayout(dimensionBox);
    m_dim3D = new QRadioButton(tr("3D"), dimensionBox);
    m_dim2D = new QRadioButton(tr("2D"), dimensionBox);
    m_dimHybrid = new QRadioButton(tr("Hybrid (3D + 2D)"), dimensionBox);
    m_dimHybrid->setChecked(true);
    dimensionLayout->addWidget(m_dim3D);
    dimensionLayout->addWidget(m_dim2D);
    dimensionLayout->addWidget(m_dimHybrid);
    m_ui = new QCheckBox(tr("UI layer"), dimensionBox);
    m_ui->setChecked(true);
    dimensionLayout->addWidget(m_ui);
    featureLayout->addWidget(dimensionBox);
    auto* moduleBox = new QGroupBox(tr("Modules"), features);
    auto* moduleLayout = new QVBoxLayout(moduleBox);
    const auto addModule = [moduleBox, moduleLayout](const QString& text, const char* key) {
        auto* check = new QCheckBox(text, moduleBox);
        check->setObjectName(QStringLiteral("fuseNewProjectModule.%1").arg(QLatin1String(key)));
        moduleLayout->addWidget(check);
        return check;
    };
    m_ai = addModule(tr("AI (behaviour trees)"), "ai");
    m_cinematics = addModule(tr("Cinematics (sequencer)"), "cinematics");
    m_fx = addModule(tr("FX"), "fx");
    m_mechanics = addModule(tr("Mechanics"), "mechanics");
    m_adventure = addModule(tr("Adventure"), "adventure");
    featureLayout->addWidget(moduleBox);
    featureLayout->addStretch(1);
    addPage(features);
    updateHint(); // every page widget exists now (request() reads them all)
}

QRadioButton* NewProjectWizard::dimensionButton(Dimension dimension) const {
    switch (dimension) {
    case Dimension::World3D:
        return m_dim3D;
    case Dimension::World2D:
        return m_dim2D;
    case Dimension::Hybrid:
        return m_dimHybrid;
    }
    return nullptr;
}

QCheckBox* NewProjectWizard::moduleCheck(const QString& module) const {
    if (module == QLatin1String("ai")) {
        return m_ai;
    }
    if (module == QLatin1String("cinematics")) {
        return m_cinematics;
    }
    if (module == QLatin1String("fx")) {
        return m_fx;
    }
    if (module == QLatin1String("mechanics")) {
        return m_mechanics;
    }
    if (module == QLatin1String("adventure")) {
        return m_adventure;
    }
    return nullptr;
}

NewProjectRequest NewProjectWizard::request() const {
    NewProjectRequest out;
    out.name = m_name->text().trimmed();
    const QString folder = out.name.isEmpty() ? tr("NewProject") : out.name;
    out.directory = QDir(m_location->text().trimmed()).filePath(folder);
    u32 flags = 0;
    if (m_dim3D->isChecked() || m_dimHybrid->isChecked()) {
        flags |= kProjectEnable3D;
    }
    if (m_dim2D->isChecked() || m_dimHybrid->isChecked()) {
        flags |= kProjectEnable2D;
    }
    if (m_ui->isChecked()) {
        flags |= kProjectEnableUI;
    }
    out.dimensionFlags = flags;
    out.modules.ai = m_ai->isChecked();
    out.modules.cinematics = m_cinematics->isChecked();
    out.modules.fx = m_fx->isChecked();
    out.modules.mechanics = m_mechanics->isChecked();
    out.modules.adventure = m_adventure->isChecked();
    return out;
}

void ProjectHubWidget::onOpenClicked() {
    const QString path = selectedProjectPath();
    if (!path.isEmpty()) {
        emit projectOpenRequested(path);
    }
}

} // namespace fuse::editor::qt
