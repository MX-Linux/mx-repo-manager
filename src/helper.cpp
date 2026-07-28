/**********************************************************************
 *  helper.cpp
 **********************************************************************
 * Copyright (C) 2015-2026 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *          OpenAI Codex
 *
 * This file is part of mx-repo-manager.
 *
 * mx-repo-manager is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 **********************************************************************/

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
// Safety net against a genuinely hung privileged process. waitForFinished() returns as soon as
// the process exits, so this only bounds a stuck process; legitimate long operations such as
// netselect/netselect-apt mirror testing and apt-get update should finish well within it. Normal
// user-initiated interruption is handled GUI-side via MainWindow::cancelOperation().
constexpr int kProcessTimeoutMs = 300000; // 5 minutes

struct ProcessResult
{
    bool started = false;
    int exitCode = 1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QByteArray standardOutput;
    QByteArray standardError;
};

void writeAndFlush(FILE *stream, const QByteArray &data)
{
    if (!data.isEmpty()) {
        std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stream);
        std::fflush(stream);
    }
}

void printError(const QString &message)
{
    writeAndFlush(stderr, message.toUtf8() + '\n');
}

[[nodiscard]] QByteArray readHelperInput()
{
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) {
        return {};
    }
    return input.readAll();
}

[[nodiscard]] const QHash<QString, QStringList> &allowedCommands()
{
    static const QHash<QString, QStringList> commands {
        {"apt-get", {"/usr/bin/apt-get", "/bin/apt-get"}},
        {"cp", {"/usr/bin/cp", "/bin/cp"}},
        {"mkdir", {"/usr/bin/mkdir", "/bin/mkdir"}},
        {"netselect", {"/usr/bin/netselect"}},
        {"netselect-apt", {"/usr/bin/netselect-apt"}},
        {"true", {"/usr/bin/true", "/bin/true"}},
    };
    return commands;
}

[[nodiscard]] bool isCleanAbsolutePath(const QString &path)
{
    return path.startsWith(QLatin1Char('/')) && QDir::cleanPath(path) == path;
}

// The fixed set of APT source files this app manages, plus any *.list/*.sources file living
// directly inside sources.list.d (mirrors what the GUI enumerates) -- never in a subdirectory.
[[nodiscard]] bool isManagedSourceFile(const QString &path)
{
    static const QStringList fixedFiles {
        "/etc/apt/sources.list",
        "/etc/apt/sources.list.d/debian.list",
        "/etc/apt/sources.list.d/debian.sources",
        "/etc/apt/sources.list.d/debian-stable-updates.list",
        "/etc/apt/sources.list.d/debian-stable-updates.sources",
    };
    static const QString sourcesListDir = QStringLiteral("/etc/apt/sources.list.d/");

    if (!isCleanAbsolutePath(path)) {
        return false;
    }
    if (fixedFiles.contains(path)) {
        return true;
    }
    if (!path.startsWith(sourcesListDir)) {
        return false;
    }
    const QString rest = path.mid(sourcesListDir.size());
    return !rest.isEmpty() && !rest.contains(QLatin1Char('/'))
        && (rest.endsWith(".list") || rest.endsWith(".sources"));
}

// A per-file backup living directly inside sources.list.d/backups.
[[nodiscard]] bool isBackupFile(const QString &path)
{
    static const QString backupDir = QStringLiteral("/etc/apt/sources.list.d/backups/");

    if (!isCleanAbsolutePath(path) || !path.startsWith(backupDir)) {
        return false;
    }
    const QString rest = path.mid(backupDir.size());
    return !rest.isEmpty() && !rest.contains(QLatin1Char('/'));
}

// A caller can only ever point us at a file it already owns, never redirect a privileged write
// at a file belonging to another user or root via a pre-planted path. pkexec exports PKEXEC_UID
// as the uid of the user who invoked it; if it's absent we're already running as root directly
// (no elevation boundary to enforce).
[[nodiscard]] bool isOwnedByInvokingUser(const QString &path)
{
    const QByteArray pkexecUid = qgetenv("PKEXEC_UID");
    if (pkexecUid.isEmpty()) {
        return true;
    }
    bool ok = false;
    const uint expectedUid = pkexecUid.toUInt(&ok);
    return ok && QFileInfo(path).ownerId() == expectedUid;
}

// A file directly under the system temp directory that the invoking user already owns and that
// isn't a symlink -- so a privileged write through it (netselect-apt's "-o") can't be redirected
// to a file elsewhere on the system.
[[nodiscard]] bool isTempFile(const QString &path)
{
    const QString tempDir = QDir::tempPath() + QLatin1Char('/');

    if (!isCleanAbsolutePath(path) || !path.startsWith(tempDir)) {
        return false;
    }
    const QString rest = path.mid(tempDir.size());
    if (rest.isEmpty() || rest.contains(QLatin1Char('/'))) {
        return false;
    }
    const QFileInfo info(path);
    return !info.isSymLink() && isOwnedByInvokingUser(path);
}

