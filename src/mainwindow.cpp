/**********************************************************************
 *  mainwindow.cpp
 **********************************************************************
 * Copyright (C) 2015-2026 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *
 * This file is part of mx-repo-manager.
 *
 * mx-repo-manager is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mx-repo-manager is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mx-repo-manager.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/
#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QCloseEvent>
#include <QFile>
#include <QDir>
#include <QMetaEnum>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QProgressBar>
#include <QRadioButton>
#include <QScreen>
#include <QScopedValueRollback>
#include <utility>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QStringList>
#include <QRegularExpression>

#include "about.h"
#include <chrono>

using namespace std::chrono_literals;

MainWindow::MainWindow(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow)
{
    qDebug().noquote() << QApplication::applicationName() << "version:" << QApplication::applicationVersion();
    ui->setupUi(this);
    setConnections();
    setWindowFlags(Qt::Window);
    initializeIcons();
    shell = new Cmd(this);

    connect(shell, &Cmd::started, this, &MainWindow::procStart);
    connect(shell, &Cmd::done, this, &MainWindow::procDone);

    setProgressBar();

    ui->pushOk->setDisabled(true);

    setWindowTitle(tr("MX Repo Manager"));
    ui->tabWidget->setCurrentWidget(ui->tabMX);
    refresh(true);

    restoreWindowGeometry();
}

MainWindow::~MainWindow()
{
    if (sources_changed) {
        progress->setLabelText(tr("Updating package sources..."));
        progress->show();
        QApplication::processEvents();

        shell->procAsRoot("apt-get", {"update"}, nullptr, nullptr, QuietMode::Yes);
    }

    delete ui;
}

void MainWindow::initializeIcons()
{
    setIconIfNull(ui->pushOk, "dialog-ok", ":/icons/dialog-ok.svg");
    setIconIfNull(ui->pushFastestMX, "cursor-arrow", ":/icons/cursor-arrow.svg");
    setIconIfNull(ui->pushFastestDebian, "cursor-arrow", ":/icons/cursor-arrow.svg");
}

void MainWindow::setIconIfNull(QPushButton *button, const QString &themeIcon, const QString &fallbackIcon)
{
    if (button->icon().isNull()) {
        button->setIcon(QIcon::fromTheme(themeIcon, QIcon(fallbackIcon)));
    }
}

void MainWindow::restoreWindowGeometry()
{
    QSize size = this->size();
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
        if (isMaximized()) {
            resize(size);
            centerWindow();
        }
    }
}

void MainWindow::refresh(bool force)
{
    if (!queued_changes.isEmpty()) {
        queued_changes.clear();
        QMessageBox::information(this, tr("Sources reloaded"), tr("Reloading the sources discarded pending checkbox changes."));
    }
    getCurrentRepo(force);
    displayMXRepos(repos, {});
    displayAllRepos(listAptFiles());
    ui->lineSearch->clear();
    ui->lineSearch->setFocus();
    ui->pushOk->setDisabled(true);
    radioSelectionChanged = false;
}

namespace
{
bool hasDebianComponents(const QString &components)
{
    static const QStringList standard {"main", "contrib", "non-free", "non-free-firmware"};
    const auto values = components.simplified().split(' ', Qt::SkipEmptyParts);
    return !values.isEmpty() && std::all_of(values.cbegin(), values.cend(),
                                            [](const QString &v) { return standard.contains(v); });
}

bool isDebianArchive(const QString &uri, const QString &signedBy, const QString &previousMirror, bool dedicatedArchive)
{
    const QUrl url(uri);
    if (!url.isValid() || !QStringList{"http", "https", "ftp"}.contains(url.scheme())) {
        return false;
    }
    const bool knownMirror = !previousMirror.isEmpty()
        && url.matches(QUrl(previousMirror), QUrl::StripTrailingSlash);
    const bool archivePath = url.path(QUrl::FullyEncoded).endsWith("/debian")
        || url.path(QUrl::FullyEncoded).endsWith("/debian/");
    if (!archivePath && !knownMirror) {
        return false;
    }
    if (!signedBy.isEmpty()) {
        static const QRegularExpression archiveKey(
            R"(^/usr/share/keyrings/debian-archive-(?:keyring|[a-z]+-(?:automatic|stable|security-automatic))\.(?:gpg|pgp)$)");
        return archiveKey.match(signedBy).hasMatch();
    }
    return dedicatedArchive || knownMirror || url.host() == "deb.debian.org" || url.host() == "ftp.debian.org"
        || (url.host().startsWith("ftp.") && url.host().endsWith(".debian.org"));
}

// Work on physical lines so that whitespace, comments, and continuation lines survive a URI edit.
QByteArray switchDebianMirror(const QByteArray &original, bool deb822, const QString &mirror,
                              const QString &previousMirror, bool dedicatedFile)
{
    QStringList lines = QString::fromLatin1(original).split('\n');
    static const QRegularExpression legacy(
        R"(^[ \t]*#?[ \t]*deb(?:-src)?[ \t]+(?:\[([^\]]*)\][ \t]+)?(\S+)[ \t]+([^\s#]+))");
    static const QRegularExpression signingOption(R"((?:^|\s)signed-by=(\S+))",
                                                  QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression token(R"(\S+)");
    auto replacement = [&](const QString &uri) {
        return uri.endsWith('/') ? mirror + '/' : mirror;
    };
    if (!deb822) {
        for (QString &line : lines) {
            const auto entry = legacy.match(line);
            if (!entry.hasMatch() || entry.captured(3).contains("security", Qt::CaseInsensitive)) {
                continue;
            }
            const QString key = signingOption.match(entry.captured(1)).captured(1);
            const QString components = line.mid(entry.capturedEnd(3)).section('#', 0, 0);
            if (isDebianArchive(entry.captured(2), key, previousMirror, dedicatedFile && hasDebianComponents(components))) {
                line.replace(entry.capturedStart(2), entry.capturedLength(2), replacement(entry.captured(2)));
            }
        }
    } else {
        for (qsizetype start = 0; start < lines.size();) {
            qsizetype end = start;
            QString field, key, suites, components;
            QList<qsizetype> uriLines;
            while (end < lines.size() && !lines.at(end).trimmed().isEmpty()) {
                const QString &line = lines.at(end++);
                if (line.startsWith('#')) {
                    continue;
                }
                qsizetype offset = 0;
                if (!line.front().isSpace()) {
                    offset = line.indexOf(':') + 1;
                    field = offset ? line.left(offset - 1).toLower() : QString();
                }
                if (field == "signed-by") {
                    key += " " + line.mid(offset).trimmed();
                } else if (field == "suites") {
                    suites += " " + line.mid(offset);
                } else if (field == "components") {
                    components += " " + line.mid(offset);
                } else if (field == "uris") {
                    uriLines.append(end - 1);
                }
            }
            if (!suites.contains("security", Qt::CaseInsensitive)) {
                for (qsizetype index : uriLines) {
                    QString &line = lines[index];
                    const QString source = line;
                    const qsizetype offset = source.front().isSpace() ? 0 : source.indexOf(':') + 1;
                    auto urls = token.globalMatch(source, offset);
                    qsizetype delta = 0;
                    while (urls.hasNext()) {
                        const auto match = urls.next();
                        if (isDebianArchive(match.captured(), key.trimmed(), previousMirror,
                                            dedicatedFile && hasDebianComponents(components))) {
                            const QString value = replacement(match.captured());
                            line.replace(match.capturedStart() + delta, match.capturedLength(), value);
                            delta += value.size() - match.capturedLength();
                        }
                    }
                }
            }
            start = end + 1;
        }
    }
    return lines.join('\n').toLatin1();
}
} // namespace

void MainWindow::replaceDebianRepos(const QString &url)
{
    if (!isValidRepositoryUrl(url)) {
        qWarning() << "Invalid repository URL:" << url;
        return;
    }

    const QStringList files {"/etc/apt/sources.list.d/debian.list", "/etc/apt/sources.list.d/debian.sources",
                             "/etc/apt/sources.list.d/debian-stable-updates.list",
                             "/etc/apt/sources.list.d/debian-stable-updates.sources", "/etc/apt/sources.list"};
    const QDir backupDir("/etc/apt/sources.list.d/backups");

    if (!backupDir.exists() && !Cmd().procAsRoot("mkdir", {"-p", backupDir.path()})) {
        qWarning() << "Failed to create backup directory:" << backupDir.path();
        return;
    }

    bool anyChange = false;
    bool failed = false;
    const QString trimmedUrl = url.trimmed().remove(QRegularExpression("/$"));
    QStringList changedFiles;
    QStringList changedFilesBackups;

    for (const QString &filePath : files) {
        if (!QFile::exists(filePath)) {
            continue;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Could not open file:" << file.fileName() << file.errorString();
            continue;
        }

        const QByteArray original = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            failed = true;
            break;
        }
        const QByteArray content = switchDebianMirror(original, filePath.endsWith(".sources"),
            QString::fromLatin1(QUrl(trimmedUrl).toEncoded()), settings.value("lastDebianMirror").toString(),
            filePath != "/etc/apt/sources.list");
        file.close();
        if (content == original) {
            continue;
        }

        QFileInfo fileInfo(filePath);
        const QString backupFilePath = backupDir.absoluteFilePath(
            fileInfo.fileName() + "." + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));

        if (!Cmd().procAsRoot("cp", {filePath, backupFilePath})) {
            qWarning() << "Failed to backup" << filePath;
            failed = true;
            break;
        }

        // Track for rollback as soon as the backup exists: writeUpdatedFile() can fail after its
        // "mv" step already replaced the file's contents, so a false return doesn't mean filePath
        // is untouched.
        changedFiles << filePath;
        changedFilesBackups << backupFilePath;

        if (!Cmd().installAsRoot(filePath, content)) {
            failed = true;
            break;
        }

        anyChange = true;
    }

    if (failed) {
        qWarning() << "Rolling back" << changedFiles.size() << "file(s) after a failed repository update";
        bool rollbackFailed = false;
        for (int i = 0; i < changedFiles.size(); ++i) {
            if (!Cmd().procAsRoot("cp", {changedFilesBackups.at(i), changedFiles.at(i)})) {
                qWarning() << "Failed to roll back" << changedFiles.at(i) << "from" << changedFilesBackups.at(i);
                rollbackFailed = true;
            }
        }
        QMessageBox::critical(this, tr("Error"),
                              rollbackFailed ? tr("Failed to update the repository sources, and some changes could "
                                                  "not be automatically rolled back. Check %1 to restore manually.")
                                                    .arg(backupDir.path())
                                            : tr("Failed to update the repository sources. Any changes made have "
                                                 "been rolled back."));
        return;
    }

    if (anyChange) {
        settings.setValue("lastDebianMirror", trimmedUrl);
        sources_changed = true;
        QMessageBox::information(this, tr("Success"),
                                 tr("Your new selection will take effect the next time sources are updated."));
    } else {
        QMessageBox::information(this, tr("No Changes"),
                                 tr("No matching Debian archive URI needed updating."));
    }
}

bool MainWindow::writeUpdatedFile(const QString &filePath, const QString &content)
{
    Cmd cmd;
    if (!cmd.installAsRoot(filePath, content.toUtf8())) {
        qWarning() << "Failed to replace the file and update permissions for" << filePath;
        return false;
    }
    return true;
}

void MainWindow::readMXRepos()
{
    repos.clear();
    listMXUrls.clear();
    QTemporaryDir tmpdir;
    QFile file(tmpdir.filePath("repos.txt"));
    const QString remoteUrl = "https://raw.githubusercontent.com/MX-Linux/mx-repo-list/master/repos.txt";
    QStringList paths;
    if (tmpdir.isValid() && downloadFile(remoteUrl, &file, 3s)) {
        paths << file.fileName();
    }
    paths << "/usr/share/mx-repo-list/repos.txt";
    for (const auto &path : paths) {
        QFile catalog(path);
        if (!catalog.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QString content = QString::fromUtf8(catalog.readAll());
        if (catalog.error() != QFileDevice::NoError) {
            continue;
        }
        for (const QString &entry : content.split('\n')) {
            const QString line = entry.trimmed();
            const QString url = line.section(" - ", 1).trimmed();
            if (!line.startsWith('#') && isValidRepositoryUrl(url) && QUrl(url, QUrl::StrictMode).isValid()) {
                repos << line;
                listMXUrls << url;
            }
        }
        if (!repos.isEmpty()) {
            repos.sort(Qt::CaseInsensitive);
            return;
        }
    }
}

// Compares by exact host, not substring: "mxrepo.com" must not match "la.mxrepo.com".
bool MainWindow::repoEntryMatchesHost(const QString &repoLine, const QString &host)
{
    const QString url = repoLine.section(" - ", 1, 1).trimmed();
    return QUrl(url).host().compare(host, Qt::CaseInsensitive) == 0;
}

int MainWindow::getDebianVerNum()
{
    QFile file("/etc/debian_version");
    QString line;

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        if (!in.atEnd()) {
            line = in.readLine().trimmed();
        }
        file.close();
    } else {
        qCritical() << "Could not open /etc/debian_version:" << file.errorString() << ". Defaulting to Bullseye.";
        return Version::Bullseye;
    }

    if (line.isEmpty()) {
        qCritical() << "/etc/debian_version is empty. Defaulting to Bullseye.";
        return Version::Bullseye;
    }

    static const QHash<QString, Version> versionMap {
        {"bullseye", Version::Bullseye},
        {"bookworm", Version::Bookworm},
        {"trixie", Version::Trixie}
    };

    // Check named versions first
    for (auto it = versionMap.cbegin(); it != versionMap.cend(); ++it) {
        if (line.contains(it.key(), Qt::CaseInsensitive)) {
            return it.value();
        }
    }

    // Try numeric debian_versions such as "13.1" or "12" by extracting the major number and mapping to Version.
    QRegularExpression numberPattern{R"((\d+))"};
    QRegularExpressionMatch numberMatch = numberPattern.match(line);
    if (numberMatch.hasMatch()) {
        bool ok = false;
        int majorVersion = numberMatch.captured(1).toInt(&ok);
        if (ok && majorVersion >= Version::Bullseye && majorVersion < Version::MAX) {
            return static_cast<Version>(majorVersion);
        }
    }

    qCritical() << "Unrecognized or unparseable Debian version:" << line << ". Defaulting to Bullseye.";
    return Version::Bullseye;
}

QString MainWindow::getDebianVerName(int ver)
{
    static const QHash<int, QString> versionNames {
        {Version::Bullseye, "bullseye"}, {Version::Bookworm, "bookworm"}, {Version::Trixie, "trixie"}};

    if (versionNames.contains(ver)) {
        return versionNames.value(ver);
    } else {
        qWarning() << "Error: Invalid Debian version. Defaulting to Bullseye";
        return "bullseye";
    }
}

void MainWindow::displayMXRepos(const QStringList &repos, const QString &filter)
{
    ui->listWidget->clear();
    for (const QString &repo : repos) {
        if (!filter.isEmpty() && !repo.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        QString country = repo.section('-', 0, 0).trimmed().section(',', 0, 0);
        auto *item = new QListWidgetItem(ui->listWidget);
        auto *radio = new QRadioButton(repo);
        radio->setIcon(getFlag(country));
        ui->listWidget->setItemWidget(item, radio);
        if (repoEntryMatchesHost(repo, current_repo)) {
            radio->setChecked(true);
            ui->listWidget->scrollToItem(item);
        }
        connect(radio, &QRadioButton::clicked, this, [this]() {
            radioSelectionChanged = true;
            ui->pushOk->setEnabled(true);
        });
    }
}

namespace
{
struct AptEntry {
    qsizetype line;
    qsizetype offset;
    QString text;
    bool enabled;
};

const QRegularExpression legacyEntry(R"(^[ \t]*#?[ \t]*deb[ \t]+)");

QList<AptEntry> aptEntries(const QString &content, bool deb822)
{
    QList<AptEntry> entries;
    qsizetype pos = 0, lineNumber = 0;
    qsizetype stanzaStart = -1, stanzaLine = 0, stanzaEnd = 0;
    const auto finishStanza = [&] {
        if (stanzaStart >= 0) {
            QString text = content.mid(stanzaStart, stanzaEnd - stanzaStart);
            static const QRegularExpression disabled(R"(Enabled:\s*no)", QRegularExpression::CaseInsensitiveOption);
            entries.append({stanzaLine, stanzaStart, text, !text.contains(disabled)});
            stanzaStart = -1;
        }
    };
    while (pos < content.size()) {
        qsizetype newline = content.indexOf('\n', pos);
        if (newline < 0) {
            newline = content.size();
        }
        qsizetype end = newline;
        if (end > pos && content.at(end - 1) == '\r') {
            --end;
        }
        const QString line = content.mid(pos, end - pos);
        if (deb822) {
            if (line.trimmed().isEmpty()) {
                finishStanza();
            } else {
                if (stanzaStart < 0) {
                    stanzaStart = pos;
                    stanzaLine = lineNumber;
                }
                stanzaEnd = end;
            }
        } else if (legacyEntry.match(line).hasMatch()) {
            entries.append({lineNumber, pos, line, !line.trimmed().startsWith('#')});
        }
        pos = newline + 1;
        ++lineNumber;
    }
    finishStanza();
    return entries;
}

QString fieldValue(const QString &stanza, const QString &name)
{
    QStringList values;
    bool inField = false;
    for (const QString &line : stanza.split('\n')) {
        if (line.startsWith('#') || line.isEmpty()) {
            continue;
        }
        if (!line.front().isSpace()) {
            inField = line.startsWith(name + ':', Qt::CaseInsensitive);
            if (inField) {
                values << line.mid(name.size() + 1);
            }
        } else if (inField) {
            values << line;
        }
    }
    return values.join(' ').simplified();
}

bool stanzaEnabled(const QString &stanza)
{
    const QString value = fieldValue(stanza, "Enabled").toLower();
    static const QStringList disabled {"0", "false", "no", "off", "disable", "without"};
    return !disabled.contains(value);
}

bool enableAhsSource(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QString content = QString::fromUtf8(file.readAll());
    if (file.error() != QFileDevice::NoError) {
        return false;
    }
    file.close();

    const bool deb822 = path.endsWith(".sources");
    const auto entries = aptEntries(content, deb822);
    bool found = false;
    for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
        QString text = it->text;
        if (deb822) {
            static const QRegularExpression components(R"(^Components:[ \t]*(.*)$)",
                QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
            if (!components.match(text).captured(1).simplified().split(' ').contains("ahs")) {
                continue;
            }
            static const QRegularExpression enabled(R"(^Enabled:[^\r\n]*)",
                QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
            text.replace(enabled, "Enabled: yes");
        } else {
            static const QRegularExpression source(R"(^[ \t]*#?[ \t]*deb\s+(?:\[[^\]]*\]\s+)?\S+\s+\S+\s+([^#]+))");
            if (!source.match(text).captured(1).simplified().split(' ').contains("ahs")) {
                continue;
            }
            text.remove(QRegularExpression(R"(^[ \t]*#[ \t]*)"));
        }
        found = true;
        content.replace(it->offset, it->text.size(), text);
    }
    if (!found || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    return file.write(bytes) == bytes.size() && file.flush();
}

enum EntryRole { SourcePath = Qt::UserRole, SourceLine, OriginalText, OriginallyEnabled };
} // namespace

void MainWindow::getCurrentRepo(bool force)
{
    current_repo.clear();
    QStringList hosts;
    bool readFailed = false;
    for (const auto &name : {"mx.list", "mx.sources"}) {
        QFile file(QStringLiteral("/etc/apt/sources.list.d/") + name);
        if (!file.exists()) {
            continue;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            readFailed = true;
            continue;
        }
        const bool deb822 = file.fileName().endsWith(".sources");
        const auto entries = aptEntries(QString::fromUtf8(file.readAll()), deb822);
        if (file.error() != QFileDevice::NoError) {
            readFailed = true;
            continue;
        }
        for (const auto &entry : entries) {
            if (deb822 ? !stanzaEnabled(entry.text) : !entry.enabled) {
                continue;
            }
            static const QRegularExpression legacyUri(R"(^\s*deb\s+(?:\[[^\]]*\]\s+)?(\S+))");
            const QString uris = deb822 ? fieldValue(entry.text, "URIs") : legacyUri.match(entry.text).captured(1);
            for (const auto &uri : uris.split(' ', Qt::SkipEmptyParts)) {
                const QUrl url(uri, QUrl::StrictMode);
                const QString path = url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash).path();
                if (isValidRepositoryUrl(uri) && url.isValid()
                    && (path.endsWith("/mx/repo") || path.endsWith("/mx/testrepo"))) {
                    hosts << url.host();
                }
            }
        }
    }

    const bool reload = force || repos.isEmpty();
    if (reload) {
        readMXRepos();
    }
    for (const auto &host : hosts) {
        if (std::any_of(repos.cbegin(), repos.cend(), [&](const QString &entry) {
                return repoEntryMatchesHost(entry, host);
            })) {
            current_repo = host;
            return;
        }
    }
    if (hosts.isEmpty()) {
        return;
    }
    current_repo = hosts.first();
    if (reload && !readFailed && !repos.isEmpty() && replaceRepos("http://mxrepo.com", true)) {
        current_repo = "mxrepo.com";
    }
}

void MainWindow::displayAllRepos(const QFileInfoList &apt_files)
{
    ui->treeWidget->clear();
    ui->treeWidgetDeb->clear();
    ui->treeWidget->blockSignals(true);
    ui->treeWidgetDeb->blockSignals(true);

    const QStringList columnNames {tr("Lists"), tr("Sources (checked sources are enabled)")};
    ui->treeWidget->setHeaderLabels(columnNames);
    ui->treeWidgetDeb->setHeaderLabels(columnNames);

    auto processEntries = [](QTreeWidgetItem *parentItem, const auto &entries) {
        for (const auto &item : entries) {
            auto *childItem = new QTreeWidgetItem(parentItem, {QString(), item.text});
            childItem->setData(1, SourceLine, item.line);
            childItem->setData(1, OriginalText, item.text);
            childItem->setData(1, OriginallyEnabled, item.enabled);
            childItem->setCheckState(1, item.enabled ? Qt::Checked : Qt::Unchecked);
        }
    };

    for (const QFileInfo &file_info : apt_files) {
        QString file_name = file_info.fileName();
        QString file = file_info.absoluteFilePath();
        auto *topLevelItem = new QTreeWidgetItem(ui->treeWidget, {file_name});
        topLevelItem->setFlags(topLevelItem->flags() & ~Qt::ItemIsUserCheckable);
        topLevelItem->setForeground(0, QBrush(Qt::darkGreen));
        QTreeWidgetItem *topLevelItemDeb = nullptr;

        if (file_name.contains("debian") || file_name == "sources.list") {
            topLevelItemDeb = new QTreeWidgetItem(ui->treeWidgetDeb, {file_name});
            topLevelItemDeb->setFlags(topLevelItemDeb->flags() & ~Qt::ItemIsUserCheckable);
            topLevelItemDeb->setForeground(0, QBrush(Qt::darkGreen));
        }

        topLevelItem->setData(0, SourcePath, file);
        if (topLevelItemDeb) {
            topLevelItemDeb->setData(0, SourcePath, file);
        }
        QFile source(file);
        if (!source.open(QIODevice::ReadOnly)) {
            qWarning() << "Could not read" << file << source.errorString();
            continue;
        }
        const auto entries = aptEntries(QString::fromUtf8(source.readAll()), file.endsWith(".sources"));
        processEntries(topLevelItem, entries);
        if (topLevelItemDeb) {
            processEntries(topLevelItemDeb, entries);
        }
    }

    for (auto *treeWidget : {ui->treeWidget, ui->treeWidgetDeb}) {
        for (int i = 0; i < treeWidget->columnCount(); ++i) {
            treeWidget->resizeColumnToContents(i);
        }
        treeWidget->expandAll();
        treeWidget->blockSignals(false);
    }
}

void MainWindow::cancelOperation()
{
    auto processId = shell->processId();
    if (processId != 0) {
        Cmd cmd;
        cmd.procAsRoot("kill", {});
        if (!shell->waitForFinished(1000)) {
            cmd.procAsRoot("kill", {"-9"});
        }
    }
    procDone();
}

void MainWindow::centerWindow()
{
    auto screenGeometry = QApplication::primaryScreen()->geometry();
    auto x = (screenGeometry.width() - width()) / 2;
    auto y = (screenGeometry.height() - height()) / 2;
    move(x, y);
}

void MainWindow::closeEvent([[maybe_unused]] QCloseEvent *event)
{
    settings.setValue("geometry", saveGeometry());
    event->accept();
}

void MainWindow::displaySelected(const QString &repo)
{
    for (int row = 0; row < ui->listWidget->count(); ++row) {
        auto *item = ui->listWidget->item(row);
        auto *radio = qobject_cast<QRadioButton *>(ui->listWidget->itemWidget(item));
        if (radio && radio->text().contains(repo, Qt::CaseInsensitive)) {
            radio->setChecked(true);
            ui->listWidget->scrollToItem(item);
            radioSelectionChanged = true;
            ui->pushOk->setEnabled(true);
            break;
        }
    }
}

void MainWindow::setSelected()
{
    if (!radioSelectionChanged) {
        return;
    }
    for (int row = 0; row < ui->listWidget->count(); ++row) {
        auto *item = ui->listWidget->item(row);
        if (auto *radio = qobject_cast<QRadioButton *>(ui->listWidget->itemWidget(item)); radio && radio->isChecked()) {
            QString url = radio->text().section(" - ", 1, 1).trimmed();
            replaceRepos(url);
            break;
        }
    }
}

void MainWindow::procTime()
{
    bar->setValue((bar->value() + 1) % bar->maximum());
}

void MainWindow::procStart()
{
    bar->setValue(0);
    QApplication::setOverrideCursor(QCursor(Qt::BusyCursor));
    timer.start(100ms);
    connect(&timer, &QTimer::timeout, this, &MainWindow::procTime);
}

void MainWindow::procDone()
{
    bar->setValue(bar->maximum());
    timer.stop();
    timer.disconnect();
    QApplication::restoreOverrideCursor();
}

bool MainWindow::replaceRepos(const QString &url, bool quiet)
{
    if (!isValidRepositoryUrl(url)) {
        qWarning() << "Invalid repository URL:" << url;
        return false;
    }

    const QString trimmedUrl = url.trimmed().remove(QRegularExpression("/$"));
    const QString mx_list {"/etc/apt/sources.list.d/mx.list"};
    const QString mx_sources {"/etc/apt/sources.list.d/mx.sources"};

    enum class UpdateOutcome { NotApplicable, NoChangeNeeded, Changed, Failed };

    auto tryUpdate = [&](const QString &path, bool sourcesFormat) -> UpdateOutcome {
        QFile file(path);
        if (!file.exists()) {
            return UpdateOutcome::NotApplicable;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Could not open file:" << path << file.errorString();
            return UpdateOutcome::Failed;
        }
        QTextStream in(&file);
        QString content = in.readAll();
        file.close();

        QString updated = content;
        if (sourcesFormat) {
            static const QRegularExpression uriPattern(
                R"((URIs:\s*)(\S*)(/mx/(?:[.]?/)*repo/?|/mx/(?:[.]?/)*testrepo/?))",
                QRegularExpression::CaseInsensitiveOption);
            updated.replace(uriPattern, QStringLiteral("\\1%1\\3").arg(trimmedUrl));
        } else {
            static const QRegularExpression repoPattern(
                R"((deb(?:-src)?\s+(?:\[.*?\]\s+)?)(\S*)(/mx/(?:[.]?/)*repo/?|/mx/(?:[.]?/)*testrepo/?))",
                QRegularExpression::CaseInsensitiveOption);
            updated.replace(repoPattern, QStringLiteral("\\1%1\\3").arg(trimmedUrl));
        }
        updated.replace(QRegularExpression(R"([\t ]{2,})"), " ");
        updated.replace(QRegularExpression(R"([\t ]+$)", QRegularExpression::MultilineOption), "");

        if (updated == content) {
            return UpdateOutcome::NoChangeNeeded;
        }
        return writeUpdatedFile(path, updated) ? UpdateOutcome::Changed : UpdateOutcome::Failed;
    };

    // Both formats are examined independently and unconditionally -- nothing enforces that MX
    // installs ship only one of these, so either file could be the genuinely active repo
    // declaration, or both could be (e.g. around a format migration). Checking mx.sources only
    // when mx.list didn't change would leave a real, independently-active mx.sources untouched
    // whenever mx.list also happened to need (and successfully receive) an update.
    const UpdateOutcome listOutcome = tryUpdate(mx_list, false);
    const UpdateOutcome sourcesOutcome = tryUpdate(mx_sources, true);

    const bool anyChanged = listOutcome == UpdateOutcome::Changed || sourcesOutcome == UpdateOutcome::Changed;
    const bool anyFailed = listOutcome == UpdateOutcome::Failed || sourcesOutcome == UpdateOutcome::Failed;
    const bool anyExists = listOutcome != UpdateOutcome::NotApplicable
        || sourcesOutcome != UpdateOutcome::NotApplicable;

    if (anyChanged) {
        sources_changed = true;
    }

    // A failure on either file takes priority over the other succeeding: silently reporting
    // "Success" would hide that one of two active repo declarations was left stale.
    if (anyFailed) {
        if (!quiet) {
            QMessageBox::critical(this, tr("Error"), tr("Could not change the repo."));
        }
        return false;
    }
    if (anyChanged) {
        if (!quiet) {
            QMessageBox::information(this, tr("Success"),
                                     tr("Your new selection will take effect the next time sources are updated."));
        }
        return true;
    }
    if (anyExists) {
        // Already pointing at this mirror -- not a failure, so don't show "Could not change".
        if (!quiet) {
            QMessageBox::information(this, tr("No Changes"),
                                     tr("The selected repository is already configured."));
        }
        return true;
    }
    if (!quiet) {
        QMessageBox::critical(this, tr("Error"), tr("Could not change the repo."));
    }
    return false;
}

void MainWindow::setConnections()
{
    connect(ui->lineSearch, &QLineEdit::textChanged, this, &MainWindow::lineSearch_textChanged);
    connect(ui->pushRestoreSources, &QPushButton::clicked, this, &MainWindow::pushRestoreSources_clicked);
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAbout_clicked);
    connect(ui->pushCancel, &QPushButton::clicked, this, &MainWindow::close);
    connect(ui->pushFastestDebian, &QPushButton::clicked, this, &MainWindow::pushFastestDebian_clicked);
    connect(ui->pushFastestMX, &QPushButton::clicked, this, &MainWindow::pushFastestMX_clicked);
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelp_clicked);
    connect(ui->pushOk, &QPushButton::clicked, this, &MainWindow::pushOk_clicked);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::tabWidget_currentChanged);
    connect(ui->treeWidget, &QTreeWidget::itemChanged, this, &MainWindow::treeWidget_itemChanged);
    connect(ui->treeWidgetDeb, &QTreeWidget::itemChanged, this, &MainWindow::treeWidget_itemChanged);
}

void MainWindow::setProgressBar()
{
    progress = new QProgressDialog(this);
    bar = new QProgressBar(progress);
    progCancel = new QPushButton(tr("Cancel"));
    connect(progCancel, &QPushButton::clicked, this, &MainWindow::cancelOperation);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                             | Qt::WindowStaysOnTopHint);
    progress->setCancelButton(progCancel);
    progress->setLabelText(tr("Please wait..."));
    progress->setAutoClose(false);
    progress->setBar(bar);
    bar->setTextVisible(false);
    progress->reset();
}

QFileInfoList MainWindow::listAptFiles()
{
    QFileInfoList list;
    const QDir apt_dir("/etc/apt/sources.list.d");
    list = apt_dir.entryInfoList(QStringList({"*.list", "*.sources"}), QDir::Files | QDir::Readable, QDir::Name);

    QFile file("/etc/apt/sources.list");
    if (file.exists() && file.size() > 0 && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QRegularExpression re(R"(^\s*deb)");
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (re.match(line).hasMatch()) {
                list << QFileInfo(file);
                break;
            }
        }
        file.close();
    }

    return list;
}

void MainWindow::pushOk_clicked()
{
    if (applyingChanges) {
        return;
    }
    const QScopedValueRollback guard(applyingChanges, true);
    ui->pushOk->setEnabled(false);
    ui->tabWidget->setEnabled(false);
    const auto pending = std::exchange(queued_changes, {});

    bool appliedChanges = false;
    QStringList failedFiles;
    for (auto changes = pending.cbegin(); changes != pending.cend(); ++changes) {
        QFile file(changes.key());
        if (!file.open(QIODevice::ReadOnly)) {
            failedFiles << file.fileName();
            continue;
        }
        const QByteArray bytes = file.readAll();
        QString content = QString::fromUtf8(bytes);
        if (file.error() != QFileDevice::NoError || content.toUtf8() != bytes) {
            failedFiles << file.fileName();
            continue;
        }
        const auto entries = aptEntries(content, file.fileName().endsWith(".sources"));
        qsizetype matched = 0;
        for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
            const auto edit = changes->constFind(it->line);
            if (edit != changes->cend() && it->text == edit->original) {
                content.replace(it->offset, it->text.size(), edit->replacement);
                ++matched;
            }
        }
        if (matched != changes->size() || !shell->installAsRoot(file.fileName(), content.toUtf8())) {
            failedFiles << file.fileName();
        } else {
            appliedChanges = true;
        }
    }
    if (appliedChanges) {
        sources_changed = true;
    }
    setSelected();
    refresh();
    ui->tabWidget->setEnabled(true);
    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, tr("Some Changes Not Applied"),
                             tr("These source files changed on disk or could not be written:\n%1").arg(failedFiles.join('\n')));
    }
}

void MainWindow::pushAbout_clicked()
{
    hide();
    displayAboutMsgBox(
        tr("About %1").arg(windowTitle()),
        "<p align=\"center\"><b><h2>" + windowTitle() + "</h2></b></p><p align=\"center\">" + tr("Version: ")
            + QApplication::applicationVersion() + "</p><p align=\"center\"><h3>"
            + tr("Program for choosing the default APT repository")
            + R"(</h3></p><p align="center"><a href="http://mxlinux.org">http://mxlinux.org</a><br /></p><p align="center">)"
            + tr("Copyright (c) MX Linux") + "<br /><br /></p>",
        "/usr/share/doc/mx-repo-manager/license.html", tr("%1 License").arg(windowTitle()));
    show();
}

void MainWindow::pushHelp_clicked()
{
    QLocale locale;
    QString lang = locale.bcp47Name();
    QString url = "/usr/share/doc/mx-repo-manager/mx-repo-manager.html";

    if (lang.startsWith("fr")) {
        url = "https://mxlinux.org/wiki/help-files/help-mx-gestionnaire-de-d%C3%A9p%C3%B4ts";
    }

    displayHelpDoc(url, tr("%1 Help").arg(windowTitle()));
}

void MainWindow::treeWidget_itemChanged(QTreeWidgetItem *item, int column)
{
    if (!item || !item->parent()) {
        return;
    }

    ui->pushOk->setEnabled(true);
    ui->treeWidget->blockSignals(true);
    ui->treeWidgetDeb->blockSignals(true);

    const QString text = item->data(column, OriginalText).toString();
    const QString filePath = item->parent()->data(0, SourcePath).toString();
    const qsizetype line = item->data(column, SourceLine).toLongLong();
    const bool enabled = item->checkState(column) == Qt::Checked;
    const bool changed = enabled != item->data(column, OriginallyEnabled).toBool();
    QString new_text = text;

    if (text.contains("/mx/testrepo/") && item->checkState(column) == Qt::Checked) {
        QMessageBox::warning(this, tr("Warning"),
                             tr("You have selected MX Test Repo. It's not recommended to leave it enabled or to "
                                "upgrade all the packages from it.")
                                 + "\n\n"
                                 + tr("A safer option is to install packages individually with MX Package "
                                      "Installer."));
    }

    if (filePath.endsWith(".sources")) {
        if (item->checkState(column) == Qt::Checked) {
            new_text.replace(QRegularExpression("Enabled:\\s*no", QRegularExpression::CaseInsensitiveOption),
                             "Enabled: yes");
        } else {
            if (!new_text.contains(QRegularExpression("Enabled:", QRegularExpression::CaseInsensitiveOption))) {
                new_text.append("\nEnabled: no");
            } else {
                new_text.replace(QRegularExpression("Enabled:\\s*yes", QRegularExpression::CaseInsensitiveOption),
                                 "Enabled: no");
            }
        }
    } else {
        if (item->checkState(column) == Qt::Checked) {
            new_text.remove(QRegularExpression("#\\s*"));
        } else {
            new_text.prepend("# ");
        }
    }

    if (changed) {
        queued_changes[filePath].insert(line, {text, new_text});
    } else {
        new_text = text;
        auto file = queued_changes.find(filePath);
        if (file != queued_changes.end()) {
            file->remove(line);
            if (file->isEmpty()) {
                queued_changes.erase(file);
            }
        }
    }
    item->setText(column, new_text);
    QTreeWidget *other = item->treeWidget() == ui->treeWidget ? ui->treeWidgetDeb : ui->treeWidget;
    for (int i = 0; i < other->topLevelItemCount(); ++i) {
        auto *parent = other->topLevelItem(i);
        if (parent->data(0, SourcePath).toString() == filePath) {
            auto *twin = parent->child(item->parent()->indexOfChild(item));
            twin->setText(column, new_text);
            twin->setCheckState(column, item->checkState(column));
            break;
        }
    }
    ui->pushOk->setEnabled(radioSelectionChanged || !queued_changes.isEmpty());

    ui->treeWidgetDeb->blockSignals(false);
    ui->treeWidget->blockSignals(false);
}

void MainWindow::tabWidget_currentChanged()
{
    if (ui->tabWidget->currentWidget() == ui->tabMX) {
        ui->label->setText(tr("Select the APT repository that you want to use:"));
    } else {
        ui->label->setText(tr("Select the APT repository and sources that you want to use:"));
    }
}

// Transform "country" name to 2-3 letter ISO 3166 country code and provide the QIcon for it
QIcon MainWindow::getFlag(QString country)
{
    static const QMap<QString, QString> specialCases = {{"The Netherlands", "Netherlands"},
                                                        {"USA", "UnitedStates"},
                                                        {"Anycast", "any"},
                                                        {"Any", "any"},
                                                        {"World", "any"}};

    if (specialCases.contains(country)) {
        if (country == "Anycast" || country == "Any" || country == "World") {
            return QIcon("/usr/share/flags-common/any.png");
        }
        country = specialCases[country];
    }

    QMetaEnum metaEnum = QMetaEnum::fromType<QLocale::Country>();
    int index = metaEnum.keyToValue(country.remove(' ').toUtf8());
    if (index == -1) {
        return {};
    }

    QList<QLocale> locales
        = QLocale::matchingLocales(QLocale::AnyLanguage, QLocale::AnyScript, static_cast<QLocale::Country>(index));
    if (!locales.isEmpty()) {
        QString short_name = locales.at(0).name().section('_', 1, 1).toLower();
        return QIcon("/usr/share/flags-common/" + short_name + ".png");
    }
    return {};
}

// Detect fastest Debian repo
void MainWindow::pushFastestDebian_clicked()
{
    if (!shell->procAsRoot("true", {}, nullptr, nullptr, QuietMode::Yes)) {
        QMessageBox::critical(this, tr("Error"), tr("Could not detect fastest repo."));
        return;
    }
    progress->show();

    QString ver_name = getDebianVerName(getDebianVerNum());
    if (ver_name == "buster" || ver_name == "bullseye") {
        ver_name.clear(); // netselect-apt doesn't like name buster/bullseye for some reason,
                          // maybe it expects "stable"
    }

    QStringList args;
    if (!ver_name.isEmpty()) {
        args << ver_name;
    }

    QString output;
    bool success = shell->procAsRoot("netselect-apt", args, &output);
    progress->hide();

    if (!success) {
        QMessageBox::critical(this, tr("Error"), tr("netselect-apt could not detect fastest repo."));
        return;
    }
    QString repo;
    const QStringList lines = output.split('\n');
    for (const QString &line : lines) {
        if (line.startsWith("deb ")) {
            repo = line.section(' ', 1, 1).trimmed();
            break;
        }
    }
    if (checkRepo(repo)) {
        replaceDebianRepos(repo);
        refresh();
    } else {
        QMessageBox::critical(this, tr("Error"), tr("Could not detect fastest repo."));
    }
}

// Detect and select the fastest MX repo
void MainWindow::pushFastestMX_clicked()
{
    if (!shell->procAsRoot("true", {}, nullptr, nullptr, QuietMode::Yes)) {
        QMessageBox::critical(this, tr("Error"), tr("Could not detect fastest repo."));
        return;
    }
    progress->show();
    QString out;
    bool success = shell->procAsRoot("netselect", QStringList {"-D", "-I"} + listMXUrls, &out);
    out = out.trimmed();
    // netselect's result ("<score> <url>") and its "unknown host" diagnostics for unreachable
    // mirrors both land in this same buffer (stdout and stderr merged), with no guaranteed
    // ordering between the two, so the result line isn't reliably the last one. Identify it by
    // shape instead of position, keeping the last match in case more than one line qualifies.
    static const QRegularExpression resultLine(R"(^\d+\s+(\S+)$)");
    QString selectedRepo;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = resultLine.match(line.simplified());
        if (match.hasMatch()) {
            selectedRepo = match.captured(1);
        }
    }
    qDebug() << "FASTEST " << success << out;
    progress->hide();
    if (success && !selectedRepo.isEmpty()) {
        displaySelected(selectedRepo);
        pushOk_clicked();
    } else {
        QMessageBox::critical(this, tr("Error"), tr("Could not detect fastest repo."));
    }
}

void MainWindow::lineSearch_textChanged(const QString &arg1)
{
    displayMXRepos(repos, arg1);
}

void MainWindow::pushRestoreSources_clicked()
{
    // Check if running on antiX/MX
    if (!QFileInfo::exists("/etc/antix-version") && !QFileInfo::exists("/etc/mx-version")) {
        QMessageBox::critical(this, tr("Error"), tr("Can't figure out if this app is running on antiX or MX"));
        return;
    }

    bool ok = true;
    int mx_version = shell->getOut("grep -oP '(?<=DISTRIB_RELEASE=).*' /etc/lsb-release").left(2).toInt(&ok);
    if (!ok || mx_version < 18) {
        QMessageBox::critical(this, tr("Error"),
                              tr("MX version not detected or out of range: ") + QString::number(mx_version));
        return;
    }

    QTemporaryDir tmpdir;
    if (!tmpdir.isValid()) {
        qDebug() << "Could not create temp dir";
        return;
    }
    // Download source files from
    const QString url = QString("https://codeload.github.com/MX-Linux/mx-sources/zip/mx%1").arg(mx_version);
    QFile tofile(tmpdir.path() + "/" + QFileInfo(url).fileName() + ".zip");
    if (!downloadFile(url, &tofile)) {
        QMessageBox::critical(this, tr("Error"), tr("Could not download original APT files."));
        return;
    }

    // Extract master.zip to temp folder
    QString cmd = QString("unzip -q %1 -d %2")
                      .arg(Cmd::shellQuote(tofile.fileName()), Cmd::shellQuote(tmpdir.path()));
    if (!tofile.exists() || !shell->run(cmd)) {
        QMessageBox::critical(this, tr("Error"), tr("Could not unzip downloaded file."));
        return;
    }

    const QString aptUrisOut = shell->getOut("apt-get", {"update", "--print-uris"}, QuietMode::Yes);
    static const QRegularExpression ahsRx(QStringLiteral("/mx/([.]?/)*repo/.*/ahs/binary-amd64/Packages"));
    static const QRegularExpression mxRx(QStringLiteral("/mx/([.]?/)*repo/.*/main/binary-amd64/Packages"));

    bool enable_ahs = false;
    // For newer versions and 64-bit OS check if AHS was enabled
    if (mx_version >= 19 && shell->getOut("uname", {"-m"}, QuietMode::Yes).trimmed() == "x86_64") {
        if (ahsRx.match(aptUrisOut).hasMatch()) {
            enable_ahs = true;
            qDebug() << "AHS repo detected:" << enable_ahs;
        }
    }

    const bool enable_mx = mxRx.match(aptUrisOut).hasMatch();

    QString mx_list = QString("%1/mx-sources-mx%2/mx.list").arg(tmpdir.path(), QString::number(mx_version));
    QString mx_sources = QString("%1/mx-sources-mx%2/mx.sources").arg(tmpdir.path(), QString::number(mx_version));

    const QString file = QFile::exists(mx_list) ? mx_list : mx_sources;
    if (!enable_ahs && mx_version >= 19 && !enable_mx
        && shell->getOut("uname", {"-m"}, QuietMode::Yes).trimmed() == "x86_64") {
        enable_ahs = QMessageBox::Yes == QMessageBox::question(
            this, tr("Enabling AHS"), tr("Do you use AHS (Advanced Hardware Stack) repo?"));
    }
    if (enable_ahs && !enableAhsSource(file)) {
        QMessageBox::critical(this, tr("Error"), tr("Could not enable AHS in the downloaded MX sources."));
        return;
    }

    // Move the sources list files from the temporary directory to /etc/apt/sources.list.d/
    const QDir restoredSourcesDir(QString("%1/mx-sources-mx%2").arg(tmpdir.path(), QString::number(mx_version)));
    const QFileInfoList restoredFiles
        = restoredSourcesDir.entryInfoList({"*.list", "*.sources"}, QDir::Files | QDir::Readable, QDir::Name);

    const QDir backupDir("/etc/apt/sources.list.d/backups");
    if (!backupDir.exists() && !Cmd().procAsRoot("mkdir", {"-p", backupDir.path()})) {
        qWarning() << "Failed to create backup directory:" << backupDir.path();
        QMessageBox::critical(this, tr("Error"), tr("Could not restore the original APT source files."));
        return;
    }

    bool restoreSuccess = !restoredFiles.isEmpty();
    QStringList installedTargets;
    QHash<QString, QString> targetBackups; // only present for targets that existed before this restore

    for (const QFileInfo &sourceInfo : restoredFiles) {
        const QString targetPath = QString("/etc/apt/sources.list.d/%1").arg(sourceInfo.fileName());

        if (QFile::exists(targetPath)) {
            const QString backupFilePath = backupDir.absoluteFilePath(
                sourceInfo.fileName() + "." + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
            if (!Cmd().procAsRoot("cp", {targetPath, backupFilePath})) {
                qWarning() << "Failed to backup" << targetPath;
                restoreSuccess = false;
                break;
            }
            targetBackups.insert(targetPath, backupFilePath);
        }

        QFile sourceFile(sourceInfo.absoluteFilePath());
        if (!sourceFile.open(QIODevice::ReadOnly)) {
            qWarning() << "Could not read restored file:" << sourceInfo.absoluteFilePath();
            restoreSuccess = false;
            break;
        }
        const QByteArray content = sourceFile.readAll();
        sourceFile.close();

        restoreSuccess = shell->installAsRoot(targetPath, content, true);
        if (!restoreSuccess) {
            break;
        }
        installedTargets << targetPath;
    }

    if (!restoreSuccess && !installedTargets.isEmpty()) {
        qWarning() << "Rolling back" << installedTargets.size() << "restored file(s) after failure";
        for (const QString &targetPath : std::as_const(installedTargets)) {
            const auto backupIt = targetBackups.constFind(targetPath);
            if (backupIt != targetBackups.constEnd()) {
                if (!Cmd().procAsRoot("cp", {backupIt.value(), targetPath})) {
                    qWarning() << "Failed to roll back" << targetPath;
                }
            } else if (!Cmd().procAsRoot("rm", {"-f", targetPath})) {
                qWarning() << "Failed to remove newly-restored file during rollback:" << targetPath;
            }
        }
    }

    if (restoreSuccess) {
        refresh(true);
        QMessageBox::information(this, tr("Success"),
                                 tr("Original APT sources have been restored to the release status. User added "
                                    "source files in /etc/apt/sources.list.d/ have not been touched.")
                                     + "\n\n"
                                     + tr("Your new selection will take effect the next time sources are updated."));
        sources_changed = true;
    } else {
        QMessageBox::critical(this, tr("Error"), tr("Could not restore the original APT source files."));
    }
}

