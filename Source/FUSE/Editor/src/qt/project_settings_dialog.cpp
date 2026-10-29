#include "project_settings_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSaveFile>
#include <QSpinBox>
#include <QVBoxLayout>

#include <fuse/project/loader.hpp>

namespace fuse::editor::qt {

ProjectSettingsDialog::ProjectSettingsDialog(const QString& projectRoot, QWidget* parent)
    : QDialog(parent), m_root(QFileInfo(projectRoot).absoluteFilePath()) {
    setObjectName(QStringLiteral("fuseProjectSettingsDialog"));
    setWindowTitle(tr("Project Settings"));
    const project::LoadResult r = project::loadFromDirectory(m_root.toStdString());
    m_loaded = r.status == project::LoadStatus::Ok;
    m_base = r.manifest;

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    nameEdit = new QLineEdit(QString::fromStdString(m_base.name), this);
    nameEdit->setObjectName(QStringLiteral("projectName"));
    world3DEdit = new QLineEdit(QString::fromStdString(m_base.defaultWorld3D), this);
    world3DEdit->setObjectName(QStringLiteral("defaultWorld3D"));
    world2DEdit = new QLineEdit(QString::fromStdString(m_base.defaultWorld2D), this);
    world2DEdit->setObjectName(QStringLiteral("defaultWorld2D"));
    workerCap = new QSpinBox(this);
    workerCap->setObjectName(QStringLiteral("workerCap"));
    workerCap->setRange(0, 256);
    workerCap->setSpecialValueText(tr("Auto"));
    workerCap->setValue(static_cast<int>(m_base.workerCap));
    form->addRow(tr("Name"), nameEdit);
    form->addRow(tr("Default 3D world"), world3DEdit);
    form->addRow(tr("Default 2D world"), world2DEdit);
    form->addRow(tr("Worker threads"), workerCap);
    layout->addLayout(form);

    const auto box = [this](const QString& title) {
        auto* g = new QGroupBox(title, this);
        new QHBoxLayout(g);
        return g;
    };
    const auto check = [this](QGroupBox* g, const QString& text, bool on, const char* name) {
        auto* c = new QCheckBox(text, g);
        c->setChecked(on);
        c->setObjectName(QString::fromLatin1(name));
        g->layout()->addWidget(c);
        return c;
    };
    QGroupBox* dims = box(tr("Dimensions"));
    enable3D = check(dims, tr("3D"), m_base.dimensions.enable3D, "enable3D");
    enable2D = check(dims, tr("2D"), m_base.dimensions.enable2D, "enable2D");
    enableUI = check(dims, tr("UI"), m_base.dimensions.enableUI, "enableUI");
    layout->addWidget(dims);
    QGroupBox* mods = box(tr("Modules"));
    modAi = check(mods, tr("AI"), m_base.modules.ai, "modAi");
    modCinematics = check(mods, tr("Cinematics"), m_base.modules.cinematics, "modCinematics");
    modFx = check(mods, tr("FX"), m_base.modules.fx, "modFx");
    modMechanics = check(mods, tr("Mechanics"), m_base.modules.mechanics, "modMechanics");
    modAdventure = check(mods, tr("Adventure"), m_base.modules.adventure, "modAdventure");
    layout->addWidget(mods);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setEnabled(m_loaded);
    buttons->setEnabled(true);
}

project::ProjectManifest ProjectSettingsDialog::manifest() const {
    project::ProjectManifest m = m_base;
    m.schemaVersion = project::kProjectSchemaVersion;
    m.name = nameEdit->text().trimmed().toStdString();
    m.defaultWorld3D = world3DEdit->text().trimmed().toStdString();
    m.defaultWorld2D = world2DEdit->text().trimmed().toStdString();
    m.workerCap = static_cast<u32>(workerCap->value());
    m.dimensions = {enable3D->isChecked(), enable2D->isChecked(), enableUI->isChecked()};
    m.modules = {modAi->isChecked(), modCinematics->isChecked(), modFx->isChecked(), modMechanics->isChecked(),
                 modAdventure->isChecked()};
    return m;
}

bool ProjectSettingsDialog::save(QString* error) const {
    const project::ProjectManifest m = manifest();
    if (m.name.empty()) {
        if (error) *error = tr("Project name must not be empty");
        return false;
    }
    if (!m.dimensions.enable3D && !m.dimensions.enable2D) {
        if (error) *error = tr("Enable at least one of 3D / 2D");
        return false;
    }
    QSaveFile f(QDir(m_root).filePath(QStringLiteral("project.json")));
    const std::string json = project::writeManifestJson(m);
    if (!f.open(QIODevice::WriteOnly) || f.write(json.data(), static_cast<qint64>(json.size())) < 0 || !f.commit()) {
        if (error) *error = tr("Cannot write project.json: %1").arg(f.errorString());
        return false;
    }
    return true;
}

static QString assetKind(const QString& suffix) {
    const QString s = suffix.toLower();
    if (QStringList{"png", "jpg", "jpeg", "tga", "bmp", "hdr", "exr", "dds", "ktx", "ktx2"}.contains(s)) return QStringLiteral("textures");
    if (QStringList{"gltf", "glb", "obj", "fbx"}.contains(s)) return QStringLiteral("models");
    if (QStringList{"wav", "ogg", "mp3", "flac"}.contains(s)) return QStringLiteral("audio");
    if (QStringList{"fuselevel", "fusescene", "prefab"}.contains(s)) return QStringLiteral("scenes");
    if (QStringList{"lua"}.contains(s)) return QStringLiteral("scripts");
    return QStringLiteral("misc");
}

QStringList importAssetsIntoProject(const QString& projectRoot, const QStringList& sources, QString* error) {
    QStringList written;
    const QDir root(projectRoot);
    for (const QString& src : sources) {
        const QFileInfo in(src);
        if (!in.isFile()) {
            if (error) *error = QStringLiteral("not a file: %1").arg(src);
            continue;
        }
        const QString rel = QStringLiteral("assets/%1").arg(assetKind(in.suffix()));
        root.mkpath(rel);
        QString name = in.fileName();
        for (int n = 1; QFileInfo::exists(root.filePath(rel + QLatin1Char('/') + name)); ++n) {
            name = QStringLiteral("%1_%2.%3").arg(in.completeBaseName()).arg(n).arg(in.suffix());
        }
        const QString dst = rel + QLatin1Char('/') + name;
        if (QFile::copy(in.absoluteFilePath(), root.filePath(dst))) {
            written << dst;
        } else if (error) {
            *error = QStringLiteral("copy failed: %1").arg(src);
        }
    }
    return written;
}

} // namespace fuse::editor::qt