[[nodiscard]] QString pidFilePath()
{
    return QStringLiteral("/run/mx-repo-manager.pid");
}

[[nodiscard]] qint64 readTrackedPid()
{
    QFile file(pidFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    bool ok = false;
    const qint64 pid = QString::fromUtf8(file.readAll()).trimmed().toLongLong(&ok);
    return ok ? pid : -1;
}

// Written via a temp file + atomic rename so a concurrent readTrackedPid() (from another
// in-flight helper invocation, e.g. a second app instance) never sees a torn/partial write.
void writePidFile(qint64 pid)
{
    const QString tmpPath = pidFilePath() + QStringLiteral(".new");
    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QByteArray::number(pid));
    file.close();
    std::rename(QFile::encodeName(tmpPath).constData(), QFile::encodeName(pidFilePath()).constData());
}

// Only clears the tracked pid if it's still ours: two elevated long-running operations can
// overlap (e.g. two instances of this app), and the one that finishes first must not erase the
// other's still-valid tracking entry.
void removePidFileIfMatches(qint64 pid)
{
    if (readTrackedPid() == pid) {
        QFile::remove(pidFilePath());
    }
}

// Extra guard against the tracked pid having exited and been reused for an unrelated process by
// the time "kill" runs.
[[nodiscard]] bool isTrackedProcessAllowed(qint64 pid)
{
    static const QStringList allowedNames {"apt-get", "netselect", "netselect-apt"};
    const QFileInfo exeLink(QString("/proc/%1/exe").arg(pid));
    return allowedNames.contains(QFileInfo(exeLink.symLinkTarget()).fileName());
}

// Validate that the requested arguments match one of the specific operations this app ever
// performs, so an authenticated pkexec session cannot be reused to run e.g.
// "cp <anything> <anywhere>" or kill an arbitrary process -- only the program name was checked
// before.
[[nodiscard]] bool validateArgs(const QString &command, const QStringList &args)
{
    if (command == "apt-get") {
        return args == QStringList {"update"};
    }
    if (command == "true") {
        return args.isEmpty();
    }
    if (command == "mkdir") {
        return args == QStringList {"-p", QStringLiteral("/etc/apt/sources.list.d/backups")};
    }
    if (command == "cp") {
        if (args.size() != 2) {
            return false;
        }
        const QString &src = args.at(0);
        const QString &dst = args.at(1);
        if (isManagedSourceFile(src) && isBackupFile(dst)) {
            return true; // taking a fresh, uniquely-named backup
        }
        // Rollback direction: only ever restores a file this app already knows about, never
        // conjures a brand-new source file from a backup's content.
        return isBackupFile(src) && isManagedSourceFile(dst) && QFileInfo::exists(dst);
    }
    if (command == "kill") {
        // The target pid is never taken from here -- see handleCancel(). This only validates the
        // signal-choice flag.
        return args.isEmpty() || args == QStringList {"-9"};
    }
    if (command == "netselect-apt") {
        if (args.size() < 2 || args.size() > 3 || args.at(args.size() - 2) != "-o") {
            return false;
        }
        if (args.size() == 3) {
            static const QRegularExpression releaseName("^[A-Za-z0-9._-]+$");
            if (!releaseName.match(args.constFirst()).hasMatch()) {
                return false;
            }
        }
        return isTempFile(args.constLast());
    }
    if (command == "netselect") {
        return args.size() >= 2 && args.at(0) == "-D" && args.at(1) == "-I";
    }
    return false;
}

[[nodiscard]] QString resolveBinary(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isExecutable()) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] ProcessResult runProcess(const QString &program, const QStringList &args, const QByteArray &input,
                                       bool trackForCancel)
{
    ProcessResult result;

    QProcess process;
    process.start(program, args, QIODevice::ReadWrite);
    if (!process.waitForStarted()) {
        result.standardError = QString("Failed to start %1").arg(program).toUtf8();
        result.exitCode = 127;
        return result;
    }

    result.started = true;
    if (trackForCancel) {
        writePidFile(process.processId());
    }

    if (!input.isEmpty()) {
        process.write(input);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        result.standardOutput = process.readAllStandardOutput();
        result.standardError = process.readAllStandardError()
            + QString("\nCommand timed out after %1 seconds and was killed: %2\n")
                  .arg(kProcessTimeoutMs / 1000)
                  .arg(program)
                  .toUtf8();
        result.exitStatus = QProcess::CrashExit;
        result.exitCode = 124; // conventional timeout exit code
        if (trackForCancel) {
            removePidFileIfMatches(process.processId());
        }
        return result;
    }

    if (trackForCancel) {
        removePidFileIfMatches(process.processId());
    }

    result.exitStatus = process.exitStatus();
    result.exitCode = process.exitCode();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
}

