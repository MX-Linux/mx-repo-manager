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

#include <cstdio>

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
        {"chmod", {"/usr/bin/chmod", "/bin/chmod"}},
        {"chown", {"/usr/bin/chown", "/bin/chown"}},
        {"cp", {"/usr/bin/cp", "/bin/cp"}},
        {"kill", {"/usr/bin/kill", "/bin/kill"}},
        {"mkdir", {"/usr/bin/mkdir", "/bin/mkdir"}},
        {"mv", {"/usr/bin/mv", "/bin/mv"}},
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

// A QTemporaryFile the GUI created directly under the system temp directory.
[[nodiscard]] bool isTempFile(const QString &path)
{
    const QString tempDir = QDir::tempPath() + QLatin1Char('/');

    if (!isCleanAbsolutePath(path) || !path.startsWith(tempDir)) {
        return false;
    }
    const QString rest = path.mid(tempDir.size());
    return !rest.isEmpty() && !rest.contains(QLatin1Char('/'));
}

// A *.list/*.sources file extracted from a package into a QTemporaryDir before being restored.
[[nodiscard]] bool isRestoreSourceFile(const QString &path)
{
    const QString tempDir = QDir::tempPath() + QLatin1Char('/');

    return isCleanAbsolutePath(path) && path.startsWith(tempDir)
        && (path.endsWith(".list") || path.endsWith(".sources"));
}

// Validate that the requested arguments match one of the specific operations this app ever
// performs, so an authenticated pkexec session cannot be reused to run e.g.
// "chmod 4755 /bin/bash" or "cp <anything> <anywhere>" -- only the program name was checked before.
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
    if (command == "chmod") {
        return args.size() == 2 && args.at(0) == "644" && isManagedSourceFile(args.at(1));
    }
    if (command == "chown") {
        return args.size() == 2 && (args.at(0) == "root:" || args.at(0) == "0:0") && isManagedSourceFile(args.at(1));
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
    if (command == "mv") {
        QStringList positional = args;
        QString flag;
        if (!positional.isEmpty() && positional.constFirst().startsWith(QLatin1Char('-'))) {
            flag = positional.takeFirst();
        }
        if (positional.size() != 2 || !(flag.isEmpty() || flag == "-f" || flag == "-b")) {
            return false;
        }
        const QString &src = positional.at(0);
        const QString &dst = positional.at(1);
        if (!(isTempFile(src) || isRestoreSourceFile(src)) || !isManagedSourceFile(dst)) {
            return false;
        }
        // Only the restore flow (mv -b) legitimately recreates a file the user deleted; the
        // replace/toggle flows (-f or no flag) always target a file that already exists. Without
        // this, any *.list/*.sources name under sources.list.d would be accepted, letting a caller
        // plant a brand-new, attacker-controlled APT source file that was never one of the app's
        // known files.
        return flag == "-b" || QFileInfo::exists(dst);
    }
    if (command == "kill") {
        QStringList positional = args;
        if (!positional.isEmpty() && positional.constFirst() == "-9") {
            positional.removeFirst();
        }
        if (positional.size() != 1) {
            return false;
        }
        bool ok = false;
        const qint64 pid = positional.constFirst().toLongLong(&ok);
        return ok && pid > 0;
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

[[nodiscard]] ProcessResult runProcess(const QString &program, const QStringList &args, const QByteArray &input = {})
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
        return result;
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

[[nodiscard]] int runAllowedCommand(const QString &command, const QStringList &args, const QByteArray &input = {})
{
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

    return relayResult(runProcess(program, args, input));
}

[[nodiscard]] int handleExec(const QStringList &args)
{
    if (args.isEmpty()) {
        printError(QStringLiteral("exec requires a command name"));
        return 1;
    }
    return runAllowedCommand(args.constFirst(), args.mid(1), readHelperInput());
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

    printError(QString("Unsupported helper action: %1").arg(action));
    return 1;
}
