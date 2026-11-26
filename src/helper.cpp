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

#include "common.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <poll.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>

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
        {"rm", {"/usr/bin/rm", "/bin/rm"}},
        {"true", {"/usr/bin/true", "/bin/true"}},
    };
    return commands;
}

[[nodiscard]] bool isCleanAbsolutePath(const QString &path)
{
    return path.startsWith(QLatin1Char('/')) && QDir::cleanPath(path) == path;
}

// The fixed, always-present Debian source files. rm should never be able to remove any of
// these -- it's only ever used to undo a just-created MX-specific restore file.
[[nodiscard]] bool isFixedSourceFile(const QString &path)
{
    static const QStringList fixedFiles {
        "/etc/apt/sources.list",
        "/etc/apt/sources.list.d/debian.list",
        "/etc/apt/sources.list.d/debian.sources",
        "/etc/apt/sources.list.d/debian-stable-updates.list",
        "/etc/apt/sources.list.d/debian-stable-updates.sources",
    };
    return fixedFiles.contains(path);
}

// The fixed set of APT source files this app manages, plus any *.list/*.sources file living
// directly inside sources.list.d (mirrors what the GUI enumerates) -- never in a subdirectory.
[[nodiscard]] bool isManagedSourceFile(const QString &path)
{
    static const QString sourcesListDir = QStringLiteral("/etc/apt/sources.list.d/");

    if (!isCleanAbsolutePath(path)) {
        return false;
    }
    if (isFixedSourceFile(path)) {
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
    if (command == "netselect") {
        return args.size() >= 2 && args.at(0) == "-D" && args.at(1) == "-I";
    }
    if (command == "rm") {
        // Only ever used to undo a just-created MX-specific restore file on rollback. Never one of
        // the fixed Debian source files -- restore never creates those, so rm should never be able
        // to remove one even if invoked directly rather than via the actual restore/rollback flow.
        return args.size() == 2 && args.at(0) == "-f" && isManagedSourceFile(args.at(1))
            && !isFixedSourceFile(args.at(1));
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

// Only the process that launched this helper holds the write end of its control pipe.
bool cancelRequested()
{
    pollfd control {STDIN_FILENO, POLLIN, 0};
    if (::poll(&control, 1, 0) <= 0) {
        return false;
    }
    if (control.revents & POLLHUP) {
        return true;
    }
    if (!(control.revents & POLLIN)) {
        return false;
    }
    char byte = 0;
    return ::read(STDIN_FILENO, &byte, 1) == 1 && byte == 'C';
}

[[nodiscard]] ProcessResult runProcess(const QString &program, const QStringList &args, bool monitorCancel)
{
    ProcessResult result;
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.setChildProcessModifier([] {
        if (::setpgid(0, 0) == -1) {
            ::_exit(1);
        }
    });
    process.start(program, args, QIODevice::ReadOnly);
    if (!process.waitForStarted()) {
        result.standardError = QString("Failed to start %1").arg(program).toUtf8();
        return result;
    }

    result.started = true;
    const auto leader = static_cast<pid_t>(process.processId());
    QDeadlineTimer deadline(kProcessTimeoutMs);
    while (process.state() != QProcess::NotRunning) {
        const bool cancelled = monitorCancel && cancelRequested();
        if (cancelled || deadline.hasExpired()) {
            // The leader remains our unreaped child throughout escalation, so the group ID cannot
            // be recycled even when a shell exits before a child that ignored SIGTERM.
            ::kill(-leader, SIGTERM);
            QDeadlineTimer grace(1000);
            while (!grace.hasExpired()) {
                ::poll(nullptr, 0, static_cast<int>(grace.remainingTime()));
            }
            ::kill(-leader, SIGKILL);
            process.waitForFinished(-1);
            result.standardError = cancelled ? QByteArray("Operation cancelled\n") : QByteArray("Command timed out\n");
            result.exitCode = cancelled ? 130 : 124;
            break;
        }
        const int remaining = static_cast<int>(deadline.remainingTime());
        process.waitForFinished(monitorCancel ? std::min(remaining, 100) : remaining);
    }
    if (result.standardError.isEmpty()) {
        result.exitStatus = process.exitStatus();
        result.exitCode = process.exitCode();
    }
    result.standardOutput = process.readAllStandardOutput();
    result.standardError += process.readAllStandardError();
    return result;
}

[[nodiscard]] int relayResult(const ProcessResult &result)
{
    writeAndFlush(stdout, result.standardOutput);
    writeAndFlush(stderr, result.standardError);
    if (!result.started) {
        return result.exitCode;
    }
    // The GUI interprets 126/127 as pkexec failures. A command run by this helper
    // must not impersonate an authentication cancellation or elevation error.
    if (result.exitCode == 126 || result.exitCode == 127) {
        return 1;
    }
    return result.exitStatus == QProcess::NormalExit ? result.exitCode : 1;
}

// Runs netselect-apt with an output destination the helper creates and owns itself, never one
// supplied by the caller: a freshly mkstemp()-style created file can't be a pre-planted symlink,
// unlike a caller-supplied "-o" path that could be swapped for one between validation and use.
// The resulting content is relayed back over our own stdout instead.
[[nodiscard]] int handleNetselectApt(const QStringList &args, bool monitorCancel)
{
    if (args.size() > 1) {
        printError(QStringLiteral("Arguments not allowed for command: netselect-apt"));
        return 1;
    }
    if (args.size() == 1) {
        static const QRegularExpression releaseName("^[A-Za-z0-9._-]+$");
        if (!releaseName.match(args.constFirst()).hasMatch()) {
            printError(QStringLiteral("Arguments not allowed for command: netselect-apt"));
            return 1;
        }
    }

    const QString program = resolveBinary(allowedCommands().value(QStringLiteral("netselect-apt")));
    if (program.isEmpty()) {
        printError(QStringLiteral("Command is not available: netselect-apt"));
        return 1;
    }

    QTemporaryFile outputFile;
    outputFile.setAutoRemove(false);
    if (!outputFile.open()) {
        printError(QStringLiteral("Could not create a temporary output file"));
        return 1;
    }
    const QString outputPath = outputFile.fileName();
    outputFile.close();

    QStringList fullArgs = args;
    fullArgs << "-o" << outputPath;

    ProcessResult result = runProcess(program, fullArgs, monitorCancel);
    if (result.started && result.exitStatus == QProcess::NormalExit && result.exitCode == 0) {
        QFile output(outputPath);
        result.standardOutput = output.open(QIODevice::ReadOnly) ? output.readAll() : QByteArray();
        result.standardError.clear();
    }
    QFile::remove(outputPath);
    return relayResult(result);
}

[[nodiscard]] int runAllowedCommand(const QString &command, const QStringList &args, bool monitorCancel)
{
    if (command == "netselect-apt") {
        return handleNetselectApt(args, monitorCancel);
    }

    const auto commandIt = allowedCommands().constFind(command);
    if (commandIt == allowedCommands().constEnd()) {
        printError(QString("Command is not allowed: %1").arg(command));
        return 1;
    }

    if (!validateArgs(command, args)) {
        printError(QString("Arguments not allowed for command: %1").arg(command));
        return 1;
    }

    const QString program = resolveBinary(commandIt.value());
    if (program.isEmpty()) {
        printError(QString("Command is not available: %1").arg(command));
        return 1;
    }

    return relayResult(runProcess(program, args, monitorCancel));
}

[[nodiscard]] int handleExec(const QStringList &args, bool monitorCancel)
{
    if (args.isEmpty()) {
        printError(QStringLiteral("exec requires a command name"));
        return 1;
    }
    if (monitorCancel && !isCancellableCommand(args.constFirst())) {
        printError(QStringLiteral("This command does not support cancellation"));
        return 1;
    }
    return runAllowedCommand(args.constFirst(), args.mid(1), monitorCancel);
}

// Writes new content for a managed APT source file directly as root: no caller-supplied path is
// ever trusted as the source of file content, only the target location and the bytes received
// over our own stdin (which nothing but our direct parent process can feed).
//
// Known, accepted residual: --allow-create (needed by the restore flow to recreate a file the
// user deleted) is just another argument to a directly-callable action, so a caller invoking the
// helper itself during the pkexec auth-cache window -- not only the GUI -- can also use it to
// create a brand-new *.list/*.sources file under sources.list.d. This can't be closed by
// validating arguments more tightly: PolicyKit's auth_admin_keep authorizes running this binary
// for the session, not any specific calling process, so any gate added here (a flag, a token
// minted by an earlier call, a narrower filename allow-list) is just as directly invokable by
// that same caller. Fully closing this needs either dropping auth_admin_keep in favor of
// re-authenticating every privileged action (real UX cost), or replacing this pkexec+CLI-helper
// model with a D-Bus service that verifies the caller's actual binary/peer credentials (a
// significant rewrite of the whole elevation mechanism). Neither has been done; this is the same
// class of limitation as a caller being able to overwrite the *content* of an existing managed
// file, just extended to being able to create a new one.
[[nodiscard]] int handleInstall(const QStringList &args, const QByteArray &content)
{
    QStringList positional = args;
    const bool allowCreate = !positional.isEmpty() && positional.constFirst() == QLatin1String("--allow-create");
    if (allowCreate) {
        positional.removeFirst();
    }

    if (positional.size() != 1) {
        printError(QStringLiteral("install requires exactly one target path"));
        return 1;
    }

    const QString &targetPath = positional.constFirst();
    if (!isManagedSourceFile(targetPath)) {
        printError(QString("Refusing to install to disallowed path: %1").arg(targetPath));
        return 1;
    }
    // Every legitimate caller except the restore flow (which may recreate a file the user
    // deleted) only ever replaces a file that already exists; without this, any *.list/*.sources
    // name under sources.list.d would be accepted, letting a caller plant a brand-new,
    // attacker-controlled APT source file that was never one of the app's known files.
    if (!allowCreate && !QFileInfo::exists(targetPath)) {
        printError(QString("Refusing to create new file: %1").arg(targetPath));
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

    if (action == "exec" || action == "exec-cancellable") {
        return handleExec(remainingArgs, action == "exec-cancellable");
    }
    if (action == QLatin1String("install")) {
        return handleInstall(remainingArgs, readHelperInput());
    }

    printError(QString("Unsupported helper action: %1").arg(action));
    return 1;
}
