#include "build_package_dialog.hpp"

#include "project_settings_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

namespace fuse::editor::qt {

namespace {

bool skipDir(const QString& rel) {
    const QStringList parts = rel.split(QLatin1Char('/'));
    for (const QString& p : parts) {
        if (p.startsWith(QLatin1Char('.')) || p == QLatin1String("build")) return true;
    }
    return rel.startsWith(QLatin1String("assets/cooked"));
}

QString projectName(const QString& root) {
    QFile f(QDir(root).filePath(QStringLiteral("project.json")));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        const QString n = o.value(QStringLiteral("name")).toString();
        if (!n.isEmpty()) {
            QString safe;
            for (QChar c : n) safe += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) ? c : QLatin1Char('_');
            return safe;
        }
    }
    return QFileInfo(root).fileName();
}

} // namespace

PackageResult packageProject(const PackageOptions& opts, const std::function<void(const QString&)>& log) {
    auto say = [&](const QString& s) { if (log) log(s); };
    PackageResult r;
    const QDir src(opts.projectRoot);
    if (opts.projectRoot.isEmpty() || !QFileInfo::exists(src.filePath(QStringLiteral("project.json")))) {
        r.error = QStringLiteral("No project.json in %1").arg(opts.projectRoot);
        return r;
    }
    if (opts.outputDir.isEmpty()) {
        r.error = QStringLiteral("No output folder");
        return r;
    }
    const QString pkgDir = QDir(opts.outputDir).filePath(projectName(opts.projectRoot));
    const QString absSrc = QFileInfo(opts.projectRoot).canonicalFilePath();
    if (QDir::cleanPath(QFileInfo(pkgDir).absoluteFilePath()).startsWith(absSrc + QLatin1Char('/'))) {
        r.error = QStringLiteral("Output folder must be outside the project");
        return r;
    }
    r.packageDir = pkgDir;
    if (opts.clean && QFileInfo::exists(pkgDir)) {
        say(QStringLiteral("Cleaning %1").arg(pkgDir));
        QDir(pkgDir).removeRecursively();
    }
    const QDir pkg(pkgDir);
    const QString dataDir = pkg.filePath(QStringLiteral("project"));
    QDir().mkpath(dataDir);

    // 1. copy
    say(QStringLiteral("Copying project files..."));
    QStringList sources;
    QDirIterator it(absSrc, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QString rel = QDir(absSrc).relativeFilePath(abs);
        if (skipDir(QFileInfo(rel).path() == QLatin1String(".") ? QString() : QFileInfo(rel).path()) ||
            QFileInfo(rel).fileName().startsWith(QLatin1Char('.')))
            continue;
        const QString dst = QDir(dataDir).filePath(rel);
        QDir().mkpath(QFileInfo(dst).path());
        QFile::remove(dst);
        if (!QFile::copy(abs, dst)) {
            r.error = QStringLiteral("Copy failed: %1").arg(rel);
            return r;
        }
        ++r.copiedFiles;
        sources << rel;
    }
    say(QStringLiteral("Copied %1 file(s)").arg(r.copiedFiles));

    // 2. cook
    QJsonArray cooked;
    if (opts.cookAssets) {
        const QString tool = findCookTool();
        if (tool.isEmpty()) {
            say(QStringLiteral("fuse_cook not found; skipping cook (set FUSE_COOK)"));
        } else {
            for (const QString& rel : sources) {
                AssetCookJob job;
                if (!makeAssetCookJob(dataDir, rel, tool, job)) continue;
                QProcess p;
                p.setProcessChannelMode(QProcess::MergedChannels);
                p.start(job.program, job.arguments);
                const bool ok = p.waitForFinished(120000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 &&
                                QFileInfo::exists(QDir(dataDir).filePath(job.output));
                if (ok) {
                    ++r.cookedAssets;
                    cooked.append(job.output);
                    say(QStringLiteral("Cooked %1 -> %2").arg(job.source, job.output));
                } else {
                    ++r.failedCooks;
                    say(QStringLiteral("Cook FAILED %1: %2").arg(job.source, QString::fromLocal8Bit(p.readAll()).trimmed().right(300)));
                }
            }
        }
    }

    // 3. runtime + launchers
    QString runtimeName;
    if (!opts.runtimePath.isEmpty()) {
        const QFileInfo rt(opts.runtimePath);
        if (!rt.isFile()) {
            r.error = QStringLiteral("Runtime not found: %1").arg(opts.runtimePath);
            return r;
        }
        runtimeName = rt.fileName();
        pkg.mkpath(QStringLiteral("bin"));
        const QString dst = pkg.filePath(QStringLiteral("bin/") + runtimeName);
        QFile::remove(dst);
        if (!QFile::copy(rt.absoluteFilePath(), dst)) {
            r.error = QStringLiteral("Cannot copy runtime");
            return r;
        }
        QFile::setPermissions(dst, QFile::permissions(dst) | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
        QFile sh(pkg.filePath(QStringLiteral("run.sh")));
        if (sh.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sh.write(QStringLiteral("#!/bin/sh\ncd \"$(dirname \"$0\")\" && exec ./bin/%1 --present project \"$@\"\n").arg(runtimeName).toUtf8());
            sh.close();
            sh.setPermissions(sh.permissions() | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
        }
        QFile bat(pkg.filePath(QStringLiteral("run.bat")));
        if (bat.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QString exe = runtimeName;
            if (!exe.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) exe += QStringLiteral(".exe");
            bat.write(QStringLiteral("@echo off\r\ncd /d \"%~dp0\"\r\nbin\\%1 --present project %*\r\n").arg(exe).toUtf8());
        }
        say(QStringLiteral("Runtime: bin/%1 (+ run.sh / run.bat)").arg(runtimeName));
    }

    // 4. manifest
    QJsonObject m;
    m.insert(QStringLiteral("project"), projectName(opts.projectRoot));
    m.insert(QStringLiteral("source"), absSrc);
    m.insert(QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    m.insert(QStringLiteral("files"), r.copiedFiles);
    m.insert(QStringLiteral("cooked"), cooked);
    m.insert(QStringLiteral("failedCooks"), r.failedCooks);
    m.insert(QStringLiteral("runtime"), runtimeName.isEmpty() ? QString() : QStringLiteral("bin/") + runtimeName);
    QFile mf(pkg.filePath(QStringLiteral("package.json")));
    if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.error = QStringLiteral("Cannot write package.json");
        return r;
    }
    mf.write(QJsonDocument(m).toJson());
    r.ok = r.failedCooks == 0;
    if (!r.ok) r.error = QStringLiteral("%1 asset(s) failed to cook").arg(r.failedCooks);
    say(r.ok ? QStringLiteral("Package ready: %1").arg(pkgDir) : r.error);
    return r;
}

BuildPackageDialog::BuildPackageDialog(const QString& projectRoot, QWidget* parent) : QDialog(parent), m_projectRoot(projectRoot) {
    setObjectName(QStringLiteral("fuseBuildPackageDialog"));
    setWindowTitle(tr("Build / Package"));
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    auto* outRow = new QHBoxLayout;
    m_output = new QLineEdit(QDir(QFileInfo(projectRoot).absolutePath()).filePath(QStringLiteral("packages")), this);
    m_output->setObjectName(QStringLiteral("fusePackageOutput"));
    auto* browse = new QPushButton(tr("Browse..."), this);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Output Folder"), m_output->text());
        if (!d.isEmpty()) m_output->setText(d);
    });
    outRow->addWidget(m_output, 1);
    outRow->addWidget(browse);
    form->addRow(tr("Output folder"), outRow);

    m_runtime = new QComboBox(this);
    m_runtime->setObjectName(QStringLiteral("fusePackageRuntime"));
    m_runtime->setEditable(true);
    m_runtime->addItem(tr("(none - data only)"), QString());
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList guesses = {QStringLiteral("../../Apps/Demo3DEmpty/demo_3d_empty"), QStringLiteral("../../Apps/Demo2DSprites/demo_2d_sprites"),
                                 QStringLiteral("../../Apps/HybridHud/demo_hybrid_hud"), QStringLiteral("demo_3d_empty"),
                                 QStringLiteral("demo_2d_sprites")};
    for (const QString& g : guesses) {
        const QFileInfo fi(appDir.filePath(g));
        if (fi.isFile() && fi.isExecutable()) m_runtime->addItem(fi.fileName(), fi.canonicalFilePath());
    }
    form->addRow(tr("Runtime player"), m_runtime);
    m_cook = new QCheckBox(tr("Cook models / textures / audio with fuse_cook"), this);
    m_cook->setChecked(true);
    form->addRow(QString(), m_cook);
    m_clean = new QCheckBox(tr("Clean output first"), this);
    m_clean->setChecked(true);
    form->addRow(QString(), m_clean);
    lay->addLayout(form);
    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("fusePackageLog"));
    m_log->setReadOnly(true);
    m_log->setMinimumSize(520, 200);
    lay->addWidget(m_log, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_build = box->addButton(tr("Build"), QDialogButtonBox::ActionRole);
    m_build->setObjectName(QStringLiteral("fusePackageBuild"));
    connect(m_build, &QPushButton::clicked, this, [this]() { runPackage(); });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
}

PackageOptions BuildPackageDialog::options() const {
    PackageOptions o;
    o.projectRoot = m_projectRoot;
    o.outputDir = m_output->text().trimmed();
    const int idx = m_runtime->currentIndex();
    const QString data = idx >= 0 && m_runtime->itemText(idx) == m_runtime->currentText() ? m_runtime->itemData(idx).toString()
                                                                                           : m_runtime->currentText().trimmed();
    o.runtimePath = data;
    o.cookAssets = m_cook->isChecked();
    o.clean = m_clean->isChecked();
    return o;
}

bool BuildPackageDialog::runPackage() {
    m_build->setEnabled(false);
    m_log->clear();
    m_result = packageProject(options(), [this](const QString& line) {
        m_log->appendPlainText(line);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    });
    if (!m_result.ok && !m_result.error.isEmpty()) m_log->appendPlainText(tr("ERROR: %1").arg(m_result.error));
    m_build->setEnabled(true);
    return m_result.ok;
}

} // namespace fuse::editor::qt
