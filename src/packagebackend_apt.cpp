/**********************************************************************
 *  packagebackend_apt.cpp
 **********************************************************************
 * Copyright (C) 2026-2026 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
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

#include "packagebackend.h"

#include <QProcess>

#include "aptcache.h"
#include "cmd.h"

namespace {

QHash<QString, QString> debconfEnvironment()
{
    return {{QStringLiteral("DEBIAN_FRONTEND"), debconfFrontend()}};
}

QStringList installedPackageFields(const QString &line)
{
    // dpkg-query prefixes each row with three status characters and a space.
    // The second character is the current state, including for packages on hold.
    constexpr qsizetype statusWidth = 3;
    if (line.size() <= statusWidth || line.at(1) != 'i' || line.at(statusWidth) != ' ') {
        return {};
    }
    return line.mid(statusWidth + 1).split(' ', Qt::SkipEmptyParts);
}

} // namespace

bool PackageBackend::refreshRepositories(Cmd &cmd)
{
    return cmd.procScriptAsRoot(QStringLiteral("/usr/lib/mx-packageinstaller/mxpi-maintenance"),
                                {QStringLiteral("apt_update")}, nullptr, nullptr, Cmd::QuietMode::Yes);
}

QHash<QString, PackageInfo> PackageBackend::listInstalled(bool *ok)
{
    Cmd shell;
    const QString list = shell.getOut(
        {{"LANG", "C"}}, "dpkg-query",
        {"-W", "-f=${db:Status-Abbrev} ${Package} ${Version} ${binary:Synopsis}\\n"});

    if (ok) {
        *ok = shell.exitStatus() == QProcess::NormalExit && shell.exitCode() == 0;
    }
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        return {};
    }

    QHash<QString, PackageInfo> installedPackagesMap;
    const auto lines = list.split('\n', Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        const QStringList parts = installedPackageFields(line);
        if (parts.size() < 2) {
            continue;
        }

        const QString packageName = parts.at(0);
        const QString version = parts.at(1);
        const QString description = parts.size() > 2 ? parts.mid(2).join(' ') : QString();

        installedPackagesMap.insert(packageName, {version, description});
    }

    return installedPackagesMap;
}

QHash<QString, VersionNumber> PackageBackend::listInstalledVersions(bool *ok)
{
    QHash<QString, VersionNumber> installedVersions;
    Cmd shell;
    const QStringList packageList
        = shell.getOut({{"LANG", "C"}}, "dpkg-query", {"-W", "-f=${db:Status-Abbrev} ${Package} ${Version}\\n"},
                       Cmd::QuietMode::Yes)
              .split('\n', Qt::SkipEmptyParts);

    if (ok) {
        *ok = shell.exitStatus() == QProcess::NormalExit && shell.exitCode() == 0;
    }
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        return installedVersions;
    }

    for (const QString &line : packageList) {
        const QStringList packageInfo = installedPackageFields(line);
        if (packageInfo.size() == 2) {
            installedVersions.insert(packageInfo.at(0), VersionNumber(packageInfo.at(1)));
        }
    }
    return installedVersions;
}

bool PackageBackend::markManuallyInstalled(Cmd &cmd, const QStringList &names)
{
    QStringList args {"manual"};
    args += names;
    return cmd.procAsRoot("apt-mark", args);
}

QStringList PackageBackend::autoremovableCandidates(Cmd &cmd)
{
    QStringList names;
    const QString aptOut = cmd.getOut({{"LANG", "C"}}, "apt-get", {"--dry-run", "autoremove"});
    for (const QString &line : aptOut.split('\n', Qt::SkipEmptyParts)) {
        if (line.startsWith("Remv ")) {
            const QString pkg = line.section(' ', 1, 1, QString::SectionSkipEmpty);
            if (!pkg.isEmpty()) {
                names << pkg;
            }
        }
    }
    return names;
}

bool PackageBackend::installPackages(Cmd &cmd, const QStringList &names, const QStringList &extraArgs)
{
    QStringList args {"-o=Dpkg::Use-Pty=0", "install", "-y"};
    args += extraArgs;
    args += names;
    return cmd.procAsRootWithEnv(debconfEnvironment(), "apt-get", args);
}

bool PackageBackend::removePackages(Cmd &cmd, const QStringList &names)
{
    QStringList args {"-o=Dpkg::Use-Pty=0", "remove", "-y"};
    args += names;
    return cmd.procAsRootWithEnv(debconfEnvironment(), "apt-get", args);
}

int PackageBackend::compareVersions(const QString &a, const QString &b)
{
    const std::strong_ordering cmp = VersionNumber(a) <=> VersionNumber(b);
    if (cmp < 0) {
        return -1;
    }
    if (cmp > 0) {
        return 1;
    }
    return 0;
}
