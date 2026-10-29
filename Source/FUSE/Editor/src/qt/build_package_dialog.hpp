#pragma once
// File > Build / Package...: stages a project into a self-contained output folder.
// Steps: copy the project tree (minus caches), cook every model/texture/audio source with fuse_cook,
// optionally copy a runtime player and write run.sh / run.bat, then write package.json.
#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace fuse::editor::qt {

struct PackageOptions {
    QString projectRoot;
    QString outputDir;   ///< package goes to <outputDir>/<projectName>
    QString runtimePath; ///< optional player executable copied into bin/
    bool cookAssets = true;
    bool clean = true;   ///< remove an existing package folder first
    bool bundleLibraries = true; ///< Linux: copy the runtime's non-system shared libraries into lib/
    /// "Development" ships everything; "Shipping" also drops editor-only sidecars
    /// (*.prefablinks, *.variant, *.variantbase, *.materials.bak, editor layout files).
    QString configuration = QStringLiteral("Development");
    /// Extra excludes: wildcard patterns on the project-relative path or file name (e.g. "*.psd", "docs/*").
    QStringList excludePatterns;
    bool archive = false; ///< also write <outputDir>/<name>.tar.gz
};

struct PackageResult {
    bool ok = false;
    QString packageDir;
    int copiedFiles = 0;
    int cookedAssets = 0;
    int failedCooks = 0;
    int bundledLibraries = 0;
    int excludedFiles = 0;
    qint64 totalBytes = 0; ///< bytes copied into project/
    QString archivePath;   ///< set when the archive was written
    QString error;
};

/// Synchronous; `log` (optional) receives one line per step. Safe to call off the GUI thread.
PackageResult packageProject(const PackageOptions& opts, const std::function<void(const QString&)>& log = {});

class BuildPackageDialog final : public QDialog {
public:
    explicit BuildPackageDialog(const QString& projectRoot, QWidget* parent = nullptr);
    [[nodiscard]] PackageOptions options() const;
    [[nodiscard]] const PackageResult& lastResult() const { return m_result; }
    /// Runs the package step (blocking the dialog, log lines appear live).
    bool runPackage();
    /// Defaults to excluded patterns that rarely belong in a build.
    static QStringList defaultExcludes();

private:
    QString m_projectRoot;
    QLineEdit* m_output = nullptr;
    QComboBox* m_runtime = nullptr;
    QCheckBox* m_cook = nullptr;
    QCheckBox* m_clean = nullptr;
    QComboBox* m_config = nullptr;
    QLineEdit* m_excludes = nullptr;
    QCheckBox* m_archive = nullptr;
    QPushButton* m_openFolder = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_build = nullptr;
    PackageResult m_result;
};

} // namespace fuse::editor::qt
