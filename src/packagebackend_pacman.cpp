/**********************************************************************
 *  packagebackend_pacman.cpp
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

#include <QDebug>
#include <QProcess>

#include <dlfcn.h>

#include "cmd.h"

bool PackageBackend::refreshRepositories(Cmd &cmd)
{
    return cmd.procScriptAsRoot(QStringLiteral("/usr/lib/mx-packageinstaller/mxpi-maintenance"),
                                {QStringLiteral("repo_sync")}, nullptr, nullptr, Cmd::QuietMode::Yes);
}

// All packages available in the sync DBs (not just installed), for the Enabled
// Repos tab's package list. Apt's equivalent is AptCache::getCandidates(), parsed
// from downloaded archive files; pacman has no local archive to parse, so this
// queries "pacman -Ss" directly instead -- fed by the same repo cache updateApt()
// (via PackageBackend::refreshRepositories) already refreshes for both backends.
// Not part of the PackageBackend:: interface: apt has no equivalent shape for
// this (see packagebackend.h's note on asymmetric operations), so it's a plain
// pacman-only free function instead, callable from a background thread via its
// own local Cmd instance like listInstalled() above.
// Output shape, one entry per package: "repo/name version [installed]" then an
// indented description line, e.g.:
//   extra/firefox 130.0-1 [installed]
//       Fast, Private and Safe Web Browser
QHash<QString, PackageInfo> pacmanAvailablePackages(bool *ok)
{
    Cmd shell;
    const QString output = shell.getOut({{"LANG", "C"}}, "pacman", {"-Ss", "--color", "never"});

    if (ok) {
        *ok = shell.exitStatus() == QProcess::NormalExit && shell.exitCode() == 0;
    }

    QHash<QString, PackageInfo> packages;
    QString pendingName;
    QString pendingVersion;
    for (const QString &line : output.split('\n')) {
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(' ') || line.startsWith('\t')) {
            // Repos are listed in pacman.conf order, and pacman -S installs from the
            // first one that has the package, so keep the first entry for each name.
            if (!pendingName.isEmpty() && !packages.contains(pendingName)) {
                packages.insert(pendingName, {pendingVersion, line.trimmed()});
            }
            pendingName.clear();
            continue;
        }
        const QString repoAndName = line.section(' ', 0, 0);
        const QString name = repoAndName.section('/', 1);
        if (name.isEmpty()) {
            continue;
        }
        pendingName = name;
        pendingVersion = line.section(' ', 1, 1);
    }
    return packages;
}

QHash<QString, PackageInfo> PackageBackend::listInstalled(bool *ok)
{
    Cmd shell;
    const QString list = shell.getOut({{"LANG", "C"}}, "pacman", {"-Qi"});

    if (ok) {
        *ok = shell.exitStatus() == QProcess::NormalExit && shell.exitCode() == 0;
    }
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        return {};
    }

    QHash<QString, PackageInfo> installedPackagesMap;
    QString packageName;
    QString version;
    QString description;

    auto flushPackage = [&]() {
        if (!packageName.isEmpty()) {
            installedPackagesMap.insert(packageName, {version, description});
        }
        packageName.clear();
        version.clear();
        description.clear();
    };

    const auto lines = list.split('\n');
    for (const QString &line : lines) {
        if (line.startsWith("Name")) {
            flushPackage();
            packageName = line.section(':', 1).trimmed();
        } else if (line.startsWith("Version")) {
            version = line.section(':', 1).trimmed();
        } else if (line.startsWith("Description")) {
            description = line.section(':', 1).trimmed();
        }
    }
    flushPackage();

    return installedPackagesMap;
}

QHash<QString, VersionNumber> PackageBackend::listInstalledVersions(bool *ok)
{
    QHash<QString, VersionNumber> installedVersions;
    Cmd shell;
    const QString list = shell.getOut({{"LANG", "C"}}, "pacman", {"-Q"}, Cmd::QuietMode::Yes);

    if (ok) {
        *ok = shell.exitStatus() == QProcess::NormalExit && shell.exitCode() == 0;
    }
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        return installedVersions;
    }

    for (const QString &line : list.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            // VersionNumber is only used here as this shared function's return-type
            // container for the raw version string -- every caller round-trips it
            // straight back through toString() before comparing anything (see
            // MainWindow::listInstalledVersions()), so no dpkg-style ordering is
            // ever applied to it. Actual pacman version comparisons go through
            // PackageBackend::compareVersions() below (which shells out to the
            // real vercmp), not this type.
            installedVersions.insert(parts.at(0), VersionNumber(parts.at(1)));
        }
    }
    return installedVersions;
}

bool PackageBackend::markManuallyInstalled(Cmd &cmd, const QStringList &names)
{
    QStringList args {"-D", "--asexplicit"};
    args += names;
    return cmd.procAsRoot("pacman", args);
}

QStringList PackageBackend::autoremovableCandidates(Cmd &cmd)
{
    return cmd.getOut({{"LANG", "C"}}, "pacman", {"-Qtdq"}).split('\n', Qt::SkipEmptyParts);
}

bool PackageBackend::installPackages(Cmd &cmd, const QStringList &names, const QStringList &extraArgs)
{
    Q_UNUSED(extraArgs); // no pacman equivalent to apt's recommends/-t release flags today
    QStringList args {"-S", "--needed"};
    args += names;
    return cmd.procAsRoot("pacman", args);
}

bool PackageBackend::removePackages(Cmd &cmd, const QStringList &names)
{
    QStringList args {"-Rns"};
    args += names;
    return cmd.procAsRoot("pacman", args);
}

namespace {

using AlpmPkgVercmpFn = int (*)(const char *, const char *);

// Resolves libalpm's alpm_pkg_vercmp() via dlopen/dlsym rather than linking
// against it at build time: there is no libalpm-dev package (headers +
// pkg-config file) to build against in every environment this project is
// built in, but the shared library itself is always present at runtime --
// pacman is dynamically linked against it, so anywhere PACKAGE_BACKEND=pacman
// actually runs, libalpm.so is already loaded into the process or trivially
// loadable. This gets the real, exact libalpm ordering rules (including its
// "~" and unmarked-trailing-suffix behavior, which genuinely differs from
// dpkg's VersionNumber) as a single fast in-process call, with no subprocess
// spawned per comparison -- callers (PackageModel::updateInstalledVersions(),
// PackageFilterProxy::lessThan()) can be called once per package or
// O(n log n) times while sorting a full repo listing, so shelling out to the
// `vercmp` CLI per call (an earlier version of this fix did) would spawn
// thousands to hundreds of thousands of processes on the GUI thread.
AlpmPkgVercmpFn resolveAlpmPkgVercmp()
{
    // RTLD_NOLOAD first: if something else in the process already dlopen'd (or
    // linked) libalpm, reuse that handle instead of loading a second copy.
    void *handle = dlopen("libalpm.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen("libalpm.so", RTLD_NOW);
    }
    if (!handle) {
        // Sonames are versioned (e.g. libalpm.so.15); try the unversioned name
        // first since it's what a `-dev`-style symlink would provide, then
        // fall back to the versioned name pacman's own package installs.
        handle = dlopen("libalpm.so.15", RTLD_NOW);
    }
    if (!handle) {
        qWarning() << "Could not dlopen libalpm to compare pacman package versions:" << dlerror();
        return nullptr;
    }
    auto fn = reinterpret_cast<AlpmPkgVercmpFn>(dlsym(handle, "alpm_pkg_vercmp"));
    if (!fn) {
        qWarning() << "libalpm loaded but alpm_pkg_vercmp symbol not found:" << dlerror();
    }
    return fn;
}

} // namespace

int PackageBackend::compareVersions(const QString &a, const QString &b)
{
    // Resolved once and cached (function-local static init is thread-safe, and
    // a nullptr result -- libalpm somehow unavailable -- is itself cached so a
    // broken environment doesn't retry a dlopen() on every single comparison).
    static const AlpmPkgVercmpFn alpmPkgVercmp = resolveAlpmPkgVercmp();
    if (!alpmPkgVercmp) {
        qWarning() << "Cannot compare pacman versions" << a << "and" << b << "-- treating as equal";
        return 0;
    }
    return alpmPkgVercmp(a.toUtf8().constData(), b.toUtf8().constData());
}
