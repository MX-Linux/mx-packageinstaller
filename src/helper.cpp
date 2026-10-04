/**********************************************************************
 *  helper.cpp
 **********************************************************************
 * Copyright (C) 2026-2026 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *          OpenAI Codex
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package. If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include "helperengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QtXml/QDomDocument>

#include <cstdio>
#include <cstdlib>

using namespace HelperEngine;

namespace
{
#ifndef PACKAGE_BACKEND_PACMAN
constexpr auto TempSourceListPath = "/etc/apt/sources.list.d/mxpitemp.list";
#endif
constexpr auto PkgListDirPath = "/usr/share/mx-packageinstaller-pkglist";

void printError(const QString &message)
{
    writeAndFlush(stderr, message.toUtf8() + '\n');
}

[[nodiscard]] const QHash<QString, QStringList> &allowedCommands()
{
    static const QHash<QString, QStringList> commands {
        {"chown", {"/usr/bin/chown", "/bin/chown"}},
        {"fuser", {"/usr/bin/fuser", "/bin/fuser"}},
        {"ps", {"/usr/bin/ps", "/bin/ps"}},
        {"snap", {"/usr/bin/snap", "/snap/bin/snap"}},
#ifdef PACKAGE_BACKEND_PACMAN
        {"pacman", {"/usr/bin/pacman"}},
#else
        {"apt-get", {"/usr/bin/apt-get"}},
        {"apt-mark", {"/usr/bin/apt-mark"}},
        // "aptitude" intentionally omitted: mainwindow.cpp only ever runs it
        // unprivileged (for the info/simulate dialogs), never through this helper.
#endif
    };
    return commands;
}

// Environment overrides the helper passes on to the commands it runs.
// DEBIAN_FRONTEND takes any value. The locale variables only take "C": callers use
// them to get untranslated output they can parse (LockFile's "apt-get check" probe).
[[nodiscard]] bool isAllowedEnvironment(const QString &name, const QString &value)
{
    if (name == QLatin1String("DEBIAN_FRONTEND")) {
        return true;
    }
    if (name == QLatin1String("LANG") || name == QLatin1String("LC_ALL")) {
        return value == QLatin1String("C");
    }
    return false;
}

[[nodiscard]] int runAllowedCommand(const QString &command, const QStringList &commandArgs,
                                    const QHash<QString, QString> &environment = {}, bool cancelOnStdinEof = false)
{
    const auto commandIt = allowedCommands().constFind(command);
    if (commandIt == allowedCommands().constEnd()) {
        printError(QString("Command is not allowed: %1").arg(command));
        return 127;
    }

    const QString resolvedCommand = resolveBinary(commandIt.value());
    if (resolvedCommand.isEmpty()) {
        printError(QString("Command is not available: %1").arg(command));
        return 127;
    }

    QHash<QString, QString> commandEnvironment = environment;
    // The snap client warns "/snap/bin is not in your $PATH" based on the PATH of
    // the process that invokes it. Running through this elevated helper, that PATH
    // is root's sanitized one, which never contains /snap/bin, so the warning shows
    // on every install regardless of the user's session (and a reboot won't help).
    // Ensure the snap process sees /snap/bin so the spurious warning is suppressed.
    if (command == QLatin1String("snap") && !commandEnvironment.contains(QStringLiteral("PATH"))) {
        QString path = qEnvironmentVariable("PATH");
        if (!path.split(QLatin1Char(':')).contains(QLatin1String("/snap/bin"))) {
            path = path.isEmpty() ? QStringLiteral("/snap/bin") : path + QStringLiteral(":/snap/bin");
        }
        commandEnvironment.insert(QStringLiteral("PATH"), path);
    }

    if (command == QLatin1String("snap") || command == QLatin1String("pacman")) {
        // snap stays silent, and pacman drops its progress bars/coloring and
        // confirmation-prompt formatting, unless each believes it is writing to a
        // real terminal -- run both under a pseudo-terminal rather than plain pipes.
        return relayResult(runProcessOnPty(resolvedCommand, commandArgs, commandEnvironment, cancelOnStdinEof));
    }

    return relayResult(runProcess(resolvedCommand, commandArgs, commandEnvironment, true, true, cancelOnStdinEof));
}

[[nodiscard]] int handleExec(const QStringList &args, bool cancelOnStdinEof)
{
    QStringList remainingArgs = args;
    QHash<QString, QString> environment;

    while (remainingArgs.size() >= 2 && remainingArgs.constFirst() == QLatin1String("--env")) {
        const QString assignment = remainingArgs.at(1);
        const int separatorIndex = assignment.indexOf('=');
        if (separatorIndex <= 0) {
            printError(QString("Invalid environment assignment: %1").arg(assignment));
            return 1;
        }

        const QString name = assignment.left(separatorIndex);
        const QString value = assignment.mid(separatorIndex + 1);
        if (!isAllowedEnvironment(name, value)) {
            printError(QString("Environment assignment is not allowed: %1").arg(assignment));
            return 1;
        }
        environment.insert(name, value);
        remainingArgs = remainingArgs.mid(2);
    }

    if (remainingArgs.isEmpty()) {
        printError(QStringLiteral("exec requires a command name"));
        return 1;
    }

    return runAllowedCommand(remainingArgs.constFirst(), remainingArgs.mid(1), environment, cancelOnStdinEof);
}

// The only legitimate caller (LockFile::getLockingProcess()) always passes this
// same literal for the compiled backend; reject anything else rather than trusting
// an arbitrary caller-supplied path, even though the caller is already root-equivalent.
#ifdef PACKAGE_BACKEND_PACMAN
constexpr auto ExpectedLockPath = "/var/lib/pacman/db.lck";
#else
constexpr auto ExpectedLockPath = "/var/lib/dpkg/lock";
#endif

[[nodiscard]] int handleLockingProcess(const QStringList &args)
{
    if (args.size() != 1) {
        printError(QStringLiteral("locking-process requires exactly one path"));
        return 1;
    }

    const QString path = args.constFirst();
    if (path != QLatin1String(ExpectedLockPath)) {
        printError(QString("locking-process path is not allowed: %1").arg(path));
        return 1;
    }
    if (!QFileInfo::exists(path)) {
        return 0;
    }

    const QString fuserBinary = resolveBinary(allowedCommands().value(QStringLiteral("fuser")));
    const QString psBinary = resolveBinary(allowedCommands().value(QStringLiteral("ps")));
    if (fuserBinary.isEmpty() || psBinary.isEmpty()) {
        printError(QStringLiteral("Required helper command is not available"));
        return 127;
    }

    const ProcessResult fuserResult = runProcess(fuserBinary, {path}, {}, false, false);
    const QString fuserOutput = QString::fromUtf8(fuserResult.standardOutput);
    const QRegularExpression pidRegex(QStringLiteral(R"((\d+))"));
    const QRegularExpressionMatch match = pidRegex.match(fuserOutput);
    if (!match.hasMatch()) {
        return 0;
    }

    const QString pid = match.captured(1);
    const ProcessResult psResult = runProcess(psBinary, {"--no-headers", "-o", "comm=", "-p", pid}, {}, false, false);
    if (!psResult.started || psResult.exitStatus != QProcess::NormalExit) {
        return 1;
    }

    writeAndFlush(stdout, psResult.standardOutput.trimmed());
    return 0;
}

#ifndef PACKAGE_BACKEND_PACMAN
// Writing an arbitrary apt sources-list entry as root has no pacman equivalent
// (no temporary-repo concept on that side today), so this whole action is apt-only.
[[nodiscard]] int handleWriteFile(const QStringList &args)
{
    if (args.size() != 2) {
        printError(QStringLiteral("write-file requires path and content"));
        return 1;
    }

    const QString path = args.at(0);
    if (path != QLatin1String(TempSourceListPath)) {
        printError(QString("write-file path is not allowed: %1").arg(path));
        return 1;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        printError(QString("Unable to write %1").arg(path));
        return 1;
    }

    file.write(args.at(1).toUtf8());
    file.close();
    return 0;
}
#endif

[[nodiscard]] QSet<QString> loadKnownHooks()
{
    QSet<QString> hooks;
    const QDir pkgListDir(QString::fromLatin1(PkgListDirPath));
    const QStringList pmFiles = pkgListDir.entryList({"*.pm"}, QDir::Files);
    static const QStringList hookTags {QStringLiteral("preinstall"), QStringLiteral("postinstall"),
                                       QStringLiteral("preuninstall"), QStringLiteral("postuninstall")};

    for (const QString &fileName : pmFiles) {
        QFile file(pkgListDir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        QDomDocument doc;
        if (!doc.setContent(&file)) {
            continue;
        }

        const QDomElement root = doc.firstChildElement(QStringLiteral("app"));
        for (const QString &tagName : hookTags) {
            for (QDomElement element = root.firstChildElement(tagName); !element.isNull();
                 element = element.nextSiblingElement(tagName)) {
                const QString script = element.text().trimmed();
                if (!script.isEmpty()) {
                    hooks.insert(script);
                }
            }
        }
    }

    return hooks;
}

[[nodiscard]] int handleRunHook(const QStringList &args, bool cancelOnStdinEof)
{
    if (args.size() != 1) {
        printError(QStringLiteral("run-hook requires exactly one script"));
        return 1;
    }

    const QString script = args.constFirst().trimmed();
    if (script.isEmpty()) {
        return 0;
    }

    static const QSet<QString> knownHooks = loadKnownHooks();
    if (!knownHooks.contains(script)) {
        printError(QStringLiteral("Hook is not recognized from installed package metadata"));
        return 1;
    }

    return relayResult(runProcess(QStringLiteral("/bin/bash"), {"-c", script}, {}, true, true, cancelOnStdinEof));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    installTerminationHandlers();
    QStringList arguments = app.arguments().mid(1);
    bool cancelOnStdinEof = false;

    // The caller passes an auth-success marker path as a leading "--marker
    // <path>" argument (not via the environment, which pkexec strips). The
    // atomically-created file proves the helper actually ran, so the caller can
    // tell a real 126/127 from a dismissed authentication dialog.
    if (arguments.size() >= 2 && arguments.constFirst() == QLatin1String("--marker")) {
        const QString markerPath = arguments.at(1);
        arguments.removeFirst();
        arguments.removeFirst();
        if (!createMarker(markerPath)) {
            return 1;
        }
    }

    if (!arguments.isEmpty() && arguments.constFirst() == QLatin1String("--cancel-on-eof")) {
        cancelOnStdinEof = true;
        arguments.removeFirst();
    }

    if (arguments.isEmpty()) {
        printError(QStringLiteral("Missing helper action"));
        return 1;
    }

    QStringList remainingArgs = arguments;
    const QString action = remainingArgs.takeFirst();

    if (action == QLatin1String("exec")) {
        return handleExec(remainingArgs, cancelOnStdinEof);
    }
    if (action == QLatin1String("locking-process")) {
        return handleLockingProcess(remainingArgs);
    }
    if (action == QLatin1String("run-hook")) {
        return handleRunHook(remainingArgs, cancelOnStdinEof);
    }
#ifndef PACKAGE_BACKEND_PACMAN
    if (action == QLatin1String("write-file")) {
        return handleWriteFile(remainingArgs);
    }
#endif
    if (action == QLatin1String("create-marker")) {
        if (remainingArgs.size() != 1) {
            printError(QStringLiteral("create-marker requires exactly one path"));
            return 1;
        }
        return createMarker(remainingArgs.constFirst()) ? 0 : 1;
    }

    printError(QString("Unsupported helper action: %1").arg(action));
    return 1;
}