[[nodiscard]] int relayResult(const ProcessResult &result)
{
    writeAndFlush(stdout, result.standardOutput);
    writeAndFlush(stderr, result.standardError);
    if (!result.started) {
        return result.exitCode;
    }
    return result.exitStatus == QProcess::NormalExit ? result.exitCode : 1;
}

// Signals only the pid this helper itself most recently spawned for a long-running, cancellable
// command, never a caller-supplied pid -- so invoking "kill" can, at worst, cancel the app's own
// in-flight operation.
[[nodiscard]] int handleCancel(const QStringList &args)
{
    const bool force = args.contains(QStringLiteral("-9"));
    const qint64 pid = readTrackedPid();
    if (pid <= 0) {
        printError(QStringLiteral("No cancellable operation is currently running"));
        return 1;
    }
    if (!isTrackedProcessAllowed(pid)) {
        printError(QStringLiteral("Tracked process is no longer valid"));
        return 1;
    }
    if (::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) != 0) {
        const int savedErrno = errno;
        printError(QString("Failed to signal process %1: %2").arg(pid).arg(QString::fromLocal8Bit(std::strerror(savedErrno))));
        return 1;
    }
    return 0;
}

[[nodiscard]] int runAllowedCommand(const QString &command, const QStringList &args, const QByteArray &input = {})
{
    if (command == "kill") {
        if (!validateArgs(command, args)) {
            printError(QStringLiteral("Arguments not allowed for command: kill"));
            return 127;
        }
        return handleCancel(args);
    }

    const auto commandIt = allowedCommands().constFind(command);
    if (commandIt == allowedCommands().constEnd()) {
        printError(QString("Command is not allowed: %1").arg(command));
        return 127;
    }

    if (!validateArgs(command, args)) {
        printError(QString("Arguments not allowed for command: %1").arg(command));
        return 127;
    }

    const QString program = resolveBinary(commandIt.value());
    if (program.isEmpty()) {
        printError(QString("Command is not available: %1").arg(command));
        return 127;
    }

    const bool trackForCancel = command == QLatin1String("apt-get") || command == QLatin1String("netselect")
        || command == QLatin1String("netselect-apt");
    return relayResult(runProcess(program, args, input, trackForCancel));
}

[[nodiscard]] int handleExec(const QStringList &args)
{
    if (args.isEmpty()) {
        printError(QStringLiteral("exec requires a command name"));
        return 1;
    }
    return runAllowedCommand(args.constFirst(), args.mid(1), readHelperInput());
}

// Writes new content for a managed APT source file directly as root: no caller-supplied path is
// ever trusted as the source of file content, only the target location and the bytes received
// over our own stdin (which nothing but our direct parent process can feed).
[[nodiscard]] int handleInstall(const QStringList &args, const QByteArray &content)
{
    if (args.size() != 1) {
        printError(QStringLiteral("install requires exactly one target path"));
        return 1;
    }

    const QString &targetPath = args.constFirst();
    if (!isManagedSourceFile(targetPath)) {
        printError(QString("Refusing to install to disallowed path: %1").arg(targetPath));
        return 1;
    }

    const QString tmpPath = targetPath + QStringLiteral(".mxrm-new");
    {
        QFile tmpFile(tmpPath);
        if (!tmpFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            printError(QString("Could not create %1: %2").arg(tmpPath, tmpFile.errorString()));
            return 1;
        }
        if (tmpFile.write(content) != content.size()) {
            printError(QString("Could not write %1: %2").arg(tmpPath, tmpFile.errorString()));
            tmpFile.close();
            QFile::remove(tmpPath);
            return 1;
        }
    } // flush + close via scope exit

    const QByteArray tmpPathLocal = QFile::encodeName(tmpPath);
    if (::chmod(tmpPathLocal.constData(), 0644) != 0 || ::chown(tmpPathLocal.constData(), 0, 0) != 0) {
        const int savedErrno = errno;
        printError(QString("Could not set ownership/permissions on %1: %2")
                       .arg(tmpPath, QString::fromLocal8Bit(std::strerror(savedErrno))));
        QFile::remove(tmpPath);
        return 1;
    }

    const QByteArray targetPathLocal = QFile::encodeName(targetPath);
    if (std::rename(tmpPathLocal.constData(), targetPathLocal.constData()) != 0) {
        const int savedErrno = errno;
        printError(
            QString("Could not install %1: %2").arg(targetPath, QString::fromLocal8Bit(std::strerror(savedErrno))));
        QFile::remove(tmpPath);
        return 1;
    }
    return 0;
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) {
        printError(QStringLiteral("Missing helper action"));
        return 1;
    }

    const QString action = args.constFirst();
    const QStringList remainingArgs = args.mid(1);

    if (action == QLatin1String("exec")) {
        return handleExec(remainingArgs);
    }
    if (action == QLatin1String("install")) {
        return handleInstall(remainingArgs, readHelperInput());
    }

    printError(QString("Unsupported helper action: %1").arg(action));
    return 1;
}