bool MainWindow::isValidRepositoryUrl(const QString &url)
{
    if (url.trimmed().isEmpty()) {
        return false;
    }

    QUrl qurl(url.trimmed());
    if (!qurl.isValid() || qurl.host().isEmpty()) {
        return false;
    }

    // Only allow http, https, and ftp schemes
    const QString scheme = qurl.scheme().toLower();
    if (scheme != "http" && scheme != "https" && scheme != "ftp") {
        return false;
    }

    // Basic length check (reasonable URL length)
    if (url.length() > 2048) {
        return false;
    }

    return true;
}

void MainWindow::setupNetworkProxy(QNetworkAccessManager &manager, const QUrl &url)
{
    QNetworkProxyQuery query {url};
    QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
    if (!proxies.isEmpty()) {
        manager.setProxy(proxies.first());
    }
}

bool MainWindow::checkRepo(const QString &repo)
{
    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader(
        "User-Agent",
        QString("%1/%2 (linux-gnu)").arg(QApplication::applicationName(), QApplication::applicationVersion()).toUtf8());

    QNetworkAccessManager manager;
    setupNetworkProxy(manager, QUrl(repo));

    request.setUrl(QUrl(repo));
    QNetworkReply *reply = manager.head(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::errorOccurred, &loop, &QEventLoop::quit);
    manager.setTransferTimeout(5000);
    loop.exec();
    reply->disconnect();

    if (reply->error() == QNetworkReply::NoError) {
        return true;
    }

    qDebug() << "No response from repo:" << reply->url() << reply->error();
    return false;
}

bool MainWindow::downloadFile(const QString &url, QFile *file, std::chrono::seconds timeout)
{
    if (!file->open(QIODevice::WriteOnly)) {
        qWarning() << "Could not open file:" << file->fileName() << file->errorString();
        return false;
    }

    QNetworkAccessManager manager;
    setupNetworkProxy(manager, QUrl(url));

    QNetworkRequest request {QUrl(url)};
    request.setRawHeader(
        "User-Agent",
        QString("%1/%2 (linux-gnu)").arg(QApplication::applicationName(), QApplication::applicationVersion()).toUtf8());

    manager.setTransferTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());

    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;

    connect(reply, &QNetworkReply::readyRead, this, [&file, reply]() {
        if (file->write(reply->readAll()) == -1) {
            qWarning() << "Failed to write data to file:" << file->fileName();
            reply->abort();
            file->close();
            file->remove();
        }
    });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::errorOccurred, &loop, &QEventLoop::quit);

    loop.exec();
    file->close();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Failed to download file:" << url << reply->errorString();
        file->remove();
        return false;
    }
    return true;
}
