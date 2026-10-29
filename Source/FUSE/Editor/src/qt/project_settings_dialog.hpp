#pragma once
// Project > Project Settings: edits <project>/project.json (fuse::project::ProjectManifest).
#include <QDialog>
#include <QString>

#include <fuse/project/manifest.hpp>

class QCheckBox;
class QLineEdit;
class QSpinBox;

namespace fuse::editor::qt {

class ProjectSettingsDialog final : public QDialog {
public:
    /// Loads `<projectRoot>/project.json`; `loaded()` is false when it cannot be read.
    explicit ProjectSettingsDialog(const QString& projectRoot, QWidget* parent = nullptr);
    [[nodiscard]] bool loaded() const { return m_loaded; }
    /// The manifest with the widget values applied.
    [[nodiscard]] project::ProjectManifest manifest() const;
    /// Writes project.json; false (and `error`) on a write failure.
    bool save(QString* error = nullptr) const;

    QLineEdit* nameEdit = nullptr;
    QLineEdit* world3DEdit = nullptr;
    QLineEdit* world2DEdit = nullptr;
    QCheckBox* enable3D = nullptr;
    QCheckBox* enable2D = nullptr;
    QCheckBox* enableUI = nullptr;
    QCheckBox* modAi = nullptr;
    QCheckBox* modCinematics = nullptr;
    QCheckBox* modFx = nullptr;
    QCheckBox* modMechanics = nullptr;
    QCheckBox* modAdventure = nullptr;
    QSpinBox* workerCap = nullptr;

private:
    QString m_root;
    project::ProjectManifest m_base;
    bool m_loaded = false;
};

/// Copies `sources` into `<projectRoot>/assets/<kind>/` (textures, models, audio, scenes, scripts,
/// misc by extension). Existing names get a `_N` suffix. Returns the project-relative paths written.
QStringList importAssetsIntoProject(const QString& projectRoot, const QStringList& sources, QString* error = nullptr);

/// Cook-on-import: one `fuse_cook` run for an imported project asset (models -> .fusemesh,
/// textures -> .fusetex, audio -> .fuseaudio under `assets/cooked/<kind>/`).
struct AssetCookJob {
    QString program;
    QStringList arguments;
    QString source; ///< project-relative source
    QString output; ///< project-relative cooked output
};
/// `FUSE_COOK` env, next to the editor binary, the build tree's Tools/FUSE, then PATH; empty if none.
QString findCookTool();
/// False when `relPath` has no cooker (scripts, scenes, misc) or no tool was found.
bool makeAssetCookJob(const QString& projectRoot, const QString& relPath, const QString& cookTool, AssetCookJob& job);

} // namespace fuse::editor::qt
