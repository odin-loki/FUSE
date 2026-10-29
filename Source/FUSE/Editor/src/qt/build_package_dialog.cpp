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
#include <QRegularExpression>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
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

bool matchesAny(const QString& rel, const QStringList& patterns) {
    const QString name = QFileInfo(rel).fileName();
    for (const QString& raw : patterns) {
        const QString pat = raw.trimmed();
        if (pat.isEmpty()) continue;
        const QRegularExpression re(QRegularExpression::anchoredPattern(QRegularExpression::wildcardToRegularExpression(
                                        pat, QRegularExpression::UnanchoredWildcardConversion)),
                                    QRegularExpression::CaseInsensitiveOption);
        if (re.match(rel).hasMatch() || re.match(name).hasMatch()) return true;
    }
    return false;
}

const QStringList& shippingExcludes() {
    static const QStringList p = {QStringLiteral("*.prefablinks"), QStringLiteral("*.variant"), QStringLiteral("*.variantbase"),
                                  QStringLiteral("*.bak"), QStringLiteral("editor_layout*.ini"), QStringLiteral("*.fuselevel.names")};
    return p;
}

QString humanBytes(qint64 b) {
    if (b < 1024) return QStringLiteral("%1 B").arg(b);
    if (b < 1024 * 1024) return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
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
    QStringList excludes = opts.excludePatterns;
    const bool shipping = opts.configuration.compare(QStringLiteral("Shipping"), Qt::CaseInsensitive) == 0;
    if (shipping) excludes += shippingExcludes();
    QDirIterator it(absSrc, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QString rel = QDir(absSrc).relativeFilePath(abs);
        if (skipDir(QFileInfo(rel).path() == QLatin1String(".") ? QString() : QFileInfo(rel).path()) ||
            QFileInfo(rel).fileName().startsWith(QLatin1Char('.')))
            continue;
        if (matchesAny(rel, excludes)) {
            ++r.excludedFiles;
            continue;
        }
        const QString dst = QDir(dataDir).filePath(rel);
        QDir().mkpath(QFileInfo(dst).path());
        QFile::remove(dst);
        if (!QFile::copy(abs, dst)) {
            r.error = QStringLiteral("Copy failed: %1").arg(rel);
            return r;
        }
        ++r.copiedFiles;
        r.totalBytes += QFileInfo(abs).size();
        sources << rel;
    }
    say(QStringLiteral("Copied %1 file(s), %2 (%3 configuration%4)")
            .arg(r.copiedFiles)
            .arg(humanBytes(r.totalBytes), shipping ? QStringLiteral("Shipping") : QStringLiteral("Development"),
                 r.excludedFiles > 0 ? QStringLiteral(", %1 excluded").arg(r.excludedFiles) : QString()));

    // 2. cook
    QJsonArray cooked;
    if (opts.cookAssets) {
        const QString tool = findCookTool();
        if (tool.isEmpty()) {
            say(QStringLiteral("fuse_cook not found; skipping cook (set FUSE_COOK)"));
        } else {
            // Project-wide cook (scans the staged copy, or honours its cook_manifest.json).
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(tool, {QStringLiteral("--project"), dataDir});
            const bool finished = p.waitForFinished(600000);
            const QStringList lines = QString::fromLocal8Bit(p.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            const QString prefix = QDir(dataDir).absolutePath() + QLatin1Char('/');
            for (QString line : lines) {
                line = line.trimmed();
                if (!line.startsWith(QLatin1Char('['))) continue;
                line.replace(prefix, QString());
                const int arrow = line.indexOf(QStringLiteral(" -> "));
                const int paren = line.indexOf(QStringLiteral(" ("), arrow);
                if (arrow < 0 || paren < 0) continue;
                const QString out = line.mid(arrow + 4, paren - arrow - 4);
                if (line.contains(QStringLiteral("(ok)"))) {
                    ++r.cookedAssets;
                    cooked.append(out);
                    say(QStringLiteral("Cooked %1").arg(line.left(paren)));
                } else {
                    ++r.failedCooks;
                    say(QStringLiteral("Cook FAILED %1").arg(line));
                }
            }
            if (!finished || p.exitStatus() != QProcess::NormalExit || (p.exitCode() != 0 && r.failedCooks == 0)) {
                ++r.failedCooks;
                say(QStringLiteral("fuse_cook --project failed (exit %1)").arg(p.exitCode()));
            }
            QDir(QDir(dataDir).filePath(QStringLiteral(".fuse"))).removeRecursively(); // cook cache is not shipped
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
#if defined(__linux__)
        if (opts.bundleLibraries) {
            QProcess ldd;
            ldd.start(QStringLiteral("ldd"), {rt.absoluteFilePath()});
            if (ldd.waitForFinished(30000)) {
                const QStringList lines = QString::fromLocal8Bit(ldd.readAllStandardOutput()).split(QLatin1Char('\n'));
                for (const QString& line : lines) {
                    const int arrow = line.indexOf(QStringLiteral("=> /"));
                    if (arrow < 0) continue;
                    const QString lib = line.mid(arrow + 3).section(QLatin1Char(' '), 0, 0);
                    if (lib.startsWith(QLatin1String("/lib")) || lib.startsWith(QLatin1String("/usr/lib"))) continue;
                    pkg.mkpath(QStringLiteral("lib"));
                    const QString dstLib = pkg.filePath(QStringLiteral("lib/") + QFileInfo(lib).fileName());
                    QFile::remove(dstLib);
                    if (QFile::copy(QFileInfo(lib).canonicalFilePath(), dstLib)) ++r.bundledLibraries;
                }
            }
            if (r.bundledLibraries > 0) say(QStringLiteral("Bundled %1 shared librar(ies) into lib/").arg(r.bundledLibraries));
        }
#endif
        QFile sh(pkg.filePath(QStringLiteral("run.sh")));
        if (sh.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sh.write(QStringLiteral("#!/bin/sh\ncd \"$(dirname \"$0\")\" || exit 1\nexport LD_LIBRARY_PATH=\"$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\nexec ./bin/%1 --present project \"$@\"\n").arg(runtimeName).toUtf8());
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
    m.insert(QStringLiteral("bundledLibraries"), r.bundledLibraries);
    m.insert(QStringLiteral("configuration"), shipping ? QStringLiteral("Shipping") : QStringLiteral("Development"));
    m.insert(QStringLiteral("excluded"), r.excludedFiles);
    m.insert(QStringLiteral("bytes"), static_cast<double>(r.totalBytes));
    m.insert(QStringLiteral("runtime"), runtimeName.isEmpty() ? QString() : QStringLiteral("bin/") + runtimeName);
    QFile mf(pkg.filePath(QStringLiteral("package.json")));
    if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.error = QStringLiteral("Cannot write package.json");
        return r;
    }
    mf.write(QJsonDocument(m).toJson());
    mf.close();
    r.ok = r.failedCooks == 0;
    if (r.ok && opts.archive) {
        const QString archive = QDir(opts.outputDir).filePath(QFileInfo(pkgDir).fileName() + QStringLiteral(".tar.gz"));
        QFile::remove(archive);
        QProcess tar;
        tar.setProcessChannelMode(QProcess::MergedChannels);
        tar.setWorkingDirectory(opts.outputDir);
        tar.start(QStringLiteral("tar"), {QStringLiteral("-czf"), archive, QFileInfo(pkgDir).fileName()});
        if (tar.waitForFinished(600000) && tar.exitStatus() == QProcess::NormalExit && tar.exitCode() == 0) {
            r.archivePath = archive;
            say(QStringLiteral("Archive: %1 (%2)").arg(archive, humanBytes(QFileInfo(archive).size())));
        } else {
            r.ok = false;
            r.error = QStringLiteral("Archive failed: %1").arg(QString::fromLocal8Bit(tar.readAll()).trimmed());
            return r;
        }
    }
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
    const QStringList guesses = {QStringLiteral("../Apps/Demo3DEmpty/demo_3d_empty"), QStringLiteral("../Apps/Demo2DSprites/demo_2d_sprites"),
                                 QStringLiteral("../Apps/HybridHud/demo_hybrid_hud"), QStringLiteral("demo_3d_empty"),
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
    m_config = new QComboBox(this);
    m_config->setObjectName(QStringLiteral("fusePackageConfig"));
    m_config->addItems({QStringLiteral("Development"), QStringLiteral("Shipping")});
    m_config->setToolTip(tr("Shipping also leaves out editor-only sidecars (prefab links, variant snapshots, backups)"));
    form->addRow(tr("Configuration"), m_config);
    m_excludes = new QLineEdit(this);
    m_excludes->setObjectName(QStringLiteral("fusePackageExcludes"));
    m_excludes->setPlaceholderText(tr("e.g. *.psd; *.blend; docs/*"));
    m_excludes->setToolTip(tr("Semicolon-separated wildcards matched against the project-relative path or file name"));
    form->addRow(tr("Exclude"), m_excludes);
    m_archive = new QCheckBox(tr("Also write a .tar.gz archive"), this);
    m_archive->setObjectName(QStringLiteral("fusePackageArchive"));
    form->addRow(QString(), m_archive);
    {
        // Remember the last settings per project.
        QSettings st;
        st.beginGroup(QStringLiteral("package/") + QString::number(qHash(QFileInfo(projectRoot).absoluteFilePath())));
        if (st.contains(QStringLiteral("output"))) m_output->setText(st.value(QStringLiteral("output")).toString());
        m_config->setCurrentText(st.value(QStringLiteral("configuration"), QStringLiteral("Development")).toString());
        m_excludes->setText(st.value(QStringLiteral("excludes"), defaultExcludes().join(QStringLiteral("; "))).toString());
        m_archive->setChecked(st.value(QStringLiteral("archive"), false).toBool());
        m_cook->setChecked(st.value(QStringLiteral("cook"), true).toBool());
    }
    lay->addLayout(form);
    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("fusePackageLog"));
    m_log->setReadOnly(true);
    m_log->setMinimumSize(520, 200);
    lay->addWidget(m_log, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_build = box->addButton(tr("Build"), QDialogButtonBox::ActionRole);
    m_build->setObjectName(QStringLiteral("fusePackageBuild"));
    m_openFolder = box->addButton(tr("Open Folder"), QDialogButtonBox::ActionRole);
    m_openFolder->setObjectName(QStringLiteral("fusePackageOpenFolder"));
    m_openFolder->setEnabled(false);
    connect(m_openFolder, &QPushButton::clicked, this, [this]() {
        if (!m_result.packageDir.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_result.packageDir));
    });
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
    o.configuration = m_config->currentText();
    for (const QString& p : m_excludes->text().split(QRegularExpression(QStringLiteral("[;,]")), Qt::SkipEmptyParts)) {
        if (!p.trimmed().isEmpty()) o.excludePatterns << p.trimmed();
    }
    o.archive = m_archive->isChecked();
    return o;
}

bool BuildPackageDialog::runPackage() {
    m_build->setEnabled(false);
    m_openFolder->setEnabled(false);
    m_log->clear();
    {
        QSettings st;
        st.beginGroup(QStringLiteral("package/") + QString::number(qHash(QFileInfo(m_projectRoot).absoluteFilePath())));
        st.setValue(QStringLiteral("output"), m_output->text().trimmed());
        st.setValue(QStringLiteral("configuration"), m_config->currentText());
        st.setValue(QStringLiteral("excludes"), m_excludes->text());
        st.setValue(QStringLiteral("archive"), m_archive->isChecked());
        st.setValue(QStringLiteral("cook"), m_cook->isChecked());
    }
    m_result = packageProject(options(), [this](const QString& line) {
        m_log->appendPlainText(line);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    });
    if (!m_result.ok && !m_result.error.isEmpty()) m_log->appendPlainText(tr("ERROR: %1").arg(m_result.error));
    if (m_result.ok) {
        m_log->appendPlainText(tr("Done: %1 file(s), %2 cooked, %3 excluded%4")
                                   .arg(m_result.copiedFiles)
                                   .arg(m_result.cookedAssets)
                                   .arg(m_result.excludedFiles)
                                   .arg(m_result.archivePath.isEmpty() ? QString() : tr(", archive %1").arg(m_result.archivePath)));
    }
    m_openFolder->setEnabled(m_result.ok && QFileInfo::exists(m_result.packageDir));
    m_build->setEnabled(true);
    return m_result.ok;
}

QStringList BuildPackageDialog::defaultExcludes() {
    return {QStringLiteral("*.psd"), QStringLiteral("*.blend"), QStringLiteral("*.blend1"), QStringLiteral("*.kra"),
            QStringLiteral("*.xcf"), QStringLiteral("Thumbs.db")};
}

} // namespace fuse::editor::qt
