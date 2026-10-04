/**********************************************************************
 *  MainWindow.cpp
 **********************************************************************
 * Copyright (C) 2017-2026 MX Authors
 *
 * Authors: Adrian
 *          Dolphin_Oracle
 *          MX Linux <http://mxlinux.org>
 *
 * This file is part of mx-packageinstaller.
 *
 * mx-packageinstaller is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mx-packageinstaller is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mx-packageinstaller.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include "mainwindow.h"
#include "packagelistparser.h"
#include "ui_mainwindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextStream>
#include <QtConcurrent/QtConcurrent>
#include <QtGlobal>
#include <QtXml/QtXml>

#include "about.h"
#include "aptcache.h"
#include "checkableheaderview.h"
#include "mainwindow_helpers.h"
#include "outputrender.h"
#include "packagebackend.h"
#include "versionnumber.h"
#include <algorithm>
#include <chrono>
#include <pwd.h>
#include <unistd.h>

using namespace std::chrono_literals;

#ifdef PACKAGE_BACKEND_PACMAN
// Defined in packagebackend_pacman.cpp -- not part of the shared PackageBackend::
// interface (apt has no equivalent shape for this), but relocated there so it
// sits next to listInstalled() and can use its own local Cmd instance the same
// way, safe to call from a background thread. Declared here at external linkage
// (not inside the anonymous namespace below) to match its definition there.
QHash<QString, PackageInfo> pacmanAvailablePackages(bool *ok = nullptr);
#endif

namespace {
constexpr auto MxpiMaintenancePath = "/usr/lib/mx-packageinstaller/mxpi-maintenance";

using MainWindowHelpers::MxpiLibPath;
using MainWindowHelpers::appendFlatpakStatusMessage;
using MainWindowHelpers::runMxpiLibAsRoot;
using MainWindowHelpers::runScriptAsRoot;
using OutputRender::sanitizeOutputForDisplay;

QStringList packageArgs(const QString &names)
{
    return names.split(' ', Qt::SkipEmptyParts);
}

#ifdef PACKAGE_BACKEND_PACMAN
[[nodiscard]] QString userNameForUid(uid_t uid)
{
    if (const struct passwd *pw = getpwuid(uid)) {
        return QString::fromLocal8Bit(pw->pw_name);
    }
    return {};
}

[[nodiscard]] QString homeDirForUser(const QString &name)
{
    const QByteArray n = name.toLocal8Bit();
    if (const struct passwd *pw = getpwnam(n.constData())) {
        return QString::fromLocal8Bit(pw->pw_dir);
    }
    return {};
}

// When the app itself runs as root (e.g. WSL2 without a working pkexec), AUR helpers
// refuse to build, so we need a normal user to build as. No single source is reliable
// everywhere -- logname and SUDO_USER are often empty under WSL -- so try several in
// order of confidence and fall back to scanning the user database.
[[nodiscard]] QString loginUserForRoot()
{
    // 1. The user who elevated us, if recorded.
    const QString sudoUser = qEnvironmentVariable("SUDO_USER");
    if (!sudoUser.isEmpty() && sudoUser != QLatin1String("root")) {
        return sudoUser;
    }
    bool ok = false;
    const uint pkexecUid = qEnvironmentVariable("PKEXEC_UID").toUInt(&ok);
    if (ok && pkexecUid != 0) {
        const QString name = userNameForUid(static_cast<uid_t>(pkexecUid));
        if (!name.isEmpty()) {
            return name;
        }
    }

    auto isRegularUid = [](uint uid) { return uid >= 1000 && uid < 65534; };

    // 2. The owner of an active login session (systemd-logind runtime dir). Prefer the
    //    lowest regular uid (conventionally 1000) when several sessions exist.
    uid_t sessionUid = 0;
    const QDir runUser(QStringLiteral("/run/user"));
    const QStringList sessions = runUser.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : sessions) {
        bool entryOk = false;
        const uint uid = entry.toUInt(&entryOk);
        if (entryOk && isRegularUid(uid) && (sessionUid == 0 || uid < sessionUid)) {
            sessionUid = static_cast<uid_t>(uid);
        }
    }
    if (sessionUid != 0) {
        const QString name = userNameForUid(sessionUid);
        if (!name.isEmpty()) {
            return name;
        }
    }

    // 3. Fall back to the lowest regular account in the user database that has a real
    //    login shell (so daemon accounts that happen to sit at uid >= 1000 are skipped).
    auto hasLoginShell = [](const char *shell) {
        if (shell == nullptr || *shell == '\0') {
            return false;
        }
        const QString s = QString::fromLocal8Bit(shell);
        return !s.endsWith(QLatin1String("/nologin")) && !s.endsWith(QLatin1String("/false"));
    };
    QString best;
    uid_t bestUid = 0;
    setpwent();
    while (const struct passwd *pw = getpwent()) {
        if (isRegularUid(pw->pw_uid) && hasLoginShell(pw->pw_shell) && (best.isEmpty() || pw->pw_uid < bestUid)) {
            best = QString::fromLocal8Bit(pw->pw_name);
            bestUid = pw->pw_uid;
        }
    }
    endpwent();
    return best;
}
#endif

QStringList flatpakArgsWithScope(const QString &scope, QStringList args)
{
    const QString normalizedScope = scope.trimmed();
    if (!normalizedScope.isEmpty()) {
        args.insert(0, normalizedScope);
    }
    return args;
}

QString mxTestSourceLine(QString repoDistsUrl, const QString &suite, const QString &arch)
{
    if (repoDistsUrl.endsWith(QLatin1String("dists/"))) {
        repoDistsUrl.chop(QStringLiteral("dists/").size());
    }
    if (repoDistsUrl.endsWith(QLatin1Char('/'))) {
        repoDistsUrl.chop(1);
    }
    const QString prefix
        = (arch == QLatin1String("amd64")) ? QStringLiteral("deb ") : QStringLiteral("deb [arch=%1] ").arg(arch);
    return prefix + repoDistsUrl + ' ' + suite + QStringLiteral(" test\n");
}

QString backportsSourceLine(const QString &suite)
{
    return QStringLiteral("deb https://ftp.debian.org/debian %1-backports main contrib non-free\n").arg(suite);
}

bool runHooksAsRoot(Cmd &cmd, const QStringList &hooks, Cmd::QuietMode quiet = Cmd::QuietMode::No)
{
    for (const QString &hook : hooks) {
        if (!hook.trimmed().isEmpty() && !cmd.runHookAsRoot(hook, quiet)) {
            return false;
        }
    }
    return true;
}

QString shellSingleQuote(QString text)
{
    text.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QStringLiteral("'") + text + QStringLiteral("'");
}

QString shellCommandFromArgs(const QStringList &args)
{
    QStringList quoted;
    quoted.reserve(args.size());
    for (const QString &arg : args) {
        quoted.append(shellSingleQuote(arg));
    }
    return quoted.join(QLatin1Char(' '));
}

bool runMxpiLib(Cmd &cmd, const QString &action, Cmd::QuietMode quiet = Cmd::QuietMode::Yes)
{
    return cmd.proc(QString::fromLatin1(MxpiLibPath), {action}, nullptr, nullptr, quiet);
}

bool runMxpiMaintenanceAsRoot(Cmd &cmd, const QString &action, Cmd::QuietMode quiet = Cmd::QuietMode::Yes)
{
    return runScriptAsRoot(cmd, MxpiMaintenancePath, action, quiet);
}

bool systemFlatpakDefaultsPresent()
{
    Cmd shell;
    QStringList remotes
        = shell.getOut("flatpak", {"--system", "remote-list", "--columns=name"}, Cmd::QuietMode::Yes)
              .split('\n', Qt::SkipEmptyParts);
    for (QString &remote : remotes) {
        remote = remote.trimmed();
    }
    return remotes.contains(QLatin1String("flathub")) && remotes.contains(QLatin1String("flathub-verified"));
}

// The subprocess-calling half of listFlatpakRemotes(), extracted so the startup
// preload can run it on a background thread: every call here goes through a local
// Cmd instance, so it's safe off the GUI thread, unlike listFlatpakRemotes() itself
// (which also touches ui->comboRemote directly and isn't split out).
QStringList fetchFlatpakRemotes(const QString &scope, bool *ok)
{
    const bool isUserScope = scope.startsWith(QLatin1String("--user"));

    auto fetchRemotes = [&scope](QStringList &outList) {
        Cmd shell;
        outList = shell.getOut("flatpak", flatpakArgsWithScope(scope, {"remote-list", "--columns=name"}))
                      .split('\n', Qt::SkipEmptyParts);
        for (QString &name : outList) {
            name = name.trimmed();
        }
        outList.removeAll(QString());
        return shell.exitCode() == 0;
    };

    auto addUserRemotes = []() {
        Cmd addRemotes;
        return runMxpiLib(addRemotes, QStringLiteral("flatpak_add_repos_user"));
    };

    QStringList list;
    bool listOk = fetchRemotes(list);

    // If user scope listing failed (common when user has never set up flatpak), attempt to set up defaults
    if (!listOk && isUserScope) {
        qDebug() << "User remote-list failed; attempting to set up user remotes";
        if (addUserRemotes()) {
            listOk = fetchRemotes(list);
        }
    }

    // If no user remotes exist, set up the default ones
    if (list.isEmpty() && isUserScope) {
        qDebug() << "No flatpak remotes found for user, setting up default remotes";

        if (addUserRemotes()) {
            qDebug() << "Successfully set up flatpak remotes for user";

            // Re-fetch the remote list after setup
            listOk = fetchRemotes(list);
        } else {
            qDebug() << "Failed to set up flatpak remotes for user";
        }
    }

    if (ok) {
        *ok = listOk;
    }
    return list;
}

// The actual implementation behind MainWindow::listFlatpaks(), extracted to a free
// function taking the arch option explicitly (rather than calling getArchOption(),
// which reads the this->arch member) so the constructor's Flatpak catalog preload can
// call it directly from a QtConcurrent worker thread without touching `this` at all --
// the caller computes archOption on the GUI thread beforehand and passes it in by
// value. Every subprocess call here goes through a local Cmd instance, so it's safe to
// run in the background the same way fetchFlatpakRemotes() above is.
QStringList listFlatpaksImpl(const QString &remote, const QString &fpUserScope, const QString &archOption,
                             const QString &type = QString())
{
    QString archFp = archOption;
    if (archFp.isEmpty()) {
        return {};
    }

    // Check if remote parameter is empty (which would happen if no remotes are configured)
    if (remote.isEmpty()) {
        qDebug() << "Remote parameter is empty - no flatpak remotes configured for user";
        return {};
    }

    const bool isUserScope = fpUserScope.startsWith(QLatin1String("--user"));

    auto buildRemoteLsArgs = [&](const QString &scope) {
        QStringList args {"remote-ls"};
        args += flatpakArgsWithScope(scope, {});
        args << remote;
        const QString archOptionTrimmed = archFp.trimmed();
        if (!archOptionTrimmed.isEmpty()) {
            args << archOptionTrimmed;
        }
        args << "--columns=ver,branch,ref,installed-size";
        return args;
    };

    QString typeFlag;
    if (type == QLatin1String("--app")) {
        typeFlag = QStringLiteral("--app");
    } else if (type == QLatin1String("--runtime")) {
        typeFlag = QStringLiteral("--runtime");
    }
    auto runRemoteLs = [&typeFlag](QStringList args) {
        if (!typeFlag.isEmpty()) {
            args << typeFlag;
        }
        QString output;
        Cmd command;
        if (!command.proc("flatpak", args, &output, nullptr, Cmd::QuietMode::Yes)) {
            return QStringList {};
        }
        QStringList rows;
        for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
            if (line.contains('\t')) {
                rows << line;
            }
        }
        return rows;
    };

    // Execute the command and process the output
    QStringList list = runRemoteLs(buildRemoteLsArgs(fpUserScope));

    if (list.isEmpty()) {
        qDebug() << QString("Could not list packages from %1 remote, attempting to update remote").arg(remote);

        // Try to update the remote if it's empty
        QStringList updateArgs {"update"};
        updateArgs += flatpakArgsWithScope(fpUserScope, {});
        updateArgs << "--appstream" << remote;
        qDebug() << "Running remote update command:" << updateArgs;

        Cmd updateCommand;
        if (updateCommand.proc("flatpak", updateArgs, nullptr, nullptr, Cmd::QuietMode::Yes)) {
            qDebug() << "Remote update completed, retrying package list";

            // Retry the original command after update
            list = runRemoteLs(buildRemoteLsArgs(fpUserScope));
            if (!list.isEmpty()) {
                qDebug() << QString("Successfully retrieved %1 packages after remote update").arg(list.size());
            } else {
                qDebug() << QString("Remote %1 still empty after update").arg(remote);
            }
        } else {
            qDebug() << "Failed to update remote" << remote;
        }
    }

    // If user scope returned nothing (e.g. only system remotes exist), fall back to system remotes for listing
    if (list.isEmpty() && isUserScope) {
        const QStringList systemArgs = buildRemoteLsArgs(QStringLiteral("--system "));
        qDebug() << "User remotes empty; retrying flatpak listing using system remotes:" << systemArgs;
        list = runRemoteLs(systemArgs);
    }

    return list;
}
} // namespace

#ifdef MXPI_TESTING
MainWindow::MainWindow(const QCommandLineParser &argParser, QWidget *parent,
                       BackgroundWorkerTestHooks testHooks, bool testSkipFlatpakPreload)
    : QDialog(parent),
      ui(new Ui::MainWindow),
      dictionary("/usr/share/mx-packageinstaller-pkglist/category.dict", QSettings::IniFormat),
      args {argParser},
      testSkipFlatpakPreload {testSkipFlatpakPreload}
{
#else
MainWindow::MainWindow(const QCommandLineParser &argParser, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow),
      dictionary("/usr/share/mx-packageinstaller-pkglist/category.dict", QSettings::IniFormat),
      args {argParser}
{
#endif
    qDebug().noquote() << QCoreApplication::applicationName() << "version:" << QCoreApplication::applicationVersion();
    ui->setupUi(this);
    outputRenderer.setOutputBox(ui->outputBox);
    setProgressDialog();

    // A bare carriage return (not part of a "\r\n" line ending) marks a tool redrawing
    // its progress line in place. Those redraws are shown live in the Output tab; keep
    // them out of the debug log, where each one would otherwise be a separate line.
    const auto isProgressRedraw = [](const QString &out) {
        QString stripped = out;
        stripped.remove(QStringLiteral("\r\n"));
        return stripped.contains(QLatin1Char('\r'));
    };
    connect(&timer, &QTimer::timeout, this, &MainWindow::updateBar);
    connect(&cmd, &Cmd::started, this, &MainWindow::cmdStart);
    connect(&cmd, &Cmd::done, this, &MainWindow::cmdDone);
    connect(&cmd, &Cmd::outputAvailable, this, [this, isProgressRedraw](const QString &out) {
        if (!suppressCmdOutput && !isProgressRedraw(out)) {
            const QString clean = sanitizeOutputForDisplay(out).trimmed();
            if (!clean.isEmpty()) {
                qDebug() << clean;
            }
        }
    });
    connect(&cmd, &Cmd::errorAvailable, this,
            [this, isProgressRedraw](const QString &out) {
                if (!suppressCmdOutput && !isProgressRedraw(out)) {
                    const QString clean = sanitizeOutputForDisplay(out).trimmed();
                    if (!clean.isEmpty()) {
                        qWarning() << clean;
                    }
                }
            });
    setWindowFlags(Qt::Window); // For the close, min and max buttons

    setup();

    // Run package display in a separate thread
    // Run package preload in background
    // (apt-only: pacmanAvailablePackages() shells out via the shared Cmd member,
    // which isn't safe to touch off the GUI thread the way AptCache's pure file-IO
    // read is; pacman's Enabled tab just loads lazily on first visit instead.)
#ifndef PACKAGE_BACKEND_PACMAN
    [[maybe_unused]] auto future = QtConcurrent::run(
        [this, guardMutex = destructionGuardMutex, destructingFlag = destructing
#ifdef MXPI_TESTING
        , testHooks
#endif
        ] {
        AptCache cache;
        auto loadedList = cache.getCandidates();

#ifdef MXPI_TESTING
        if (testHooks.beforeGuardCheck) {
            testHooks.beforeGuardCheck();
        }
        // Fires on every exit from here on, regardless of which branch below is
        // taken -- lets a test wait for a real "the guarded section is done"
        // signal instead of assuming it completed within some time budget.
        auto workerFinishedGuard = qScopeGuard([&testHooks] {
            if (testHooks.workerFinished) {
                testHooks.workerFinished();
            }
        });
#endif
        // See destructionGuardMutex's declaration: this must be the worker's one and
        // only touch of `this`, immediately preceded by the check below.
        QMutexLocker locker(guardMutex.get());
        if (*destructingFlag) {
            return;
        }
#ifdef MXPI_TESTING
        if (testHooks.afterGuardPassed) {
            testHooks.afterGuardPassed();
        }
#endif
        // Set the model on main thread after preload — all member access happens on the GUI thread
        QMetaObject::invokeMethod(
            this,
            [this, loadedList = std::move(loadedList)]() mutable {
                enabledList = std::move(loadedList);
                if (enabledModel && !enabledList.isEmpty()) {
                    QVector<PackageData> packages;
                    packages.reserve(enabledList.size() + installedPackages.size());

                    for (const auto &[name, info] : std::as_const(enabledList).asKeyValueRange()) {
                        packages.append(createPackageData(name, info.version, info.description));
                    }

                    for (const auto &[name, info] : std::as_const(installedPackages).asKeyValueRange()) {
                        if (!enabledList.contains(name)) {
                            packages.append(createPackageData(name, QString(), info.description));
                        }
                    }

                    enabledModel->setPackageData(packages);

                    // Update installed versions
                    const auto installedVersions = listInstalledVersions();
                    QHash<QString, QString> versionStrings;
                    versionStrings.reserve(installedVersions.size());
                    for (const auto &[name, version] : installedVersions.asKeyValueRange()) {
                        versionStrings.insert(name, version.toString());
                    }
                    enabledModel->updateInstalledVersions(versionStrings);

                    // Ensure enabled tree is sorted after background load
                    if (enabledProxy) {
                        enabledProxy->sort(TreeCol::Name, Qt::AscendingOrder);
                    }

                    // Mark as not dirty since we just loaded it
                    dirtyEnabledRepos = false;
                }

                ui->tabWidget->setTabEnabled(Tab::Test, true);
                ui->tabWidget->setTabEnabled(Tab::Backports, true);
            },
            Qt::QueuedConnection);
    });
#endif

#ifdef PACKAGE_BACKEND_PACMAN
    // installedPackages ("pacman -Qi") is fetched in the background so it doesn't
    // block startup -- PackageBackend::listInstalled() uses a local Cmd instance,
    // safe to call off the GUI thread (matching how the arch branch originally
    // loaded this via QtConcurrent, before the unification made it synchronous).
    [[maybe_unused]] auto installedFuture = QtConcurrent::run(
        [this, guardMutex = destructionGuardMutex, destructingFlag = destructing
#ifdef MXPI_TESTING
        , testHooks
#endif
        ] {
        bool ok = false;
        auto loaded = PackageBackend::listInstalled(&ok);
#ifdef MXPI_TESTING
        if (testHooks.beforeGuardCheck) {
            testHooks.beforeGuardCheck();
        }
        // See the AptCache worker's matching comment above.
        auto workerFinishedGuard = qScopeGuard([&testHooks] {
            if (testHooks.workerFinished) {
                testHooks.workerFinished();
            }
        });
#endif
        QMutexLocker locker(guardMutex.get());
        if (*destructingFlag) {
            return;
        }
#ifdef MXPI_TESTING
        if (testHooks.afterGuardPassed) {
            testHooks.afterGuardPassed();
        }
#endif
        QMetaObject::invokeMethod(
            this,
            [this, ok, loaded = std::move(loaded)]() mutable {
                if (!ok) {
                    QMessageBox::critical(this, tr("Error"),
                                          tr("The installed-package query returned an error. Please check the log "
                                             "for details."));
                    return;
                }
                installedPackages = std::move(loaded);
                handleInstalledPackagesArrived();
            },
            Qt::QueuedConnection);
    });
#endif

    // Preload the flatpak list so the tab is populated on first visit. Listing
    // needs no elevation; system remote setup, if defaults are missing, happens
    // on the first visit to the Flatpak tab where an auth prompt has context.
    // Checked via the binary's presence, not checkInstalled()/installedPackages:
    // on pacman, installedPackages is now filled in by a background QtConcurrent
    // task kicked off just above, so it's still empty at this exact point --
    // checkInstalled() would always say "not installed" and silently skip this.
    // testSkipFlatpakPreload (test-only, see the MXPI_TESTING constructor overload)
    // lets Testing/test_mainwindow.cpp construct a MainWindow without this preload
    // chain reaching real flatpak remotes -- listFlatpaksImpl()'s "flatpak update
    // --appstream <remote>" fallback can genuinely touch the network if a remote's
    // local appstream cache is stale, which a test run must not do.
    if (arch != QLatin1String("i386") && !QStandardPaths::findExecutable(QStringLiteral("flatpak")).isEmpty()
#ifdef MXPI_TESTING
        && !testSkipFlatpakPreload
#endif
        ) {
        // Three subprocess calls happen before the Flatpak tab is ready: the remote
        // list ("flatpak remote-list", ~30-150ms), then the remote catalog
        // ("flatpak remote-ls", ~250-400ms) and installed-list ("flatpak list") for
        // whichever remote that produces. All three go through a local Cmd instance
        // (fetchFlatpakRemotes()/listFlatpaksImpl()/the shell below), never the shared
        // cmd member, so all are safe to run in the background -- same treatment as
        // installedPackages above, just chained in two stages since stage 2 needs to
        // know which remote stage 1 selected. None of the three ever calls a
        // MainWindow:: member function from the worker thread (listFlatpaksImpl() is a
        // free function taking the arch option explicitly, rather than this->listFlatpaks()
        // calling getArchOption() to read this->arch) -- so this stays safe even if the
        // window is destroyed while a fetch is still running.
        displayFlatpaksIsRunning = true;
        const QString scope = fpUser;
        // See flatpakRequestGeneration's declaration: bumped and captured now so this
        // preload's eventual completions can tell whether a newer Flatpak load (e.g.
        // the user switching remote/scope) has since superseded them.
        const int myGeneration = ++flatpakRequestGeneration;
        [[maybe_unused]] auto remotesFuture = QtConcurrent::run(
            [this, scope, myGeneration, guardMutex = destructionGuardMutex, destructingFlag = destructing] {
            bool remotesOk = false;
            const QStringList remotes = fetchFlatpakRemotes(scope, &remotesOk);
            QMutexLocker locker(guardMutex.get());
            if (*destructingFlag) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, remotes, remotesOk, scope, myGeneration, guardMutex, destructingFlag] {
                    if (isFlatpakGenerationStale(myGeneration)) {
                        // Superseded by a newer Flatpak load that started (and, being
                        // synchronous, already finished) while this fetch was in flight.
                        // Its result is current; publishing this stale one would clobber
                        // it. Only clear the busy flag if it's still plausibly ours --
                        // a newer synchronous load always resets it back to false itself.
                        displayFlatpaksIsRunning = false;
                        return;
                    }
                    if (remotesOk) {
                        // Warm the cache listFlatpakRemotes() checks first, so
                        // setupFlatpakDisplay()'s call into it below is a fast,
                        // synchronous cache hit instead of a second fetch.
                        cachedFlatpakRemotes = remotes;
                        cachedFlatpakRemotesScope = scope;
                        cachedFlatpakRemotesFetched = true;
                    }
                    setupFlatpakDisplay();
                    if (!remotesOk) {
                        finalizeFlatpakDisplay();
                        return;
                    }
                    const QString remote = ui->comboRemote->currentText();
                    // Computed here on the GUI thread (reads this->arch) and passed by
                    // value into the worker below, which must not touch `this` at all.
                    const QString archOption = getArchOption();
                    [[maybe_unused]] auto catalogFuture = QtConcurrent::run(
                        [this, remote, scope, archOption, myGeneration, guardMutex, destructingFlag] {
                        const QStringList remoteEntries = listFlatpaksImpl(remote, scope, archOption);
                        Cmd shell;
                        const QStringList allInstalled
                            = shell.getOut("flatpak", flatpakArgsWithScope(scope, {"list", "--columns=ref,size"}),
                                          Cmd::QuietMode::Yes, /*discardStderr=*/true)
                                  .split('\n', Qt::SkipEmptyParts);
                        QMutexLocker locker(guardMutex.get());
                        if (*destructingFlag) {
                            return;
                        }
                        QMetaObject::invokeMethod(
                            this,
                            [this, remoteEntries, allInstalled, scope, myGeneration] {
                                if (isFlatpakGenerationStale(myGeneration)) {
                                    // See the remotesFuture completion above -- a newer,
                                    // synchronous load has already published its own
                                    // (correct) data and reset the busy flag; don't stomp
                                    // on either.
                                    displayFlatpaksIsRunning = false;
                                    return;
                                }
                                processFlatpakData(remoteEntries, allInstalled, scope);
                                populateFlatpakTree();
                                finalizeFlatpakDisplay();
                            },
                            Qt::QueuedConnection);
                    });
                },
                Qt::QueuedConnection);
        });
    }
}

MainWindow::~MainWindow()
{
    // See destructionGuardMutex's declaration: this must run before anything else so
    // that any QtConcurrent::run() worker still in flight (installedPackages, the
    // Flatpak preload chain, the pacman -Ss repo fetch) is guaranteed to either see
    // *destructing == true and skip touching `this` entirely, or have already made its
    // one and only touch of `this` (a QMetaObject::invokeMethod(this, ...,
    // Qt::QueuedConnection) call, safe as long as `this` hasn't started destructing
    // yet) before this can acquire the lock. No wait is needed: those workers don't
    // need to finish before the window closes, they just need to never touch a `this`
    // that might already be gone -- which this ordering, not a wait, is what
    // guarantees.
    {
        QMutexLocker locker(destructionGuardMutex.get());
        *destructing = true;
    }
    delete ui;
}

void MainWindow::setup()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->blockSignals(true);
#ifdef PACKAGE_BACKEND_PACMAN
    // Popular apps has no data on this backend (see the Tab::Popular visibility
    // gating below), so land on Enabled Repos instead.
    ui->tabWidget->setCurrentWidget(ui->tabEnabled);
#else
    ui->tabWidget->setCurrentWidget(ui->tabPopular);
#endif
    ui->tabWidget->setTabEnabled(Tab::Test, false);
    ui->tabWidget->setTabEnabled(Tab::Backports, false);
    ui->pushRemoveAutoremovable->setHidden(true);

    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    ui->outputBox->setFont(font);

    // Stored as a locale-independent token ("system"/"user"); anything else
    // (including labels saved by older versions) falls back to system scope.
    const bool userScope = settings.value(QStringLiteral("FlatpakUser")).toString() == QLatin1String("user");
    fpUser = userScope ? QStringLiteral("--user ") : QStringLiteral("--system ");
    ui->comboUser->blockSignals(true);
    ui->comboUser->setCurrentIndex(userScope ? 1 : 0);
    ui->comboUser->blockSignals(false);

#ifdef PACKAGE_BACKEND_PACMAN
    // No Debian-release concept on this backend; these two only ever gate apt-only
    // code paths (Test/Backports, which are unreachable here since those tabs are
    // hidden). Arch doesn't rename architectures the way Debian's DEB_BUILD_ARCH
    // does, so the raw CPU architecture string is already the right value.
    arch = QSysInfo::currentCpuArchitecture();
    debianVersion = Release::Trixie;
    verName.clear();
#else
    arch = AptCache::getArch();
    debianVersion = getDebianVerNum();
    verName = getDebianVerName(debianVersion);
#endif

    ui->tabWidget->setTabVisible(Tab::Flatpak, arch != QLatin1String("i386"));
    // Snap is only meaningful on systemd systems (snapd requires systemd)
    ui->tabWidget->setTabVisible(Tab::Snap, isSystemdInit());
#ifdef PACKAGE_BACKEND_PACMAN
    ui->tabWidget->setTabVisible(Tab::Test, false);
    ui->tabWidget->setTabVisible(Tab::Backports, false);
    ui->tabWidget->setTabVisible(Tab::AUR, true);
    // Popular apps' package-list data (.pm files under
    // /usr/share/mx-packageinstaller-pkglist) is Debian-package-name-keyed and
    // shipped by a separate, apt-only packaging source; there's no Arch data for
    // it, so the tab would just be empty. Hide it rather than show dead space.
    ui->tabWidget->setTabVisible(Tab::Popular, false);
    // "Install recommends" has no pacman equivalent (installPackages() ignores
    // extraArgs on this backend) -- showing a checkbox with no effect would mislead.
    ui->checkBoxInstallRecommends->setVisible(false);
    // isLibraryPackage() is a no-op on this backend (no Debian-style lib*/-dev/
    // -dbg*/-libs split-package convention to filter on yet) -- same reasoning.
    ui->checkHideLibs->setVisible(false);
#else
    ui->tabWidget->setTabVisible(Tab::AUR, false);
    ui->tabWidget->setTabVisible(Tab::Test, QFile::exists("/etc/apt/sources.list.d/mx.list")
                                                || QFile::exists("/etc/apt/sources.list.d/mx.sources"));

    testInitiallyEnabled = cmd.pipeline({
        {"apt-get", {"update", "--print-uris"}},
        {"grep", {"-m1", "-qE", "/mx/testrepo/dists/" + verName + "/test/"}},
    });
#endif

    setWindowTitle(tr("MX Package Installer"));

    // Load icons FIRST, before models need them
    setIcons();

    // Set up models and proxies - requires icons to be loaded first
    setupModels();

    hideColumns();
#ifdef PACKAGE_BACKEND_PACMAN
    // Popular Apps has no data on this backend (tab hidden above) -- skip
    // scanning for .pm files and building/sorting an always-empty tree.
    // installedPackages is loaded in the background instead (see the
    // constructor, right after setup() returns) rather than blocking here.
#else
    loadPmFiles();
    refreshPopularApps();
#endif

    // Load persisted setting for hiding libraries/developer packages
    const bool savedHideLibs = settings.value("HideLibs", true).toBool();
    hideLibsChecked = savedHideLibs;
    ui->checkHideLibs->setChecked(savedHideLibs);
    ui->checkHideLibsMX->setChecked(savedHideLibs);
    ui->checkHideLibsBP->setChecked(savedHideLibs);

    // Load persisted setting for repo-only filter
    const bool savedRepoOnly = settings.value("RepoOnly", false).toBool();
    ui->checkRepoOnlyMX->setChecked(savedRepoOnly);
    ui->checkRepoOnlyBP->setChecked(savedRepoOnly);

    // Ensure "Select all" checkboxes start hidden/unchecked
    // (Deprecated UI checkboxes remain hidden in UI; header checkboxes are used instead.)
    if (auto *w = ui->checkSelectAllEnabled) {
        w->setVisible(false);
        w->setChecked(false);
    }
    if (auto *w = ui->checkSelectAllMX) {
        w->setVisible(false);
        w->setChecked(false);
    }
    if (auto *w = ui->checkSelectAllBP) {
        w->setVisible(false);
        w->setChecked(false);
    }
    // Make legend buttons non-interactive (avoid hover/click hints)
    const QList<QAbstractButton *> legendButtons {
        ui->iconInstalledPackages,
        ui->iconInstalledPackages_2,
        ui->iconInstalledPackages_3,
        ui->iconInstalledPackages_4,
        ui->iconInstalledPackages_5,
        ui->iconUpgradable,
        ui->iconUpgradable_2,
        ui->iconUpgradable_3,
    };
    for (auto *button : legendButtons) {
        if (!button) {
            continue;
        }
        button->setFocusPolicy(Qt::NoFocus);
        button->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    // Install custom header views with checkbox in column 0 (TreeCol::Check)
    headerEnabled = new CheckableHeaderView(Qt::Horizontal, ui->treeEnabled);
    headerEnabled->setTargetColumn(TreeCol::Check);
    headerEnabled->setMinimumSectionSize(22);
    ui->treeEnabled->setHeader(headerEnabled);

    headerMX = new CheckableHeaderView(Qt::Horizontal, ui->treeMXtest);
    headerMX->setTargetColumn(TreeCol::Check);
    headerMX->setMinimumSectionSize(22);
    ui->treeMXtest->setHeader(headerMX);

    headerBP = new CheckableHeaderView(Qt::Horizontal, ui->treeBackports);
    headerBP->setTargetColumn(TreeCol::Check);
    headerBP->setMinimumSectionSize(22);
    ui->treeBackports->setHeader(headerBP);

#ifdef PACKAGE_BACKEND_PACMAN
    headerAUR = new CheckableHeaderView(Qt::Horizontal, ui->treeAUR);
    headerAUR->setTargetColumn(TreeCol::Check);
    headerAUR->setMinimumSectionSize(22);
    ui->treeAUR->setHeader(headerAUR);
#endif

    setConnections();

#ifdef PACKAGE_BACKEND_PACMAN
    ui->searchBoxEnabled->setFocus();
    currentTree = ui->treeEnabled;
#else
    ui->searchPopular->setFocus();
    currentTree = ui->treePopularApps;
#endif

    ui->tabWidget->setTabEnabled(Tab::Output, false);
    ui->tabWidget->blockSignals(false);
    ui->pushUpgradeAll->setVisible(false);

#ifdef PACKAGE_BACKEND_PACMAN
    // Landing on Enabled Repos happened above while tabWidget signals were still
    // blocked, so tabWidget_currentChanged (which is what actually loads a tab's
    // data everywhere else) never fired for it. Apt's equivalent initial load
    // comes from the constructor's background AptCache preload, which pacman
    // deliberately skips (see MainWindow::MainWindow) -- so without this, the tab
    // stays empty until the user switches away and back, which does fire a real
    // signal. Trigger the same load explicitly instead.
    QMetaObject::invokeMethod(this, [this] { handleEnabledReposTab(QString()); }, Qt::QueuedConnection);
#endif

    const auto size = this->size();
    if (settings.contains(QStringLiteral("geometry"))) {
        restoreGeometry(settings.value("geometry").toByteArray());
        if (isMaximized()) { // Add option to resize if maximized
            resize(size);
            centerWindow();
        }
    }
#ifndef PACKAGE_BACKEND_PACMAN
    const QString aptConfigOutput
        = cmd.getOut(QStringLiteral("apt-config"), {"shell", "APTOPT", "APT::Install-Recommends/b"}).trimmed();
    ui->checkBoxInstallRecommends->setChecked(aptConfigOutput == QLatin1String("APTOPT='true'"));
    ui->checkBoxInstallRecommendsMX->setChecked(aptConfigOutput == QLatin1String("APTOPT='true'"));
    ui->checkBoxInstallRecommendsBP->setChecked(aptConfigOutput == QLatin1String("APTOPT='true'"));
#endif

    // Check/uncheck tree items spacebar press or double-click
    auto *shortcutToggle = new QShortcut(Qt::Key_Space, this);
    connect(shortcutToggle, &QShortcut::activated, this, &MainWindow::checkUncheckItem);

    // Ctrl+I shows info for the current row of whichever list tab is open
    auto *shortcutInfo = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_I), this);
    connect(shortcutInfo, &QShortcut::activated, this, [this] {
        if (currentTree && currentTree->isVisible()) {
            displayInfo(currentTree, currentTree->currentIndex());
        }
    });

    // Clicking the Info icon opens the same dialog as "More info..." (Popular Apps
    // is wired in setConnections())
    connect(ui->treeFlatpak, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() == FlatCol::Info) {
            displayFlatpakInfo(index);
        }
    });
    connect(ui->treeSnap, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() == SnapCol::Info) {
            displaySnapInfo(index);
        }
    });

    // Connect tree views for double-click toggle
    QList<QTreeView *> listTree {ui->treePopularApps, ui->treeEnabled, ui->treeMXtest, ui->treeBackports,
                                  ui->treeFlatpak, ui->treeSnap};
#ifdef PACKAGE_BACKEND_PACMAN
    listTree.append(ui->treeAUR);
#endif
    for (auto *tree : listTree) {
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree, &QTreeView::doubleClicked, this, &MainWindow::checkUncheckItem);
        // treePopularApps has its own context-menu handler wired in setConnections();
        // wiring the generic one too would pop up two menus.
        if (tree != ui->treePopularApps) {
            connect(tree, &QTreeView::customContextMenuRequested, this,
                    [this, tree](QPoint pos) { displayPackageInfo(tree, pos); });
        }
    }
}

void MainWindow::setupModels()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    // Create Package models for APT trees
    enabledModel = new PackageModel(this);
    mxtestModel = new PackageModel(this);
    backportsModel = new PackageModel(this);

    // Create filter proxies for APT trees
    enabledProxy = new PackageFilterProxy(this);
    enabledProxy->setSourceModel(enabledModel);
    enabledProxy->setHideLibraries(hideLibsChecked);

    mxtestProxy = new PackageFilterProxy(this);
    mxtestProxy->setSourceModel(mxtestModel);
    mxtestProxy->setHideLibraries(hideLibsChecked);
    mxtestProxy->setRepoOnly(settings.value("RepoOnly", false).toBool());

    backportsProxy = new PackageFilterProxy(this);
    backportsProxy->setSourceModel(backportsModel);
    backportsProxy->setHideLibraries(hideLibsChecked);
    backportsProxy->setRepoOnly(settings.value("RepoOnly", false).toBool());

    // Set models on tree views
    ui->treeEnabled->setModel(enabledProxy);
    ui->treeMXtest->setModel(mxtestProxy);
    ui->treeBackports->setModel(backportsProxy);

    // Disable editing on all tree views (allow selection/copy but not modification)
    ui->treeEnabled->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->treeMXtest->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->treeBackports->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Enable sorting and set initial sort by Package Name (column 1)
    ui->treeEnabled->setSortingEnabled(true);
    ui->treeEnabled->sortByColumn(1, Qt::AscendingOrder);
    ui->treeMXtest->setSortingEnabled(true);
    ui->treeMXtest->sortByColumn(1, Qt::AscendingOrder);
    ui->treeBackports->setSortingEnabled(true);
    ui->treeBackports->sortByColumn(1, Qt::AscendingOrder);

    // Create Flatpak model and proxy
    flatpakModel = new FlatpakModel(this);
    flatpakProxy = new FlatpakFilterProxy(this);
    flatpakProxy->setSourceModel(flatpakModel);
    ui->treeFlatpak->setModel(flatpakProxy);
    ui->treeFlatpak->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Create Snap model and proxy
    snapModel = new SnapModel(this);
    snapProxy = new SnapFilterProxy(this);
    snapProxy->setSourceModel(snapModel);
    ui->treeSnap->setModel(snapProxy);
    ui->treeSnap->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Create Popular model and proxy
    popularModel = new PopularModel(this);
    popularProxy = new PopularFilterProxy(this);
    popularProxy->setSourceModel(popularModel);
    ui->treePopularApps->setModel(popularProxy);

#ifdef PACKAGE_BACKEND_PACMAN
    // Create AUR model and proxy
    aurModel = new PackageModel(this);
    aurProxy = new PackageFilterProxy(this);
    aurProxy->setSourceModel(aurModel);
    ui->treeAUR->setModel(aurProxy);
    ui->treeAUR->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->treeAUR->setSortingEnabled(true);
    ui->treeAUR->sortByColumn(1, Qt::AscendingOrder);
#endif
    ui->treePopularApps->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Set icons for all models
    enabledModel->setIcons(qiconInstalled, qiconUpgradable);
    mxtestModel->setIcons(qiconInstalled, qiconUpgradable);
    backportsModel->setIcons(qiconInstalled, qiconUpgradable);
    flatpakModel->setIcons(qiconInstalled, QIcon::fromTheme("dialog-information"));
    snapModel->setIcons(qiconInstalled, QIcon::fromTheme("dialog-information"));
    popularModel->setIcons(qiconInstalled, QIcon::fromTheme("folder"), QIcon::fromTheme("dialog-information"));
#ifdef PACKAGE_BACKEND_PACMAN
    aurModel->setIcons(qiconInstalled, qiconUpgradable);
#endif

    // Connect model signals to slots
    connect(enabledModel, &PackageModel::checkStateChanged, this, &MainWindow::onPackageCheckStateChanged);
    connect(mxtestModel, &PackageModel::checkStateChanged, this, &MainWindow::onPackageCheckStateChanged);
    connect(backportsModel, &PackageModel::checkStateChanged, this, &MainWindow::onPackageCheckStateChanged);
#ifdef PACKAGE_BACKEND_PACMAN
    connect(aurModel, &PackageModel::checkStateChanged, this, &MainWindow::onPackageCheckStateChanged);
#endif
    connect(flatpakModel, &FlatpakModel::checkStateChanged, this, &MainWindow::onFlatpakCheckStateChanged);
    connect(snapModel, &SnapModel::checkStateChanged, this, &MainWindow::onSnapCheckStateChanged);
    // Use QStandardItemModel's built-in itemChanged signal for PopularModel
    connect(popularModel, &PopularModel::checkStateChanged, this, &MainWindow::onPopularItemChanged);
}

bool MainWindow::uninstall(const QString &names, const QStringList &preuninstall, const QStringList &postuninstall)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setCurrentWidget(ui->tabOutput);

    bool success = true;
    // Simulate removal of selections and present for confirmation. A canceled
    // confirmation is neither a successful nor failed operation.
    if (!confirmActions(names, "remove")) {
        operationCanceled = true;
        return true;
    }

    ui->tabWidget->setTabText(Tab::Output, tr("Uninstalling packages..."));
    enableOutput();

    if (!preuninstall.isEmpty()) {
        qDebug() << "Pre-uninstall";
        ui->tabWidget->setTabText(Tab::Output, tr("Running pre-uninstall operations..."));
        enableOutput();
        if (lockFile.isLockedGUI()) {
            return false;
        }
        success = runHooksAsRoot(cmd, preuninstall);
    }

    if (success) {
        enableOutput();
        if (lockFile.isLockedGUI()) {
            return false;
        }
        success = PackageBackend::removePackages(cmd, packageArgs(names));
    }

    if (success && !postuninstall.isEmpty()) {
        qDebug() << "Post-uninstall";
        ui->tabWidget->setTabText(Tab::Output, tr("Running post-uninstall operations..."));
        enableOutput();
        if (lockFile.isLockedGUI()) {
            return false;
        }
        success = runHooksAsRoot(cmd, postuninstall);
    }
    return success;
}

bool MainWindow::updateApt()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (lockFile.isLockedGUI()) {
        return false;
    }
    ui->tabOutput->isVisible() // Don't display in output if calling to refresh from tabs
        ? ui->tabWidget->setTabText(Tab::Output, tr("Refreshing sources..."))
        : progress->show();
    if (!timer.isActive()) {
        timer.start(100ms);
    }

    enableOutput();
    if (PackageBackend::refreshRepositories(cmd)) {
        qDebug() << "sources updated OK";
        updatedOnce = true;
        return true;
    }
    qDebug() << "problem updating sources";
    QMessageBox::critical(this, tr("Error"),
                          tr("There was a problem updating sources. Some sources may not have "
                             "provided updates. For more info check: ")
                              + "<a href=\"/var/log/mxpi.log\">/var/log/mxpi.log</a>");
    return false;
}

// Convert different size units to bytes
quint64 MainWindow::convert(const QString &size, bool *ok)
{
    return FlatpakModel::sizeStringToBytes(size, ok);
}

// Convert to string (#bytes, KiB, MiB, and GiB)
QString MainWindow::convert(quint64 bytes)
{
    auto size = static_cast<double>(bytes);
    if (bytes < KiB) {
        return QString::number(size) + " bytes";
    } else if (bytes < MiB) {
        return QString::number(size / KiB) + " KiB";
    } else if (bytes < GiB) {
        return QString::number(size / MiB, 'f', 1) + " MiB";
    } else {
        return QString::number(size / GiB, 'f', 2) + " GiB";
    }
}

void MainWindow::listSizeInstalledFP()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    auto sumSizes = [](const QList<QString> &sizes) {
        return std::accumulate(sizes.cbegin(), sizes.cend(), quint64(0),
                               [](quint64 acc, const QString &item) {
                                   bool ok = false;
                                   const quint64 bytes = convert(item, &ok);
                                   if (!ok && !item.isEmpty()) {
                                       qWarning() << "MainWindow: could not parse installed Flatpak size" << item;
                                   }
                                   // Skip sizes that failed to parse rather than folding a bogus
                                   // value (or a silently-wrong small one) into the running total.
                                   return ok ? acc + bytes : acc;
                               });
    };

    quint64 total = 0;
    if (cachedInstalledScope == fpUser && cachedInstalledFetched) {
        total = sumSizes(cachedInstalledSizeMap.values());
    } else {
        QScopedValueRollback<bool> guard(suppressCmdOutput, true);
        QStringList list = cmd.getOut("flatpak", flatpakArgsWithScope(fpUser, {"list", "--columns", "app,size"}),
                                      Cmd::QuietMode::No).split('\n', Qt::SkipEmptyParts);
        total = std::accumulate(list.cbegin(), list.cend(), quint64(0),
                                [](quint64 acc, const QString &item) {
                                    const QString sizeField = item.section('\t', 1);
                                    bool ok = false;
                                    const quint64 bytes = convert(sizeField, &ok);
                                    if (!ok && !sizeField.isEmpty()) {
                                        qWarning() << "MainWindow: could not parse installed Flatpak size" << sizeField;
                                    }
                                    return ok ? acc + bytes : acc;
                                });
    }
    ui->labelNumSize->setText(convert(total));
}

// Keep Flatpak UI enabled; rely on modal progress dialog to block interaction
void MainWindow::blockInterfaceFP()
{
    // Maintain cursor feedback without toggling widget enabled state
    const bool isBusy = displayFlatpaksIsRunning;
    setCursor(isBusy ? QCursor(Qt::BusyCursor) : QCursor(Qt::ArrowCursor));
}

// Update interface when changing Tab::Enabled, MX, Backports
void MainWindow::updateInterface()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (currentTree == ui->treePopularApps || currentTree == ui->treeFlatpak || currentTree == ui->treeSnap) {
        return;
    }
    if (displayPackagesIsRunning) {
        connect(this, &MainWindow::displayPackagesFinished, this, &MainWindow::updateInterface, Qt::UniqueConnection);
        return;
    }
    QApplication::restoreOverrideCursor();
    progress->hide();

    auto *model = getCurrentModel();
    if (!model) {
        return;
    }

    auto *proxy = getCurrentProxy();
    int upgradeCount = model->countByStatus(Status::Upgradable);
    int installCount = model->countByStatus(Status::Installed);
    int totalCount = (proxy && proxy->repoOnly()) ? proxy->rowCount() : model->rowCount();

    auto updateLabelsAndFocus = [&](QLabel *labelNumApps, QLabel *labelNumUpgrade, QLabel *labelNumInstall,
                                    QPushButton *pushForceUpdate, QLineEdit *searchBox) {
        labelNumApps->setText(QString::number(totalCount));
        labelNumUpgrade->setText(QString::number(upgradeCount));
        labelNumInstall->setText(QString::number(installCount + upgradeCount));
        pushForceUpdate->setEnabled(true);
        searchBox->setFocus();
    };

    switch (ui->tabWidget->currentIndex()) {
    case Tab::EnabledRepos:
        ui->pushUpgradeAll->setVisible(upgradeCount > 0);
        updateLabelsAndFocus(ui->labelNumApps, ui->labelNumUpgr, ui->labelNumInst, ui->pushForceUpdateEnabled,
                             ui->searchBoxEnabled);
        break;
    case Tab::Test:
        updateLabelsAndFocus(ui->labelNumApps_2, ui->labelNumUpgrMX, ui->labelNumInstMX, ui->pushForceUpdateMX,
                             ui->searchBoxMX);
        break;
    case Tab::Backports:
        updateLabelsAndFocus(ui->labelNumApps_3, ui->labelNumUpgrBP, ui->labelNumInstBP, ui->pushForceUpdateBP,
                             ui->searchBoxBP);
        break;
#ifdef PACKAGE_BACKEND_PACMAN
    case Tab::AUR:
        updateLabelsAndFocus(ui->labelNumAppsAUR, ui->labelNumUpgrAUR, ui->labelNumInstAUR, ui->pushForceUpdateAUR,
                             ui->searchBoxAUR);
        break;
#endif
    }
}

uchar MainWindow::getDebianVerNum()
{
    QFile file {"/etc/debian_version"};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "Could not open /etc/debian_version:" << file.errorString();
        return showVersionDialog(tr("Could not determine Debian version. Please select your version:"));
    }

    QTextStream in(&file);
    const QString version = in.readLine().split('/').at(0); // Handle cases like "bookworm/sid"
    file.close();

    // First try parsing as numeric version
    bool ok = false;
    const int numericVer = version.split('.').at(0).toInt(&ok);
    if (ok) {
        return numericVer;
    }

    // Then try matching codename
    const QString codename = version.toLower();
    if (codename == QLatin1String("bullseye")) {
        return Release::Bullseye;
    }
    if (codename == QLatin1String("bookworm")) {
        return Release::Bookworm;
    }
    if (codename == QLatin1String("trixie")) {
        return Release::Trixie;
    }

    qCritical() << "Unknown Debian version:" << version;
    return showVersionDialog(tr("Could not determine Debian version. Please select your version:"));
}

uchar MainWindow::showVersionDialog(const QString &message)
{
    QMessageBox msgBox;
    msgBox.setWindowTitle(tr("Debian Version"));
    msgBox.setText(message);
    const QAbstractButton *bookwormButton = msgBox.addButton("Bookworm", QMessageBox::AcceptRole);
    msgBox.addButton("Trixie", QMessageBox::AcceptRole);
    const QAbstractButton *cancelButton = msgBox.addButton(QMessageBox::Cancel);
    msgBox.show();

    // exec()'s return value is opaque for custom buttons (since Qt 6.5 it is no
    // longer the button's index), so identify the choice by the clicked button.
    msgBox.exec();

    if (msgBox.clickedButton() == cancelButton) {
        exit(EXIT_FAILURE);
    }
    return msgBox.clickedButton() == bookwormButton ? Release::Bookworm : Release::Trixie;
}

QString MainWindow::getDebianVerName(uchar version)
{
    if (version == 0) {
        version = getDebianVerNum();
    }
    static const std::map<uchar, QString> versionMap
        = {{Release::Jessie, QStringLiteral("jessie")},     {Release::Stretch, QStringLiteral("stretch")},
           {Release::Buster, QStringLiteral("buster")},     {Release::Bullseye, QStringLiteral("bullseye")},
           {Release::Bookworm, QStringLiteral("bookworm")}, {Release::Trixie, QStringLiteral("trixie")}};

    if (const auto it = versionMap.find(version); it != versionMap.end()) {
        return it->second;
    }

    qWarning() << "Error: Invalid Debian version, assumes bookworm";
    return QStringLiteral("bookworm");
}

QString MainWindow::getLocalizedName(const QDomElement &element) const
{
    const QString &localeName = locale.name();
    QStringList tagCandidates = {localeName, localeName.section('_', 0, 0), "en", "en_US"};

    for (const auto &tag : tagCandidates) {
        for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (child.tagName() == tag && !child.text().trimmed().isEmpty()) {
                return child.text().trimmed();
            }
        }
    }

    auto child = element.firstChildElement();
    return child.isNull() ? element.text().trimmed() : child.text().trimmed();
}

QString MainWindow::categoryTranslation(const QString &item)
{
    // Return original item for English locale
    if (locale.name() == QLatin1String("en_US")) {
        return item;
    }

    // Try full locale name (e.g. "fr_FR") then language code only (e.g. "fr")
    const QStringList tagCandidates = {locale.name(), locale.name().section('_', 0, 0)};

    dictionary.beginGroup(item);
    for (const auto &tag : tagCandidates) {
        const QString translation = dictionary.value(tag).toString();
        if (!translation.isEmpty()) {
            dictionary.endGroup();
            return translation;
        }
    }
    dictionary.endGroup();

    return item; // Fallback to original if no translation found
}

QString MainWindow::getArchOption() const
{
    static const QMap<QString, QString> archMap {
        {"amd64", "--arch=x86_64"}, {"i386", "--arch=i386"}, {"armhf", "--arch=arm"}, {"arm64", "--arch=aarch64"}};
    return archMap.value(arch, QString()) + ' ';
}

void MainWindow::updateBar()
{
    QApplication::processEvents();
    bar->setValue((bar->value() + 1) % bar->maximum() + 1);
}

void MainWindow::checkUncheckItem()
{
    auto *currentTreeView = qobject_cast<QTreeView *>(focusWidget());

    if (!currentTreeView || !currentTreeView->currentIndex().isValid()) {
        return;
    }

    QModelIndex currentIndex = currentTreeView->currentIndex();

    // For popular apps, skip categories (items with children)
    if (currentTreeView == ui->treePopularApps) {
        if (!popularModel || !popularProxy) {
            return;
        }

        // Map from proxy to source model
        QModelIndex sourceIndex = popularProxy->mapToSource(currentIndex);
        if (!sourceIndex.isValid() || popularModel->hasChildren(sourceIndex)) {
            return; // Skip categories
        }

        // Get the check column index in the source model
        QModelIndex checkIndex = popularModel->index(sourceIndex.row(), PopCol::Check, sourceIndex.parent());
        Qt::CheckState currentState = static_cast<Qt::CheckState>(checkIndex.data(Qt::CheckStateRole).toInt());
        Qt::CheckState newState = (currentState == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        popularModel->setData(checkIndex, newState, Qt::CheckStateRole);
    } else if (currentTreeView == ui->treeFlatpak) {
        if (flatpakModel) {
            QModelIndex sourceIndex = flatpakProxy->mapToSource(currentIndex);
            QModelIndex checkIndex = flatpakModel->index(sourceIndex.row(), FlatCol::Check);
            Qt::CheckState currentState = static_cast<Qt::CheckState>(checkIndex.data(Qt::CheckStateRole).toInt());
            Qt::CheckState newState = (currentState == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            flatpakModel->setData(checkIndex, newState, Qt::CheckStateRole);
        }
    } else if (currentTreeView == ui->treeSnap) {
        if (snapModel) {
            QModelIndex sourceIndex = snapProxy->mapToSource(currentIndex);
            QModelIndex checkIndex = snapModel->index(sourceIndex.row(), SnapCol::Check);
            Qt::CheckState currentState = static_cast<Qt::CheckState>(checkIndex.data(Qt::CheckStateRole).toInt());
            Qt::CheckState newState = (currentState == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            snapModel->setData(checkIndex, newState, Qt::CheckStateRole);
        }
    } else {
        // APT trees
        auto *model = getCurrentModel();
        auto *proxy = getCurrentProxy();
        if (model && proxy) {
            QModelIndex sourceIndex = proxy->mapToSource(currentIndex);
            QModelIndex checkIndex = model->index(sourceIndex.row(), TreeCol::Check);
            Qt::CheckState currentState = static_cast<Qt::CheckState>(checkIndex.data(Qt::CheckStateRole).toInt());
            Qt::CheckState newState = (currentState == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            model->setData(checkIndex, newState, Qt::CheckStateRole);
        }
    }
}

void MainWindow::outputAvailable(const QString &output)
{
#ifdef PACKAGE_BACKEND_PACMAN
    // AUR installs elevate via a raw `sudo` inside paru (see install()), so its
    // password prompt shows up as plain text here rather than pkexec's own dialog.
    // Mask ui->lineEdit before the user can type their password into it.
    //
    // readyReadStandardOutput chunk boundaries are arbitrary, so "sudo ... password"
    // can straddle two calls to this function -- match against a small carry-over
    // tail plus the new text rather than the new chunk alone, so a split prompt (or a
    // wrong-password re-prompt arriving right after unmasking on Enter) still gets
    // caught. The tail is bounded and doesn't include already-matched text, so it
    // can't keep re-triggering on stale output once the buffer moves past a prompt.
    static const QRegularExpression sudoPrompt {R"(sudo.*password)", QRegularExpression::CaseInsensitiveOption};
    constexpr qsizetype maxSudoPromptTail = 64;
    const QString combined = sudoPromptTail + output;
    if (sudoPrompt.match(combined).hasMatch()) {
        setLineEditMasked(true);
        // Drop the tail so this same (now-handled) match can't keep re-triggering
        // on later, unrelated output that happens to still carry it forward.
        sudoPromptTail.clear();
    } else {
        sudoPromptTail = combined.right(qMin(combined.size(), maxSudoPromptTail));
    }
#endif
    outputRenderer.append(output);
}

void MainWindow::loadPmFiles()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    const QString pmFolderPath {QStringLiteral("/usr/share/mx-packageinstaller-pkglist")};
    const QStringList pmFileList = QDir(pmFolderPath).entryList({"*.pm"});

    for (const QString &fileName : pmFileList) {
        QFile file(pmFolderPath + '/' + fileName);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Could not open file:" << file.fileName();
            continue;
        }

        QDomDocument doc;
        if (!doc.setContent(&file)) {
            qWarning() << "Could not load document:" << fileName << "-- not valid XML?";
            file.close();
            continue;
        }

        processDoc(doc);
        file.close();
    }
}

// Process DOM documents (from .pm files)
void MainWindow::processDoc(const QDomDocument &doc)
{
    PopularInfo info;
    QDomElement root = doc.firstChildElement("app");
    QDomElement element = root.firstChildElement();

    // Optimization: Use static lookup table instead of repeated string comparisons
    static const QHash<QString, int> tagLookup = {{"category", 0},
                                                  {"name", 1},
                                                  {"description", 2},
                                                  {"installable", 3},
                                                  {"screenshot", 4},
                                                  {"preinstall", 5},
                                                  {"install_package_names", 6},
                                                  {"postinstall", 7},
                                                  {"uninstall_package_names", 8},
                                                  {"postuninstall", 9},
                                                  {"preuninstall", 10}};

    while (!element.isNull()) {
        const QString tagName = element.tagName();

        // Fast lookup instead of multiple string comparisons
        if (auto it = tagLookup.find(tagName); it != tagLookup.end()) {
            QString trimmedText = element.text().trimmed();

            switch (it.value()) {
            case 0: // category
                info.category = categoryTranslation(trimmedText);
                break;
            case 1: // name
                info.name = std::move(trimmedText);
                break;
            case 2: // description
                info.description = getLocalizedName(element);
                break;
            case 3: // installable
                info.installable = std::move(trimmedText);
                break;
            case 4: // screenshot
                info.screenshot = std::move(trimmedText);
                break;
            case 5: // preinstall
                info.preInstall = std::move(trimmedText);
                break;
            case 6: // install_package_names
                info.installNames = trimmedText.replace('\n', ' ');
                break;
            case 7: // postinstall
                info.postInstall = std::move(trimmedText);
                break;
            case 8: // uninstall_package_names
                info.uninstallNames = std::move(trimmedText);
                break;
            case 9: // postuninstall
                info.postUninstall = std::move(trimmedText);
                break;
            case 10: // preuninstall
                info.preUninstall = std::move(trimmedText);
                break;
            }
        }
        element = element.nextSiblingElement();
    }

    const QString modArch = mapArchToFormat(arch);
    if (isPackageInstallable(info.installable, modArch)) {
        popularApps.append(info);
    }
}

QString MainWindow::mapArchToFormat(const QString &arch) const
{
    static const QMap<QString, QString> archMapping
        = {{"amd64", "64"}, {"i386", "32"}, {"armhf", "armhf"}, {"arm64", "armsixtyfour"}};

    return archMapping.value(arch, QString());
}

bool MainWindow::isPackageInstallable(const QString &installable, const QString &modArch) const
{
    return installable.split(',').contains(modArch) || installable == QLatin1String("all");
}

namespace
{
struct ParsedFlatpakRef {
    QString ref;
    bool isRuntime {false};
};

QString canonicalFlatpakRef(const QString &ref)
{
    QString cleaned = ref.trimmed();
    if (cleaned.startsWith(QLatin1String("app/")) || cleaned.startsWith(QLatin1String("runtime/"))) {
        cleaned = cleaned.section('/', 1);
    }
    return cleaned;
}

bool isRuntimeToken(const QString &token)
{
    return token.startsWith(QLatin1String("runtime/")) || token.contains(QLatin1String(".runtime/")) || token.contains(QLatin1String(".Platform"));
}

ParsedFlatpakRef parseInstalledFlatpakLine(const QString &line)
{
    static const QRegularExpression refRegex(R"((app|runtime)/\S+)");

    const QRegularExpressionMatch match = refRegex.match(line);
    if (match.hasMatch()) {
        const QString refWithType = match.captured(0);
        return {.ref = refWithType.section('/', 1), .isRuntime = refWithType.startsWith(QLatin1String("runtime/"))};
    }

    const QStringList tokens = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (token.contains('/')) {
            const bool isRuntime = isRuntimeToken(token);
            // If the token already lacks the app/runtime prefix, keep it intact
            const bool hasTypePrefix = token.startsWith(QLatin1String("app/")) || token.startsWith(QLatin1String("runtime/"));
            QString ref = hasTypePrefix ? token.section('/', 1) : token;
            return {.ref = ref.trimmed(), .isRuntime = isRuntime};
        }
    }

    // Fallback: return the line as-is if it looks like a ref without type prefix
    const QString fallbackRef = line.contains('/') ? line.trimmed() : QString();
    const bool isRuntime = isRuntimeToken(fallbackRef);
    return {.ref = fallbackRef, .isRuntime = isRuntime};
}

struct RemoteLsEntry {
    QString version;
    QString branch;
    QString ref;
    QString size;
};

RemoteLsEntry parseRemoteLsLine(const QString &line)
{
    RemoteLsEntry entry;

    const QStringList tabPartsRaw = line.split('\t', Qt::KeepEmptyParts);
    QStringList tabParts;
    tabParts.reserve(tabPartsRaw.size());
    for (const QString &part : tabPartsRaw) {
        tabParts.append(part.trimmed());
    }

    auto finalizeEntry = [&entry]() {
        if (entry.branch.isEmpty() && !entry.ref.isEmpty()) {
            entry.branch = entry.ref.section('/', -1);
        }
        if ((entry.version.isEmpty() || entry.version.contains('/')) && !entry.ref.isEmpty()) {
            entry.version = entry.ref.section('/', -1);
        }
        return entry;
    };

    if (tabParts.size() >= 4) {
        entry.version = tabParts.at(0);
        entry.branch = tabParts.at(1);
        entry.ref = tabParts.at(2);
        entry.size = tabParts.at(3);
        return finalizeEntry();
    }
    if (tabParts.size() == 3) {
        entry.version = tabParts.at(0);
        const QString possibleBranchOrRef = tabParts.at(1);
        if (possibleBranchOrRef.count('/') >= 2) {
            entry.ref = possibleBranchOrRef;
            entry.size = tabParts.at(2);
        } else {
            entry.branch = possibleBranchOrRef;
            entry.ref = tabParts.at(2);
        }
        return finalizeEntry();
    }

    QStringList tokens = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    int refIndex = -1;
    for (int i = 0; i < tokens.size(); ++i) {
        if (tokens.at(i).count('/') >= 2) { // Looks like a Flatpak ref
            entry.ref = tokens.at(i);
            refIndex = i;
            break;
        }
    }

    if (!entry.ref.isEmpty()) {
        entry.version = tokens.value(0);
        entry.branch = tokens.value(1);
        if (refIndex >= 0 && refIndex + 1 < tokens.size()) {
            entry.size = tokens.mid(refIndex + 1).join(' ');
        }
        return finalizeEntry();
    }

    entry.ref = line.trimmed();
    return finalizeEntry();
}
} // namespace

void MainWindow::refreshPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    disableOutput();
    // Don't clear the model here - setPopularApps() handles it with proper reset signals
    ui->searchPopular->clear();
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);
    installedPackages = listInstalled();
    displayPopularApps();
}

// Handles duplicate Flatpak entries by adding context to their display names
void MainWindow::removeDuplicatesFP()
{
    if (flatpakModel) {
        flatpakModel->markDuplicates();
    }
}

void MainWindow::setConnections()
{
    // Direct connection: aboutToQuit is emitted after the event loop has exited,
    // so a queued invocation would never be dispatched and cleanup would not run.
    connect(QApplication::instance(), &QApplication::aboutToQuit, this, &MainWindow::cleanup);
    // Connect search boxes
    connect(ui->searchPopular, &QLineEdit::textChanged, this, &MainWindow::findPopular);
    connect(ui->searchBoxEnabled, &QLineEdit::textChanged, this, &MainWindow::findPackage);
    connect(ui->searchBoxMX, &QLineEdit::textChanged, this, &MainWindow::findPackage);
    connect(ui->searchBoxBP, &QLineEdit::textChanged, this, &MainWindow::findPackage);
    connect(ui->searchBoxFlatpak, &QLineEdit::textChanged, this, &MainWindow::findPackage);
    connect(ui->searchBoxSnap, &QLineEdit::textChanged, this, &MainWindow::findPackage);
    connect(ui->searchBoxSnap, &QLineEdit::returnPressed, this, &MainWindow::searchSnapStore);
#ifdef PACKAGE_BACKEND_PACMAN
    // Unlike the other search boxes (client-side filtering of an already-loaded
    // list), AUR search re-queries paru, so it's Enter-triggered rather than
    // live-as-you-type; textChanged only handles resetting to the installed list
    // when the box is cleared.
    connect(ui->searchBoxAUR, &QLineEdit::returnPressed, this, [this] { handleAurTab(ui->searchBoxAUR->text()); });
    connect(ui->searchBoxAUR, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty() && currentTree == ui->treeAUR) {
            handleAurTab(QString());
        }
    });
#endif
    // Connect combo filters
    connect(ui->comboFilterEnabled, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterMX, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterBP, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterFlatpak, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterSnap, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
#ifdef PACKAGE_BACKEND_PACMAN
    connect(ui->comboFilterAUR, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
#endif

    // Connect other UI elements to their respective slots
    connect(ui->checkHideLibs, &QCheckBox::toggled, this, &MainWindow::checkHideLibs_toggled);
    connect(ui->checkHideLibsBP, &QCheckBox::clicked, this, &MainWindow::checkHideLibsBP_clicked);
    connect(ui->checkHideLibsMX, &QCheckBox::clicked, this, &MainWindow::checkHideLibsMX_clicked);
    connect(ui->checkRepoOnlyMX, &QCheckBox::clicked, this, &MainWindow::checkRepoOnlyMX_clicked);
    connect(ui->checkRepoOnlyBP, &QCheckBox::clicked, this, &MainWindow::checkRepoOnlyBP_clicked);
    connect(ui->comboRemote, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::comboRemote_activated);
    connect(ui->comboUser, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::comboUser_currentIndexChanged);
    connect(ui->lineEdit, &QLineEdit::returnPressed, this, &MainWindow::lineEdit_returnPressed);
#ifdef PACKAGE_BACKEND_PACMAN
    if (!lineEditToggleMaskAction) {
        const QIcon hiddenIcon = QIcon::fromTheme("view-hidden", QIcon::fromTheme("view-visible"));
        lineEditToggleMaskAction = ui->lineEdit->addAction(hiddenIcon, QLineEdit::TrailingPosition);
        lineEditToggleMaskAction->setCheckable(true);
        lineEditToggleMaskAction->setToolTip(tr("Show/hide input"));
        connect(lineEditToggleMaskAction, &QAction::toggled, this, [this](bool checked) { setLineEditMasked(checked); });

        const QIcon clearIcon = QIcon::fromTheme("edit-clear");
        lineEditClearAction = ui->lineEdit->addAction(clearIcon, QLineEdit::TrailingPosition);
        lineEditClearAction->setToolTip(tr("Clear input"));
        connect(lineEditClearAction, &QAction::triggered, ui->lineEdit, &QLineEdit::clear);
    }
#endif
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAbout_clicked);
    connect(ui->pushCancel, &QPushButton::clicked, this, &MainWindow::pushCancel_clicked);
    connect(ui->pushEnter, &QPushButton::clicked, this, &MainWindow::pushEnter_clicked);
    connect(ui->pushForceUpdateBP, &QPushButton::clicked, this, &MainWindow::pushForceUpdateBP_clicked);
    connect(ui->pushForceUpdateEnabled, &QPushButton::clicked, this, &MainWindow::pushForceUpdateEnabled_clicked);
    connect(ui->pushForceUpdateMX, &QPushButton::clicked, this, &MainWindow::pushForceUpdateMX_clicked);
    connect(ui->pushForceUpdateFP, &QPushButton::clicked, this, &MainWindow::pushForceUpdateFP_clicked);
#ifdef PACKAGE_BACKEND_PACMAN
    connect(ui->pushForceUpdateAUR, &QPushButton::clicked, this, &MainWindow::pushForceUpdateAUR_clicked);
#endif
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelp_clicked);
    connect(ui->pushInstall, &QPushButton::clicked, this, &MainWindow::pushInstall_clicked);
    connect(ui->pushRemotes, &QPushButton::clicked, this, &MainWindow::pushRemotes_clicked);
    connect(ui->pushRemoveAutoremovable, &QPushButton::clicked, this, &MainWindow::pushRemoveAutoremovable_clicked);
    connect(ui->pushRemoveUnused, &QPushButton::clicked, this, &MainWindow::pushRemoveUnused_clicked);
    connect(ui->pushUninstall, &QPushButton::clicked, this, &MainWindow::pushUninstall_clicked);
    connect(ui->pushUpgradeAll, &QPushButton::clicked, this, &MainWindow::pushUpgradeAll_clicked);
    connect(ui->pushUpgradeFP, &QPushButton::clicked, this, &MainWindow::pushUpgradeFP_clicked);
    connect(ui->pushRefreshSnap, &QPushButton::clicked, this, &MainWindow::pushRefreshSnap_clicked);
    connect(ui->pushUpgradeSnap, &QPushButton::clicked, this, &MainWindow::pushUpgradeSnap_clicked);
    connect(ui->pushSetupSnapd, &QPushButton::clicked, this, &MainWindow::pushSetupSnapd_clicked);
    connect(ui->tabWidget, QOverload<int>::of(&QTabWidget::currentChanged), this,
            &MainWindow::tabWidget_currentChanged);
    // Header checkbox (Upgradable): select all
    connect(headerEnabled, &CheckableHeaderView::toggled, this, &MainWindow::selectAllUpgradable_toggled);
    connect(headerMX, &CheckableHeaderView::toggled, this, &MainWindow::selectAllUpgradable_toggled);
    connect(headerBP, &CheckableHeaderView::toggled, this, &MainWindow::selectAllUpgradable_toggled);
#ifdef PACKAGE_BACKEND_PACMAN
    connect(headerAUR, &CheckableHeaderView::toggled, this, &MainWindow::selectAllUpgradable_toggled);
#endif

    // Connect popular apps tree view
    connect(ui->treePopularApps, &QTreeView::customContextMenuRequested, this,
            &MainWindow::treePopularApps_customContextMenuRequested);
    connect(ui->treePopularApps, &QTreeView::collapsed, this, &MainWindow::treePopularApps_itemCollapsed);
    connect(ui->treePopularApps, &QTreeView::expanded, this, &MainWindow::treePopularApps_expanded);
    connect(ui->treePopularApps, &QTreeView::expanded, this, &MainWindow::treePopularApps_itemExpanded);
    // Only show info dialog when clicking on the Info column (not the entire row)
    connect(ui->treePopularApps, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() == PopCol::Info) {
            displayPopularInfo(index);
        }
    });
}

void MainWindow::setProgressDialog()
{
    progress = new QProgressDialog(this);
    bar = new QProgressBar(progress);
    bar->setMaximum(bar->maximum());
    pushCancel = new QPushButton(tr("Cancel"));
    connect(pushCancel, &QPushButton::clicked, this, &MainWindow::cancelDownload);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowStaysOnTopHint);
    progress->setCancelButton(pushCancel);
    pushCancel->setDisabled(true);
    progress->setLabelText(tr("Please wait..."));
    progress->setAutoClose(false);
    progress->setBar(bar);
    bar->setTextVisible(false);
    progress->reset();
}

void MainWindow::setSearchFocus() const
{
    static const QMap<int, QLineEdit *> searchBoxMap {{Tab::EnabledRepos, ui->searchBoxEnabled},
                                                      {Tab::Test, ui->searchBoxMX},
                                                      {Tab::Backports, ui->searchBoxBP},
                                                      {Tab::Flatpak, ui->searchBoxFlatpak},
                                                      {Tab::Snap, ui->searchBoxSnap},
                                                      {Tab::Popular, ui->searchPopular}};
    const auto index = ui->tabWidget->currentIndex();
    if (auto *searchBox = searchBoxMap.value(index)) {
        searchBox->setFocus();
    }
}

void MainWindow::displayPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    if (!popularModel || !ui->treePopularApps) {
        qWarning() << "PopularModel or treePopularApps not initialized!";
        return;
    }

    // Convert PopularInfo to PopularAppData
    QList<PopularAppData> apps;
    apps.reserve(popularApps.size());

    for (const auto &item : popularApps) {
        PopularAppData data;
        data.category = item.category;
        data.name = item.name;
        data.description = item.description;
        data.installNames = item.installNames;
        data.uninstallNames = item.uninstallNames;
        data.screenshot = item.screenshot;
        data.postUninstall = item.postUninstall;
        data.preUninstall = item.preUninstall;
        data.isInstalled = checkInstalled(item.uninstallNames);

        apps.append(data);
    }

    popularModel->setPopularApps(apps);

    // Enable sorting on Name and Description columns only
    ui->treePopularApps->setSortingEnabled(true);
    ui->treePopularApps->header()->setSortIndicatorShown(true);
    ui->treePopularApps->header()->setSectionsClickable(true);
    ui->treePopularApps->header()->setSectionResizeMode(PopCol::Category, QHeaderView::Interactive);
    ui->treePopularApps->header()->setSectionResizeMode(PopCol::Check, QHeaderView::Interactive);
    ui->treePopularApps->header()->setSectionResizeMode(PopCol::Info, QHeaderView::Fixed);
    // Default header sort indicator (children default to Name)
    ui->treePopularApps->header()->setSortIndicator(PopCol::Name, Qt::AscendingOrder);
    // Keep categories sorted A-Z by their label; child sorting handled separately
    ui->treePopularApps->sortByColumn(PopCol::Category, Qt::AscendingOrder);

    // Apply category spanning
    applyPopularCategorySpanning();

    // Set appropriate widths for columns
    ui->treePopularApps->setColumnWidth(PopCol::Category, 120);  // Category column
    ui->treePopularApps->setColumnWidth(PopCol::Check, 40);  // Checkbox column
    ui->treePopularApps->setColumnWidth(PopCol::Info, 30);   // Info icon column

    // Let Name and Description take remaining space
    ui->treePopularApps->header()->setStretchLastSection(false);
    ui->treePopularApps->header()->setSectionResizeMode(PopCol::Name, QHeaderView::Interactive);
    ui->treePopularApps->header()->setSectionResizeMode(PopCol::Description, QHeaderView::Stretch);
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
}

void MainWindow::displayFilteredFP(QStringList list, bool raw)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    if (!flatpakModel || !flatpakProxy) {
        return;
    }

    auto normalizeRef = [](const QString &line) {
        const RemoteLsEntry entry = parseRemoteLsLine(line);
        QString ref = entry.ref.trimmed();
        if (ref.startsWith(QLatin1String("app/")) || ref.startsWith(QLatin1String("runtime/"))) {
            ref = ref.section('/', 1); // Strip leading type segment (app/runtime)
        }
        return ref;
    };

    QMutableStringListIterator i(list);
    if (raw) { // Raw format that needs to be edited
        while (i.hasNext()) {
            i.setValue(normalizeRef(i.next()));
        }
    }

    // Build set of canonical refs for filtering
    QSet<QString> refSet;
    for (const QString &ref : std::as_const(list)) {
        refSet.insert(canonicalFlatpakRef(ref));
    }

    // Reset status filtering when showing explicit ref subsets.
    flatpakProxy->setStatusFilter(0);
    flatpakProxy->setAllowedRefs(refSet);

    // Update buttons based on current selection
    if (changeList.isEmpty()) {
        ui->pushUninstall->setEnabled(false);
        ui->pushInstall->setEnabled(false);
    }

    // Scroll to last clicked item if valid
    if (lastIndexClicked.isValid()) {
        ui->treeFlatpak->scrollTo(lastIndexClicked);
    }

    ui->labelNumAppFP->setText(QString::number(flatpakProxy->rowCount()));
    blockInterfaceFP();

    // Auto-adjust column widths after filter changes for Flatpak tab
    for (int i = 0; i < flatpakModel->columnCount(); ++i) {
        ui->treeFlatpak->resizeColumnToContents(i);
    }
}

void MainWindow::displayPackages()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    displayPackagesIsRunning = true;

    auto *model = getCurrentModel();
    auto *list = getCurrentList();

    if (!model || !list) {
        displayPackagesIsRunning = false;
        emit displayPackagesFinished();
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);

    // Disable updates to prevent flickering of unsorted data
    if (currentTree) {
        currentTree->setUpdatesEnabled(false);
    }

#ifdef PACKAGE_BACKEND_PACMAN
    if (currentTree == ui->treeEnabled && installedPackages.isEmpty()) {
        // installedPackages ("pacman -Qi") may still be in flight from the startup
        // preload (see the constructor) -- if so, createPackageDataList()'s merge of
        // installed-but-not-in-repo packages below silently finds nothing to add.
        // Remember that so the installedPackages fetch's own completion can redo this
        // render once the data it was missing has actually arrived (see
        // enabledReposRenderedWithoutInstalled's declaration).
        enabledReposRenderedWithoutInstalled = true;
    }
#endif

    // Build package data list and set on model
    QVector<PackageData> packages = createPackageDataList(list);
    model->setPackageData(packages);

    // Update installed versions
    updatePackageStatuses();

    // Sort by Package Name (column 1) after data is loaded
    auto *proxy = getCurrentProxy();
    if (proxy) {
        proxy->sort(1, Qt::AscendingOrder);
    }

    // Re-enable updates after sorting
    if (currentTree) {
        currentTree->setUpdatesEnabled(true);
    }

    // Bind the model now: callers such as rebuildPackageViews() switch currentTree
    // temporarily and restore it before this queued call runs.
    QMetaObject::invokeMethod(this, [this, model] { displayAutoremovable(model); }, Qt::QueuedConnection);

    QApplication::restoreOverrideCursor();

    displayPackagesIsRunning = false;
    emit displayPackagesFinished();
}

#ifdef PACKAGE_BACKEND_PACMAN
// See enabledReposRenderedWithoutInstalled's declaration: called once installedPackages
// ("pacman -Qi") actually lands, from the constructor's fetch completion.
void MainWindow::handleInstalledPackagesArrived()
{
    if (!enabledReposRenderedWithoutInstalled) {
        return;
    }
    // The Enabled Repos view already rendered once (see displayPackages()) while this
    // fetch was still in flight, so createPackageDataList()'s merge of
    // installed-but-not-in-repo packages silently found nothing to add. Force the next
    // render of that tab to redo the merge now that the data is here -- dirtyEnabledRepos
    // makes handleEnabledReposTab() go back through buildPackageLists()/displayPackages()
    // instead of taking the "nothing changed" fast path, whether that happens right now
    // (still on the tab) or on a later visit (dirtyEnabledRepos persists until then).
    enabledReposRenderedWithoutInstalled = false;
    dirtyEnabledRepos = true;
    if (currentTree == ui->treeEnabled) {
        handleEnabledReposTab(ui->searchBoxEnabled->text());
    }
}
#endif

void MainWindow::displayAutoremovable(PackageModel *model)
{
#ifdef PACKAGE_BACKEND_PACMAN
    if (skipInitialAutoremovableCheck) {
        skipInitialAutoremovableCheck = false;
        return;
    }
#endif
    const QStringList names = PackageBackend::autoremovableCandidates(cmd);

    ui->pushRemoveAutoremovable->setVisible(!names.isEmpty());
    if (names.isEmpty()) {
        return;
    }

    model->setAutoremovable(names);
}

MainWindow::AptTabContext MainWindow::currentAptTab()
{
    if (currentTree == ui->treeMXtest) {
        return {mxtestModel, mxtestProxy, &mxList, ui->comboFilterMX};
    }
    if (currentTree == ui->treeBackports) {
        return {backportsModel, backportsProxy, &backportsList, ui->comboFilterBP};
    }
    if (currentTree == ui->treeEnabled) {
        return {enabledModel, enabledProxy, &enabledList, ui->comboFilterEnabled};
    }
#ifdef PACKAGE_BACKEND_PACMAN
    if (currentTree == ui->treeAUR) {
        return {aurModel, aurProxy, &aurList, ui->comboFilterAUR};
    }
#endif
    return {};
}

bool MainWindow::currentTabFiltersAutoremovable()
{
    const QComboBox *filterCombo = currentAptTab().filterCombo;
    return filterCombo && filterCombo->currentText() == tr("Autoremovable");
}

PackageModel *MainWindow::getCurrentModel()
{
    return currentAptTab().model;
}

PackageFilterProxy *MainWindow::getCurrentProxy()
{
    return currentAptTab().proxy;
}

QHash<QString, PackageInfo> *MainWindow::getCurrentList()
{
    auto ctx = currentAptTab();
    return ctx.list ? ctx.list : &enabledList;
}

QVector<PackageData> MainWindow::createPackageDataList(QHash<QString, PackageInfo> *list) const
{
    QVector<PackageData> packages;

#ifdef PACKAGE_BACKEND_PACMAN
    // aurList is a small, deliberately curated view (either the installed-from-AUR
    // set or live search results) -- not "every AUR package", unlike enabledList's
    // repo-wide candidate set. Merging in every other installed package here (the
    // way the repo tabs do, to surface installed packages missing from their repo
    // candidate list) would flood the AUR tab with every native package instead.
    if (currentTree == ui->treeAUR) {
        packages.reserve(list->size());
        for (const auto &[name, info] : std::as_const(*list).asKeyValueRange()) {
            PackageData pkg = createPackageData(name, info.version, info.description);
            pkg.fromRepo = true;
            packages.append(pkg);
        }
        return packages;
    }
#endif

    packages.reserve(list->size() + installedPackages.size());

    for (const auto &[name, info] : std::as_const(*list).asKeyValueRange()) {
        PackageData pkg = createPackageData(name, info.version, info.description);
        pkg.fromRepo = true;
        packages.append(pkg);
    }

    for (const auto &[name, info] : installedPackages.asKeyValueRange()) {
        if (!list->contains(name)) {
            packages.append(createPackageData(name, QString(), info.description));
        }
    }

    return packages;
}

void MainWindow::updatePackageStatuses()
{
    auto *model = getCurrentModel();
    if (!model) {
        return;
    }

    const auto installedVersions = listInstalledVersions();

    // Build a hash map of installed version strings
    QHash<QString, QString> versionStrings;
    versionStrings.reserve(installedVersions.size());
    for (const auto &[name, version] : installedVersions.asKeyValueRange()) {
        versionStrings.insert(name, version.toString());
    }

    model->updateInstalledVersions(versionStrings);

    // Resize columns after updating statuses
    if (currentTree && currentTree != ui->treePopularApps && currentTree != ui->treeFlatpak) {
        for (int i = 0; i < model->columnCount(); ++i) {
            if (!currentTree->isColumnHidden(i)) {
                currentTree->resizeColumnToContents(i);
            }
        }
    }
}

void MainWindow::displayFlatpaks(bool forceUpdate)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (!flatpaks.isEmpty() && !forceUpdate) {
        return;
    }

    // Every synchronous Flatpak (re)load -- comboRemote_activated(), comboUser_
    // currentIndexChanged(), etc. -- goes through here, so bumping the generation
    // here covers all of them uniformly. This runs to completion before returning
    // control to the event loop, so no comparison against the old value is needed
    // on this path; it just needs to invalidate any older async completion still in
    // flight from the constructor's Flatpak startup preload (see there).
    ++flatpakRequestGeneration;

#ifdef MXPI_TESTING
    // testSkipFlatpakPreload (see the MXPI_TESTING constructor overload):
    // Testing/test_mainwindow.cpp needs to verify this function bumps the
    // generation as its first action without going on to touch real flatpak
    // remotes -- loadFlatpakData()'s "flatpak update --appstream" fallback can
    // genuinely reach the network, which a test run must not do.
    if (testSkipFlatpakPreload) {
        return;
    }
#endif

    setupFlatpakDisplay();
    loadFlatpakData();
    populateFlatpakTree();
    finalizeFlatpakDisplay();
}

void MainWindow::showFlatpakProgress(const QString &label)
{
    progress->setLabelText(label);
    pushCancel->setEnabled(false);
    progress->show();
    if (!timer.isActive()) {
        timer.start(100ms);
    }
}

void MainWindow::setupFlatpakDisplay()
{
    ui->treeFlatpak->setUpdatesEnabled(false);
    displayFlatpaksIsRunning = true;
    lastIndexClicked = QModelIndex();

    const bool isCurrentTabFlatpak = ui->tabWidget->currentIndex() == Tab::Flatpak;
    if (isCurrentTabFlatpak) {
        setCursor(QCursor(Qt::BusyCursor));
        if (!flatpakCancelHidden && pushCancel) {
            pushCancel->setEnabled(false);
            pushCancel->hide();
            flatpakCancelHidden = true;
        }
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
    }

    listFlatpakRemotes();
    if (flatpakModel) {
        flatpakModel->clear();
    }
    changeList.clear();
    blockInterfaceFP();
}

void MainWindow::loadFlatpakData()
{
    const QStringList remoteEntries = listFlatpaks(ui->comboRemote->currentText(), fpUser);

    // Optimize: Get all installed packages with one command (ref + size), then split by type
    QScopedValueRollback<bool> guard(suppressCmdOutput, true);
    const QStringList allInstalled
        = cmd.getOut("flatpak", flatpakArgsWithScope(fpUser, {"list", "--columns=ref,size"}), Cmd::QuietMode::No,
                     /*discardStderr=*/true)
              .split('\n', Qt::SkipEmptyParts);

    processFlatpakData(remoteEntries, allInstalled, fpUser);
}

// Turns already-fetched remote-ls/list output into the member state populateFlatpakTree()
// renders. Split out from loadFlatpakData() so the startup preload can run the two
// subprocess calls above on a background thread (via local Cmd instances) and hand the
// raw results here to finish on the GUI thread -- this part is pure string processing,
// safe either way, but the member writes need to happen on the GUI thread.
void MainWindow::processFlatpakData(const QStringList &remoteEntries, const QStringList &allInstalled,
                                    const QString &scope)
{
    flatpaks = remoteEntries;
    flatpaksApps.clear();
    flatpaksRuntimes.clear();
    QSet<QString> installedCanonical;
    cachedInstalledFlatpaks.clear();
    cachedInstalledSizeMap.clear();
    cachedInstalledScope.clear();
    cachedInstalledFetched = false;

    cachedInstalledFlatpaks = allInstalled;
    cachedInstalledScope = scope;
    cachedInstalledFetched = true;

    // Clear and reserve space for better performance
    installedAppsFP.clear();
    installedRuntimesFP.clear();
    installedAppsFP.reserve(allInstalled.size() / 2);
    installedRuntimesFP.reserve(allInstalled.size() / 2);

    // Split by type based on flatpak naming convention
    for (const QString &itemRaw : allInstalled) {
        if (itemRaw.startsWith(QLatin1String("Ref"))) { // header row on some versions
            continue;
        }

        const ParsedFlatpakRef parsed = parseInstalledFlatpakLine(itemRaw.section('\t', 0, 0));
        if (parsed.ref.isEmpty()) {
            continue;
        }

        const QString canonicalRef = canonicalFlatpakRef(parsed.ref);
        if (canonicalRef.isEmpty()) {
            continue;
        }
        installedCanonical.insert(canonicalRef);

        const QString sizeStr = itemRaw.section('\t', 1);
        if (!sizeStr.isEmpty()) {
            cachedInstalledSizeMap.insert(canonicalRef, sizeStr);
        }

        if (parsed.isRuntime) {
            installedRuntimesFP.append(canonicalRef);
        } else {
            installedAppsFP.append(canonicalRef);
        }
    }

    // Ensure installed refs are present in the display even if missing from remote listings
    QSet<QString> listedCanonicalRefs;
    for (const QString &entry : std::as_const(flatpaks)) {
        const RemoteLsEntry parsed = parseRemoteLsLine(entry);
        const QString canonical = canonicalFlatpakRef(parsed.ref);
        if (!canonical.isEmpty()) {
            listedCanonicalRefs.insert(canonical);
        }
    }

    const auto buildEntry = [](const QString &ref) {
        const QString branch = ref.section('/', -1);
        const QString version = branch;
        return version + '\t' + branch + '\t' + ref + '\t';
    };

    for (const QString &ref : std::as_const(installedCanonical)) {
        if (!listedCanonicalRefs.contains(ref)) {
            flatpaks.append(buildEntry(ref));
        }
    }

    // Build cached app/runtime lists from the already-fetched remote data to avoid re-querying
    flatpaksApps.clear();
    flatpaksRuntimes.clear();
    for (const QString &entry : std::as_const(flatpaks)) {
        const RemoteLsEntry parsed = parseRemoteLsLine(entry);
        const QString refForType = !parsed.ref.isEmpty() ? parsed.ref : canonicalFlatpakRef(parsed.ref);
        const bool isRuntime = isRuntimeToken(refForType) || isRuntimeToken(canonicalFlatpakRef(refForType));
        (isRuntime ? flatpaksRuntimes : flatpaksApps).append(entry);
    }
}

void MainWindow::populateFlatpakTree()
{
    if (!flatpakModel) {
        return;
    }

    const QStringList installedAll = installedAppsFP + installedRuntimesFP;
    QVector<FlatpakData> flatpakDataList;
    flatpakDataList.reserve(flatpaks.size());

    for (const QString &item : std::as_const(flatpaks)) {
        FlatpakData data = createFlatpakData(item, installedAll);
        if (!data.canonicalRef.isEmpty()) {
            flatpakDataList.append(data);
        }
    }

    flatpakModel->setFlatpakData(flatpakDataList);
    flatpakModel->updateInstalledStatus(installedAll);
    flatpakModel->setInstalledSizes(cachedInstalledSizeMap);

    updateFlatpakCounts(flatpakDataList.size());
    formatFlatpakTree();
}

FlatpakData MainWindow::createFlatpakData(const QString &item, const QStringList &installedAll) const
{
    FlatpakData data;
    const RemoteLsEntry entry = parseRemoteLsLine(item);

    const QString originalRef = entry.ref.trimmed();
    QString ref = originalRef;
    const QString branch = entry.branch.isEmpty() ? ref.section('/', -1) : entry.branch;
    QString version = entry.version;
    if (version.isEmpty() || version.contains('/')) {
        version = branch;
    }
    const QString size = entry.size;
    const QString canonicalRef = canonicalFlatpakRef(ref);
    if (canonicalRef.isEmpty()) {
        return data; // Return empty data
    }
    const QString longName = canonicalRef.section('/', 0, 0);
    const QString shortName = longName.section('.', -1);

    // Skip unwanted packages
    static const QSet<QString> unwantedPackages
        = {QStringLiteral("Locale"), QStringLiteral("Sources"), QStringLiteral("Debug")};
    if (unwantedPackages.contains(shortName)) {
        return data; // Return empty data
    }

    data.shortName = shortName;
    data.longName = longName;
    data.version = version;
    data.branch = branch;
    data.size = size;
    data.fullName = originalRef.isEmpty() ? canonicalRef : originalRef;
    data.canonicalRef = canonicalRef;
    data.status = installedAll.contains(canonicalRef) ? Status::Installed : Status::NotInstalled;

    return data;
}

void MainWindow::updateFlatpakCounts(uint totalCount)
{
    listSizeInstalledFP();
    ui->labelNumAppFP->setText(QString::number(totalCount));
    ui->labelNumInstFP->setText(QString::number(!installedAppsFP.isEmpty() ? installedAppsFP.count() : 0));
}

void MainWindow::formatFlatpakTree()
{
    ui->treeFlatpak->sortByColumn(FlatCol::Name, Qt::AscendingOrder);
    removeDuplicatesFP();

    if (flatpakModel) {
        for (int i = 0; i < flatpakModel->columnCount(); ++i) {
            ui->treeFlatpak->resizeColumnToContents(i);
        }
    }
}

void MainWindow::finalizeFlatpakDisplay()
{
    ui->treeFlatpak->blockSignals(false);

    const bool isCurrentTabFlatpak = ui->tabWidget->currentIndex() == Tab::Flatpak;
    if (isCurrentTabFlatpak) {
        if (!ui->comboFilterFlatpak->currentText().isEmpty()) {
            filterChanged(ui->comboFilterFlatpak->currentText());
        }
        ui->searchBoxFlatpak->setFocus();
    }

    displayFlatpaksIsRunning = false;
    firstRunFP = false;
    blockInterfaceFP();
    if (holdProgressForFlatpakRefresh) {
        holdProgressForFlatpakRefresh = false;
        progress->hide();
    }
    if (flatpakCancelHidden && pushCancel) {
        pushCancel->show();
        pushCancel->setEnabled(true);
        flatpakCancelHidden = false;
    }
    ui->treeFlatpak->setUpdatesEnabled(true);
}

void MainWindow::displayWarning(const QString &repo)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    bool *displayed = nullptr;
    QString msg;
    QString key;

    if (repo == QLatin1String("test")) {
        displayed = &warningTest;
        key = QStringLiteral("NoWarningTest");
        msg = tr("You are about to use the MX Test repository, whose packages are provided for "
                 "testing purposes only. It is possible that they might break your system, so it "
                 "is suggested that you back up your system and install or update only one package "
                 "at a time. Please provide feedback in the Forum so the package can be evaluated "
                 "before moving up to Main.");

    } else if (repo == QLatin1String("backports")) {
        displayed = &warningBackports;
        key = QStringLiteral("NoWarningBackports");
        msg = tr("You are about to use Debian Backports, which contains packages taken from the next "
                 "Debian release (called 'testing'), adjusted and recompiled for usage on Debian stable. "
                 "They cannot be tested as extensively as in the stable releases of Debian and MX Linux, "
                 "and are provided on an as-is basis, with risk of incompatibilities with other components "
                 "in Debian stable. Use with care!");
    } else if (repo == QLatin1String("flatpaks")) {
        displayed = &warningFlatpaks;
        key = QStringLiteral("NoWarningFlatpaks");
        msg = tr("MX Linux includes this repository of flatpaks for the users' convenience only, and "
                 "is not responsible for the functionality of the individual flatpaks themselves. "
                 "For more, consult flatpaks in the Wiki.");
#ifdef PACKAGE_BACKEND_PACMAN
    } else if (repo == QLatin1String("aur")) {
        displayed = &warningAur;
        key = QStringLiteral("NoWarningAur");
        msg = tr("You are about to use the AUR, which contains user-contributed packages. "
                 "These packages are not vetted by the distribution, may be outdated, and could "
                 "contain malicious or broken build scripts. Use with care.");
#endif
    }
    if ((displayed == nullptr) || *displayed || settings.value(key, false).toBool()) {
        return;
    }

    QMessageBox msgBox(QMessageBox::Warning, tr("Warning"), msg);
    msgBox.addButton(QMessageBox::Close);
    auto *cb = new QCheckBox(tr("Do not show this message again"));
    msgBox.setCheckBox(cb);
    msgBox.exec();
    settings.setValue(key, cb->isChecked());
    settings.sync();
    *displayed = true;
}

void MainWindow::ifDownloadFailed() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    progress->hide();
}

void MainWindow::invalidateFlatpakRemoteCache()
{
    cachedFlatpakRemotes.clear();
    cachedFlatpakRemotesScope.clear();
    cachedFlatpakRemotesFetched = false;
}

void MainWindow::listFlatpakRemotes() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QString currentRemote = ui->comboRemote->currentText();
    ui->comboRemote->blockSignals(true);
    ui->comboRemote->clear();

    auto applyRemotes = [&](const QStringList &list) {
        ui->comboRemote->addItems(list);
        QString savedRemote = firstRunFP ? settings.value("FlatpakRemote", "flathub").toString() : currentRemote;
        ui->comboRemote->setCurrentText(savedRemote.isEmpty() ? "flathub" : savedRemote);
        ui->comboRemote->blockSignals(false);
    };

    if (cachedFlatpakRemotesFetched && cachedFlatpakRemotesScope == fpUser) {
        applyRemotes(cachedFlatpakRemotes);
        return;
    }

    bool listOk = false;
    const QStringList list = fetchFlatpakRemotes(fpUser, &listOk);
    if (!listOk) {
        ui->comboRemote->blockSignals(false);
        return;
    }

    cachedFlatpakRemotes = list;
    cachedFlatpakRemotesScope = fpUser;
    cachedFlatpakRemotesFetched = true;

    applyRemotes(list);
}

bool MainWindow::confirmActions(const QString &names, const QString &action)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    qDebug() << "names" << names << "and" << changeList;

#ifdef PACKAGE_BACKEND_PACMAN
    Q_UNUSED(action);
    // No apt-get -s/aptitude dependency-simulation equivalent on this backend;
    // pacman would need to actually resolve/download to preview what it'd do, so
    // just confirm the explicit selection instead.
    QMessageBox msgBox;
    msgBox.setText("<b>" + tr("The following packages were selected. Click Show Details for list of changes.")
                   + "</b>");
    msgBox.setInformativeText('\n' + names);
    msgBox.setDetailedText(changeList.join('\n'));
    msgBox.addButton(QMessageBox::Ok);
    msgBox.addButton(QMessageBox::Cancel);

    constexpr int PacmanDialogMinWidth = 600;
    auto *pacmanSpacer = new QSpacerItem(PacmanDialogMinWidth, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    if (auto *pacmanLayout = qobject_cast<QGridLayout *>(msgBox.layout())) {
        pacmanLayout->addItem(pacmanSpacer, 0, 1);
    }
    return msgBox.exec() == QMessageBox::Ok;
#else
    QString msg;

    QString detailedNames;
    QStringList detailedInstalledNames;
    QString detailedToInstall;
    QString detailedRemovedNames;
    QString aptitudeInfo;

    // DEBIAN_FRONTEND/LANG only matter to apt-get/aptitude themselves, so they're
    // scoped to just those pipeline stages below, not the grep/awk stages.
    const QHash<QString, QString> debconfAndLangEnv {{"DEBIAN_FRONTEND", debconfFrontend()}, {"LANG", "C"}};
    if (currentTree == ui->treeFlatpak && names != QLatin1String("flatpak")) {
        detailedInstalledNames = changeList;
    } else {
        // Determine recommends flags and target based on current tab
        QCheckBox *recommendsCheck = ui->checkBoxInstallRecommends;
        QStringList target;
        bool reinstall = true;
        if (currentTree == ui->treeBackports) {
            recommendsCheck = ui->checkBoxInstallRecommendsBP;
            target = {"-t", verName + "-backports"};
        } else if (currentTree == ui->treeMXtest) {
            recommendsCheck = ui->checkBoxInstallRecommendsMX;
            target = {"-t", "mx"};
            reinstall = false;
        }
        const QString recommends = recommendsCheck->isChecked() ? QStringLiteral("--install-recommends")
                                                                 : QStringLiteral("--no-install-recommends");
        const QString recommendsAptitude = recommendsCheck->isChecked() ? QStringLiteral("--with-recommends")
                                                                         : QStringLiteral("--without-recommends");
        const QStringList packageNames = names.split(QLatin1Char(' '), Qt::SkipEmptyParts);

        // The AWK script below isolates the three fields the confirmation dialog
        // needs (package name, version/parenthetical, install-or-remove marker)
        // out of apt-get's -s simulation output; kept as a single literal argv
        // element, exactly as it was a single shell-quoted argument before.
        static const QString awkFilter =
            QStringLiteral(R"awk({V=""; P="";}; $3 ~ /^\[/ { V=$3 }; $3 ~ /^\(/ { P=$3 ")"}; $4 ~ /^\(/ {P=" => " $4 ")"};  {print $2 ";" V  P ";" $1})awk");

        QStringList aptArgs {"-s", "-V", "-o=Dpkg::Use-Pty=0", action, recommends};
        aptArgs += target;
        if (reinstall) {
            aptArgs << QStringLiteral("--reinstall");
        }
        aptArgs += packageNames;

        QString pipelineOutput;
        cmd.pipeline({
            {"apt-get", aptArgs, debconfAndLangEnv},
            {"grep", {QStringLiteral("Inst\\|Remv")}},
            {"awk", {awkFilter}},
        }, &pipelineOutput);
        detailedNames = pipelineOutput;
        {
            QStringList aptitudeArgs {"-sy", "-V", "-o=Dpkg::Use-Pty=0", action, recommendsAptitude};
            aptitudeArgs += target;
            aptitudeArgs += packageNames;
            const QStringList aptLines = cmd.getOut(debconfAndLangEnv, QStringLiteral("aptitude"), aptitudeArgs)
                                              .split('\n', Qt::KeepEmptyParts);
            if (aptLines.isEmpty()) {
                aptitudeInfo.clear();
            } else if (aptLines.size() >= 2) {
                aptitudeInfo = aptLines.at(aptLines.size() - 2);
            } else {
                aptitudeInfo = aptLines.constFirst();
            }
        }
    }

    if (currentTree != ui->treeFlatpak) {
        detailedInstalledNames = detailedNames.split('\n');
    }

    detailedInstalledNames.sort();
    qDebug() << "detailed installed names sorted " << detailedInstalledNames;
    QStringListIterator iterator(detailedInstalledNames);

    if (currentTree != ui->treeFlatpak) {
        while (iterator.hasNext()) {
            QString value = iterator.next();
            if (value.contains(QLatin1String("Remv"))) {
                value = value.section(';', 0, 0) + ' ' + value.section(';', 1, 1);
                detailedRemovedNames = detailedRemovedNames + value + '\n';
            }
            if (value.contains(QLatin1String("Inst"))) {
                value = value.section(';', 0, 0) + ' ' + value.section(';', 1, 1);
                detailedToInstall = detailedToInstall + value + '\n';
            }
        }
        if (!detailedRemovedNames.isEmpty()) {
            detailedRemovedNames.prepend(tr("Remove") + '\n');
        }
        if (!detailedToInstall.isEmpty()) {
            detailedToInstall.prepend(tr("Install") + '\n');
        }
    } else {
        if (action == QLatin1String("remove")) {
            detailedRemovedNames = changeList.join('\n');
            detailedToInstall.clear();
        }
        if (action == QLatin1String("install")) {
            detailedToInstall = changeList.join('\n');
            detailedRemovedNames.clear();
        }
    }

    msg = "<b>" + tr("The following packages were selected. Click Show Details for list of changes.") + "</b>";

    QMessageBox msgBox;
    msgBox.setText(msg);
    msgBox.setInformativeText('\n' + names + "\n\n" + aptitudeInfo);

    if (action == QLatin1String("install")) {
        msgBox.setDetailedText(detailedToInstall + '\n' + detailedRemovedNames);
    } else {
        msgBox.setDetailedText(detailedRemovedNames + '\n' + detailedToInstall);
    }

    // Find Detailed Info box and set height between 100-400 depending on length of content
    constexpr int MinDetailHeight = 100;
    constexpr int MaxDetailHeight = 400;
    auto *const detailedInfo = msgBox.findChild<QTextEdit *>();
    if (detailedInfo) {
        const auto recommended = qMax(msgBox.detailedText().length() / 2, MinDetailHeight);
        const auto height = qMin(recommended, MaxDetailHeight);
        detailedInfo->setFixedHeight(height);
    }

    msgBox.addButton(QMessageBox::Ok);
    msgBox.addButton(QMessageBox::Cancel);

    constexpr int DialogMinWidth = 600;
    auto *horizontalSpacer = new QSpacerItem(DialogMinWidth, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    auto *layout = qobject_cast<QGridLayout *>(msgBox.layout());
    if (layout) {
        layout->addItem(horizontalSpacer, 0, 1);
    }
    return msgBox.exec() == QMessageBox::Ok;
#endif
}

bool MainWindow::install(const QString &names)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    if (!isOnline()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    ui->tabWidget->setTabText(Tab::Output, tr("Installing packages..."));

    // Simulate install of selections and present for confirmation. A canceled
    // confirmation is neither a successful nor failed operation.
    if (!confirmActions(names, "install")) {
        operationCanceled = true;
        return true;
    }
    enableOutput();

#ifdef PACKAGE_BACKEND_PACMAN
    if (currentTree == ui->treeAUR) {
        // AUR packages are built unprivileged (makepkg/paru refuse to run as root),
        // so this never goes through the privileged Cmd::procAsRoot path at all --
        // no lock check either, since paru/pacman manage their own locking.
        const QString paruPath = getParuPath();
        if (paruPath.isEmpty()) {
            QMessageBox::critical(this, tr("Error"),
                                  tr("paru is not installed.\n\n"
                                     "To install AUR packages, please install paru first:\n"
                                     "pacman -S paru\n\n"
                                     "Then try installing the AUR package again."));
            return false;
        }

        QStringList paruArgs {"-S", "--needed"};
        paruArgs += packageArgs(names);
        const bool hasScript = QFile::exists("/usr/bin/script");

        if (getuid() == 0) {
            // If the app itself runs as root (e.g. WSL2 without a working pkexec),
            // drop to a normal user for the build via `runuser -u` (no login shell),
            // forcing a POSIX shell and that user's HOME so it doesn't depend on
            // their login shell understanding inline VAR=val syntax. paru still
            // elevates its own pacman step via sudo, prompting in the output box.
            const QString buildUser = loginUserForRoot();
            if (buildUser.isEmpty()) {
                QMessageBox::critical(this, tr("Error"),
                                      tr("AUR packages cannot be built as root, and no regular user could be "
                                         "found to build as.\n\n"
                                         "Install the package as your normal user instead."));
                return false;
            }
            const QString home = homeDirForUser(buildUser);
            QStringList envArgs;
            if (!home.isEmpty()) {
                envArgs << ("HOME=" + home);
            }
            envArgs << QStringLiteral("SHELL=/bin/sh") << QStringLiteral("LANG=C") << QStringLiteral("PAGER=cat")
                    << QStringLiteral("PARU_PAGER=cat");

            // runuser/env are both plain argv-vector execs (no shell involved), so this
            // whole chain needs no quoting of its own -- except the innermost `script
            // -qefc` argument, which is unavoidably a single string (see procOnPty()'s
            // doc comment in cmd.h for why `script` itself has no argv-vector mode).
            QStringList runuserArgs {"-u", buildUser, "--", "env"};
            runuserArgs += envArgs;
            if (hasScript) {
                QStringList paruInvocation {paruPath};
                paruInvocation += paruArgs;
                runuserArgs << QStringLiteral("/usr/bin/script") << QStringLiteral("-qefc")
                            << shellCommandFromArgs(paruInvocation) << QStringLiteral("/dev/null");
            } else {
                runuserArgs << paruPath;
                runuserArgs += paruArgs;
            }
            return cmd.proc(QStringLiteral("runuser"), runuserArgs);
        }

        if (hasScript) {
            QStringList envArgs {"LANG=C", "PAGER=cat", "PARU_PAGER=cat", paruPath};
            envArgs += paruArgs;
            return cmd.procOnPty(QStringLiteral("env"), envArgs);
        }
        return cmd.procWithEnv({{"LANG", "C"}, {"PAGER", "cat"}, {"PARU_PAGER", "cat"}}, paruPath, paruArgs);
    }
#endif

    if (lockFile.isLockedGUI()) {
        return false;
    }

    // Determine recommends flag and target based on current tab
    QCheckBox *recommendsCheck = ui->checkBoxInstallRecommends;
    QStringList extraArgs;
    if (currentTree == ui->treeBackports) {
        recommendsCheck = ui->checkBoxInstallRecommendsBP;
        extraArgs = {"-t", verName + "-backports", "--reinstall"};
    } else if (currentTree == ui->treeMXtest) {
        recommendsCheck = ui->checkBoxInstallRecommendsMX;
        extraArgs = {"-t", "mx"};
    } else {
        extraArgs = {"--reinstall"};
    }

    const QString recommends = recommendsCheck->isChecked() ? "--install-recommends" : "--no-install-recommends";
    QStringList backendArgs {recommends};
    backendArgs += extraArgs;
    return PackageBackend::installPackages(cmd, packageArgs(names), backendArgs);
}

// Install a list of application and run postprocess for each of them.
bool MainWindow::installBatch(const QStringList &nameList)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bool result = true;
    QStringList postinstallHooks;
    QString installNames;

    for (const QString &name : nameList) {
        for (const auto &item : std::as_const(popularApps)) {
            if (item.name == name) {
                if (!item.postInstall.isEmpty()) {
                    postinstallHooks << item.postInstall;
                }
                installNames += item.installNames + ' ';
            }
        }
    }

    if (!installNames.isEmpty()) {
        if (!install(installNames)) {
            result = false;
        }
    }
    if (operationWasCanceled()) {
        return false;
    }

    if (!postinstallHooks.isEmpty()) {
        qDebug() << "Post-install";
        ui->tabWidget->setTabText(Tab::Output, tr("Post-processing..."));
        if (lockFile.isLockedGUI()) {
            return false;
        }
        enableOutput();
        if (!runHooksAsRoot(cmd, postinstallHooks)) {
            result = false;
        }
    }
    return result;
}

bool MainWindow::installPopularApp(const QString &name)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bool result = true;
    QString preinstall;
    QString postinstall;
    QString installNames;

    // Get all the app info
    for (const auto &item : std::as_const(popularApps)) {
        if (item.name == name) {
            preinstall = item.preInstall;
            postinstall = item.postInstall;
            installNames = item.installNames;
        }
    }
    enableOutput();
    // A preinstall hook can add a temporary third-party repo (tempList). Remove it on
    // every return path below, including cancellation and lock failures, so it never
    // stays an active apt source for the rest of the session.
    const auto removeTempList = qScopeGuard([this] {
        if (QFile::exists(tempList)) {
            Cmd helperCmd;
            runMxpiMaintenanceAsRoot(helperCmd, QStringLiteral("cleanup_temp"));
            updateApt();
        }
    });
    // Preinstall
    if (!preinstall.isEmpty()) {
        qDebug() << "Pre-install";
        ui->tabWidget->setTabText(Tab::Output, tr("Pre-processing for ") + name);
        if (lockFile.isLockedGUI()) {
            return false;
        }
        if (!cmd.runHookAsRoot(preinstall)) {
            return false;
        }
    }
    // Install
    if (!installNames.isEmpty()) {
        ui->tabWidget->setTabText(Tab::Output, tr("Installing ") + name);
        result = install(installNames);
    }
    if (operationWasCanceled()) {
        return false;
    }
    enableOutput();
    // Postinstall
    if (!postinstall.isEmpty()) {
        qDebug() << "Post-install";
        ui->tabWidget->setTabText(Tab::Output, tr("Post-processing for ") + name);
        if (lockFile.isLockedGUI()) {
            return false;
        }
        if (!cmd.runHookAsRoot(postinstall)) {
            result = false;
        }
    }
    return result;
}

bool MainWindow::installPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QStringList batchNames;
    bool result = true;

    if (!isOnline()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    if (!updatedOnce) {
        updateApt();
    }
    if (operationWasCanceled()) {
        return false;
    }

    if (!popularModel) {
        return false;
    }

    // Get checked items from model
    QStringList checkedApps = popularModel->checkedPackageNames();

    // Make a list of apps to be installed together (those without preinstall)
    for (const QString &name : checkedApps) {
        for (const auto &item : std::as_const(popularApps)) {
            if (item.name == name) {
                const QString &preinstall = item.preInstall;
                if (preinstall.isEmpty()) { // Add to batch processing if there is no preinstall command
                    batchNames << name;
                }
                break;
            }
        }
    }

    if (!installBatch(batchNames)) {
        result = false;
    }
    if (operationWasCanceled()) {
        setCursor(QCursor(Qt::ArrowCursor));
        return false;
    }

    // Install the rest of the apps (those with preinstall)
    for (const QString &name : checkedApps) {
        if (!batchNames.contains(name)) {
            if (!installPopularApp(name)) {
                result = false;
            }
            if (operationWasCanceled()) {
                break;
            }
        }
    }
    setCursor(QCursor(Qt::ArrowCursor));

    ui->treePopularApps->clearSelection();
    popularModel->uncheckAll();
    return result;
}

bool MainWindow::installSelected()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabEnabled(Tab::Output, true);
    QString names = changeList.join(' ');

    // Change sources as needed
    if (currentTree == ui->treeMXtest) {
        // Add testrepo unless already enabled
        if (!testInitiallyEnabled) {
            if (!cmd.writeFileAsRoot(tempList, mxTestSourceLine(getMXTestRepoUrl(), verName, arch), Cmd::QuietMode::Yes)) {
                return false;
            }
        }
        if (!updateApt()) {
            if (QFile::exists(tempList)) {
                Cmd helperCmd;
                runMxpiMaintenanceAsRoot(helperCmd, QStringLiteral("cleanup_temp"));
            }
            return false;
        }
    } else if (currentTree == ui->treeBackports) {
        if (!cmd.writeFileAsRoot(tempList, backportsSourceLine(verName), Cmd::QuietMode::Yes)) {
            return false;
        }
        if (!updateApt()) {
            Cmd helperCmd;
            runMxpiMaintenanceAsRoot(helperCmd, QStringLiteral("cleanup_temp"));
            return false;
        }
    }
    bool result = install(names);
    if (currentTree == ui->treeBackports || currentTree == ui->treeMXtest) {
        if (QFile::exists(tempList)) {
            Cmd helperCmd;
            runMxpiMaintenanceAsRoot(helperCmd, QStringLiteral("cleanup_temp"));
            updateApt();
        }
    }
    changeList.clear();
    installedPackages = listInstalled();
    return result;
}

bool MainWindow::markKeep()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabEnabled(Tab::Output, true);
    QString names = changeList.join(' ');
    enableOutput();
    return PackageBackend::markManuallyInstalled(cmd, packageArgs(names));
}

bool MainWindow::isOnline()
{
    if (settings.value("skiponlinecheck", false).toBool() || args.isSet("skip-online-check")) {
        return true;
    }

    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", QApplication::applicationName().toUtf8() + '/'
                                           + QApplication::applicationVersion().toUtf8() + " (linux-gnu)");
    // Set the timeout on the request itself so it applies to the very first HEAD;
    // setting it on the manager only affects requests started afterwards, and would
    // also leak the timeout onto later downloads that share this manager.
    // int overload (msec); the std::chrono overload needs Qt >= 6.7, Bookworm has 6.4
    request.setTransferTimeout(settings.value("timeout", 7000).toInt());

    for (const QString address : {"https://mxrepo.com", "https://google.com"}) {
        QNetworkProxyQuery query {QUrl(address)};
        QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        if (!proxies.isEmpty()) {
            manager.setProxy(proxies.first());
        }
        request.setUrl(QUrl(address));
        QNetworkReply *onlineReply = manager.head(request);
        QEventLoop loop;
        connect(onlineReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(onlineReply, &QNetworkReply::errorOccurred, &loop, &QEventLoop::quit);
        loop.exec();
        onlineReply->disconnect();
        if (onlineReply->error() == QNetworkReply::NoError) {
            onlineReply->deleteLater();
            return true;
        }
        // Clean up failed reply before next iteration
        onlineReply->deleteLater();
    }
    qDebug() << "No network detected";
    return false;
}

bool MainWindow::downloadFile(const QString &url, QFile &file)
{
    qDebug() << "... downloading: " << url;
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Could not open file:" << file.fileName();
        return false;
    }

    QNetworkRequest request {QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", QApplication::applicationName().toUtf8() + '/'
                                           + QApplication::applicationVersion().toUtf8() + " (linux-gnu)");
    // Bound stalled package-list transfers; the same setting is used by the
    // connectivity probe above, while keeping this request-local for screenshots
    // and other network operations sharing the manager.
    request.setTransferTimeout(settings.value("timeout", 7000).toInt());

    downloadCancelRequested = false;
    bool writeFailed = false;
    QNetworkReply *dlReply = manager.get(request);
    activeDownloadReply = dlReply;
    auto clearActiveReply = qScopeGuard([this, dlReply] {
        if (activeDownloadReply == dlReply) {
            activeDownloadReply = nullptr;
        }
        downloadCancelRequested = false;
    });
    QEventLoop loop;

    connect(dlReply, &QNetworkReply::readyRead, this, [&file, dlReply, &writeFailed]() {
        if (file.write(dlReply->readAll()) == -1) {
            writeFailed = true;
            qDebug() << "Failed to write data to file:" << file.fileName();
            dlReply->abort();
            file.close();
            file.remove();
        }
    });
    connect(dlReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    file.close();

    if (dlReply->error() != QNetworkReply::NoError) {
        const bool timedOut = dlReply->error() == QNetworkReply::TimeoutError
            || (dlReply->error() == QNetworkReply::OperationCanceledError && !downloadCancelRequested && !writeFailed);
        if (!downloadCancelRequested || writeFailed) {
            const QString message = timedOut
                ? tr("The download timed out. Please check your internet connection and try again.")
                : tr("There was an error downloading or writing the file: %1. Please check your internet connection "
                     "and free space on your drive")
                      .arg(file.fileName());
            QMessageBox::warning(this, tr("Error"), message);
        }
        qDebug() << "There was an error downloading the file:" << url << "Error:" << dlReply->errorString();
        file.remove();
        dlReply->deleteLater();
        return false;
    }
    dlReply->deleteLater();
    return true;
}

bool MainWindow::downloadAndUnzip(const QString &url, QFile &file)
{
    if (!downloadFile(url, file)) {
        // Clean up both compressed and potential uncompressed files
        const QString basePath = QFileInfo(file.fileName()).path();
        const QString baseName = QFileInfo(file.fileName()).baseName();
        file.remove();
        QFile::remove(basePath + '/' + baseName);
        return false;
    }

    // Determine and execute unzip command based on file extension
    const QString fileExt = QFileInfo(file).suffix();
    const QString unzipProgram = (fileExt == QLatin1String("gz")) ? QStringLiteral("gunzip") : QStringLiteral("unxz");

    if (!cmd.proc(unzipProgram, {"-f", file.fileName()})) {
        qDebug() << "Could not unzip file:" << file.fileName();
        file.remove();
        return false;
    }

    return true;
}

bool MainWindow::downloadAndUnzip(const QString &url, const QString &repoName, const QString &branch,
                                  const QString &format, QFile &file)
{
    return downloadAndUnzip(url + repoName + branch + "/binary-" + arch + "/Packages." + format, file);
}

bool MainWindow::buildPackageLists(bool forceDownload)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (forceDownload) {
        setDirty();
    }
    clearUi();
    if (!downloadPackageList(forceDownload)) {
        ifDownloadFailed();
        return false;
    }
    if (displayPackagesIsRunning) {
        // pacman only: downloadPackageList() kicked off a background fetch instead
        // of completing inline. Its own completion callback finishes the job
        // (readPackageList()+displayPackages()+dirty-flag reset) once the data is
        // ready; nothing left to do here.
        return true;
    }
    if (!readPackageList(forceDownload)) {
        ifDownloadFailed();
        return false;
    }
    displayPackages();

    // Reset dirty flag for current tree after successful load
    if (currentTree == ui->treeEnabled) {
        dirtyEnabledRepos = false;
    } else if (currentTree == ui->treeMXtest) {
        dirtyTest = false;
    } else if (currentTree == ui->treeBackports) {
        dirtyBackports = false;
    }

    return true;
}

// Download the Packages.gz from sources
bool MainWindow::downloadPackageList(bool forceDownload)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (!isOnline()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    if (!tempDir.isValid()) {
        qDebug() << "Can't create temp folder";
        return false;
    }
    progress->setLabelText(tr("Downloading package info..."));
    pushCancel->setEnabled(true);

    auto runUpdateApt = [this, forceDownload]() {
        QScopedValueRollback<bool> holdGuard(holdProgressForAptRefresh, holdProgressForAptRefresh || forceDownload);
        const bool ok = updateApt();
        return ok;
    };

#ifdef PACKAGE_BACKEND_PACMAN
    // pacman has no local archive files to download/parse -- just (re-)query the
    // sync DBs directly. Test/MX/Backports don't exist on this backend (those tabs
    // are hidden), so there's nothing else for this function to do.
    if (displayPackagesIsRunning) {
        // A fetch is already in flight (e.g. a rapid rebuildPackageViews() call
        // racing the startup preload) -- don't kick off a second concurrent
        // "pacman -Ss", the one already running will refresh the data shortly.
        return true;
    }
    if (enabledList.isEmpty() || forceDownload) {
        if (forceDownload && !runUpdateApt()) {
            return false;
        }
        // "pacman -Ss" dumps every package in the sync DBs (tens of thousands of
        // lines) -- measured ~600-900ms on a full repo, enough to notice at boot
        // since pacman's landing tab is Enabled Repos. pacmanAvailablePackages()
        // uses its own local Cmd instance (not the shared member), so it's safe to
        // run on a background thread, same as listInstalled()'s startup preload.
        // buildPackageLists() (the only caller) checks displayPackagesIsRunning
        // and returns without touching readPackageList()/displayPackages() itself
        // when it's set -- this completion callback does that part once the data
        // is ready, via a temporary currentTree swap (same idiom rebuildPackageViews()
        // uses) so it targets Enabled Repos regardless of which tab is current by
        // the time the fetch finishes.
        displayPackagesIsRunning = true;
        [[maybe_unused]] auto future = QtConcurrent::run(
            [this, forceDownload, guardMutex = destructionGuardMutex, destructingFlag = destructing] {
            bool ok = false;
            auto loaded = pacmanAvailablePackages(&ok);
            QMutexLocker locker(guardMutex.get());
            if (*destructingFlag) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, ok, forceDownload, loaded = std::move(loaded)]() mutable {
                    if (!ok) {
                        displayPackagesIsRunning = false;
                        emit displayPackagesFinished();
                        QMessageBox::critical(this, tr("Error"),
                                              tr("Could not download the list of packages. Please check your "
                                                 "APT sources."));
                        return;
                    }
                    enabledList = std::move(loaded);
                    QTreeView *savedTree = currentTree;
                    currentTree = ui->treeEnabled;
                    // Always true for currentTree == treeEnabled (see readPackageList()'s
                    // own early return); nothing to branch on, just satisfying [[nodiscard]].
                    static_cast<void>(readPackageList(forceDownload));
                    displayPackages(); // resets displayPackagesIsRunning and emits displayPackagesFinished
                    dirtyEnabledRepos = false;
                    currentTree = savedTree;
                    if (currentTree == ui->treeEnabled) {
                        // Still on (or back to) this tab -- redo the same "data just
                        // became available" UI touches a synchronous load would have,
                        // guarded by data-readiness so it's a no-op if not applicable.
                        handleEnabledReposTab(ui->searchBoxEnabled->text());
                    }
                },
                Qt::QueuedConnection);
        });
        return true;
    }
    return true;
#else
    // Handle enabled list download/update
    if (enabledList.isEmpty() || forceDownload) {
        if (forceDownload && !runUpdateApt()) {
            return false;
        }
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
        AptCache cache;
        enabledList = cache.getCandidates();
        if (enabledList.isEmpty()) {
            runUpdateApt();
            enabledList = AptCache().getCandidates();
        }
    }

    // Handle MX test repo packages
    if (currentTree == ui->treeMXtest) {
        const QString mxPackagesPath = tempDir.path() + "/mxPackages";
        if (!QFile::exists(mxPackagesPath) || forceDownload) {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
            }

            QFile file(mxPackagesPath + ".gz");
            QString url = getMXTestRepoUrl();
            if (!downloadAndUnzip(url, verName, "/test", "gz", file)) {
                return false;
            }
        }
    }
    // Handle backports packages
    else if (currentTree == ui->treeBackports) {
        const QStringList components = {"main", "contrib", "non-free"};
        const QString basePath = tempDir.path() + "/";
        bool needsDownload = forceDownload;

        // Check if any package files are missing
        for (const QString &component : components) {
            if (!QFile::exists(basePath + component + "Packages")) {
                needsDownload = true;
                break;
            }
        }

        if (needsDownload) {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
            }

            // Download and process each component
            const QString url = QStringLiteral("https://deb.debian.org/debian/dists/");
            for (const QString &component : components) {
                QFile file(basePath + component + "Packages.xz");
                const QString branch = QStringLiteral("-backports/") + component;
                if (!downloadAndUnzip(url, verName, branch, "xz", file)) {
                    return false;
                }
            }

            // Combine all package files
            pushCancel->setDisabled(true);
            QFile outputFile(tempDir.filePath("allPackages"));
            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                qWarning() << "Could not open:" << outputFile.fileName();
                return false;
            }

            QTextStream outStream(&outputFile);
            bool success = true;
            for (const QString &component : components) {
                QFile inputFile(basePath + component + "Packages");
                if (!inputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    qWarning() << "Could not read file:" << inputFile.fileName();
                    success = false;
                    break;
                }
                outStream << inputFile.readAll();
                inputFile.close();
            }
            outputFile.close();
            if (!success) {
                return false;
            }
        }
    }
    return true;
#endif
}

void MainWindow::enableTabs(bool enable)
{
    if (enable) {
        operationInProgress = false;
    }
    for (int tab = 0; tab < ui->tabWidget->count() - 1; ++tab) { // Enable all except last (Console)
        ui->tabWidget->setTabEnabled(tab, enable);
    }
    ui->tabWidget->setTabVisible(Tab::Test, QFile::exists("/etc/apt/sources.list.d/mx.list")
                                                || QFile::exists("/etc/apt/sources.list.d/mx.sources"));
    ui->tabWidget->setTabVisible(Tab::Flatpak, arch != QLatin1String("i386"));
    ui->tabWidget->setTabVisible(Tab::Snap, isSystemdInit());
    setCursor(QCursor(Qt::ArrowCursor));
}

void MainWindow::hideColumns()
{
#ifdef PACKAGE_BACKEND_PACMAN
    ui->tabWidget->setCurrentIndex(Tab::EnabledRepos);
#else
    ui->tabWidget->setCurrentIndex(Tab::Popular);
#endif

    const bool showFlatpakBranch = debianVersion < Release::Trixie;
    ui->treeFlatpak->setColumnHidden(FlatCol::Branch, !showFlatpakBranch);
    ui->treeEnabled->hideColumn(TreeCol::Status); // Status of the package: installed, upgradable, etc
    ui->treeMXtest->hideColumn(TreeCol::Status);
    ui->treeBackports->hideColumn(TreeCol::Status);
    ui->treeFlatpak->hideColumn(FlatCol::Status);
    ui->treeFlatpak->hideColumn(FlatCol::Duplicate);
    ui->treeFlatpak->hideColumn(FlatCol::FullName);
    ui->treeSnap->hideColumn(SnapCol::Status);
    ui->treeSnap->hideColumn(SnapCol::Classic);
#ifdef PACKAGE_BACKEND_PACMAN
    ui->treeAUR->hideColumn(TreeCol::Status);
#endif
}

// Process downloaded *Packages.gz files
bool MainWindow::readPackageList(bool forceDownload)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    pushCancel->setDisabled(true);

    // Early return if lists are already populated and not forced to download
    if (!forceDownload
        && ((currentTree == ui->treeEnabled && !enabledList.isEmpty())
            || (currentTree == ui->treeMXtest && !mxList.isEmpty())
            || (currentTree == ui->treeBackports && !backportsList.isEmpty()))) {
        return true;
    }

    // treeEnabled is updated at downloadPackageList
    if (currentTree == ui->treeEnabled) {
        return true;
    }

    // Determine the file path based on the current tree
    QString filePath = tempDir.filePath((currentTree == ui->treeMXtest) ? "mxPackages" : "allPackages");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Could not open file:" << filePath;
        return false;
    }

    // Select the target package map based on the current tree
    auto &targetMap = (currentTree == ui->treeMXtest) ? mxList : backportsList;
    targetMap.clear();

    targetMap = PackageListParser::parse(file.readAll());

    file.close();
    return true;
}

void MainWindow::cancelDownload()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    holdProgressForAptRefresh = false;
    holdProgressForFlatpakRefresh = false;
    operationCanceled = true;
    if (activeDownloadReply) {
        downloadCancelRequested = true;
        activeDownloadReply->abort();
    }
    if (!cmd.terminateAndKill()) {
        qWarning() << "Could not terminate the active command";
    }
}

void MainWindow::centerWindow()
{
    const auto screenGeometry = QApplication::primaryScreen()->geometry();
    const auto x = (screenGeometry.width() - width()) / 2;
    const auto y = (screenGeometry.height() - height()) / 2;
    move(x, y);
}

void MainWindow::clearUi()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    blockSignals(true);

    // Configure buttons
    ui->pushCancel->setEnabled(true);
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);

    // Clear UI elements based on the current tree
    if (currentTree == ui->treeEnabled) {
        ui->labelNumApps->clear();
        ui->labelNumInst->clear();
        ui->labelNumUpgr->clear();
        if (enabledModel) {
            enabledModel->clear();
        }
        ui->pushUpgradeAll->setHidden(true);
    } else if (currentTree == ui->treeMXtest) {
        ui->labelNumApps_2->clear();
        ui->labelNumInstMX->clear();
        ui->labelNumUpgrMX->clear();
        if (mxtestModel) {
            mxtestModel->clear();
        }
    } else if (currentTree == ui->treeBackports) {
        ui->labelNumApps_3->clear();
        ui->labelNumInstBP->clear();
        ui->labelNumUpgrBP->clear();
        if (backportsModel) {
            backportsModel->clear();
        }
    }

    // Reset all filter combos
    const QList<QComboBox *> filterCombos = {ui->comboFilterBP, ui->comboFilterMX, ui->comboFilterEnabled};
    for (auto combo : filterCombos) {
        combo->setCurrentIndex(savedComboIndex);
    }
    ui->comboFilterFlatpak->setCurrentIndex(0);

    blockSignals(false);
}

void MainWindow::cleanup()
{
    // Callers like pushCancel_clicked() and closeEvent() clean up before quitting,
    // which then emits aboutToQuit — only the first invocation should do the work.
    if (cleanupDone) {
        return;
    }
    cleanupDone = true;
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (cmd.state() != QProcess::NotRunning) {
        qDebug() << "Command" << cmd.program() << cmd.arguments() << "terminated" << cmd.terminateAndKill();
    }
    if (QFile::exists(tempList)) {
        Cmd helperCmd;
        runMxpiMaintenanceAsRoot(helperCmd, QStringLiteral("cleanup_temp"));
        updateApt();
    }
    Cmd helperCmd;
    runMxpiMaintenanceAsRoot(helperCmd, QStringLiteral("copy_log"));
    settings.setValue("geometry", saveGeometry());
    settings.setValue("FlatpakRemote", ui->comboRemote->currentText());
    settings.setValue("FlatpakUser",
                      fpUser.startsWith(QLatin1String("--user")) ? QStringLiteral("user") : QStringLiteral("system"));
}

// Return true if all the packages listed are installed
bool MainWindow::checkInstalled(const QVariant &names) const
{

    QStringList nameList;
    if (names.canConvert<QStringList>()) {
        nameList = names.toStringList();
        // Flatten any strings in the list that contain newlines
        QStringList expandedList;
        for (const QString &name : nameList) {
            if (name.contains('\n')) {
                expandedList.append(name.split('\n', Qt::SkipEmptyParts));
            } else {
                expandedList.append(name);
            }
        }
        nameList = expandedList;
    } else if (names.canConvert<QString>()) {
        nameList = names.toString().split('\n', Qt::SkipEmptyParts);
    } else {
        return false;
    }

    if (nameList.isEmpty()) {
        return false;
    }

    // Trim whitespace from all package names
    for (QString &name : nameList) {
        name = name.trimmed();
    }
    for (const QString &name : nameList) {
        if (!installedPackages.contains(name)) {
            return false;
        }
    }
    return true;
}

// Return true if all the items in the list are upgradable
bool MainWindow::checkUpgradable(const QStringList &nameList) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (nameList.isEmpty()) {
        return false;
    }

    // Get the appropriate model for the current tree
    PackageModel *model = nullptr;
    if (currentTree == ui->treeEnabled) {
        model = enabledModel;
    } else if (currentTree == ui->treeMXtest) {
        model = mxtestModel;
    } else if (currentTree == ui->treeBackports) {
        model = backportsModel;
    }

    if (!model) {
        return false;
    }

    for (const QString &name : nameList) {
        int row = model->findPackageRow(name);
        if (row < 0) {
            return false;
        }
        const PackageData *pkg = model->packageAt(row);
        if (!pkg || pkg->status != Status::Upgradable) {
            return false;
        }
    }
    return true;
}

QHash<QString, PackageInfo> MainWindow::listInstalled()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bool ok = false;
    QHash<QString, PackageInfo> installedPackagesMap = PackageBackend::listInstalled(&ok);
    if (!ok) {
        QMessageBox::critical(this, tr("Error"),
                              tr("The installed-package query returned an error. Please check the log for details."));
    }
    return installedPackagesMap;
}

// scope is taken explicitly (not read from the fpUser member) so this can be called
// safely from a background thread without racing a concurrent write to fpUser from the
// GUI thread. The actual work lives in the free function listFlatpaksImpl() above --
// this member function only exists to supply the arch option via getArchOption(),
// which reads this->arch and so must run on the GUI thread; the constructor's Flatpak
// catalog preload calls listFlatpaksImpl() directly from its worker thread instead,
// with the arch option precomputed on the GUI thread and passed in by value.
QStringList MainWindow::listFlatpaks(const QString &remote, const QString &fpUserScope, const QString &type) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    return listFlatpaksImpl(remote, fpUserScope, getArchOption(), type);
}

PackageData MainWindow::createPackageData(const QString &name, const QString &version,
                                          const QString &description) const
{
    return {.name = name, .repoVersion = version, .description = description};
}

void MainWindow::setCurrentTree()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    const QList<QTreeView *> trees = {
        ui->treePopularApps, ui->treeEnabled, ui->treeMXtest, ui->treeBackports, ui->treeFlatpak, ui->treeSnap,
#ifdef PACKAGE_BACKEND_PACMAN
        ui->treeAUR,
#endif
    };

    auto it = std::find_if(trees.cbegin(), trees.cend(), [](const QTreeView *tree) { return tree->isVisible(); });

    if (it != trees.cend()) {
        currentTree = *it;
    }
}

void MainWindow::setDirty()
{
    dirtyBackports = true;
    dirtyEnabledRepos = true;
    dirtyTest = true;
}

void MainWindow::rebuildPackageViews()
{
    setDirty();
    // Rebuild Enabled Repos (temporarily switch currentTree so dirty flag is cleared)
    QTreeView *savedTree = currentTree;
    if (currentTree != ui->treeEnabled) {
        currentTree = ui->treeEnabled;
        buildPackageLists();
        currentTree = savedTree;
        // Also update original tree if it's an APT tab (data already loaded, just refresh display)
        if (currentTree == ui->treeMXtest) {
            displayPackages();
            dirtyTest = false;
        } else if (currentTree == ui->treeBackports) {
            displayPackages();
            dirtyBackports = false;
        }
    } else {
        buildPackageLists();
    }
    // Only refresh Popular if we're on that tab, otherwise it stays dirty for tab switch
    if (currentTree == ui->treePopularApps) {
        refreshPopularApps();
    }
}

void MainWindow::setIcons()
{

    const QString iconUpgradableName {QStringLiteral("package-installed-outdated")};
    const QString iconInstalledName {QStringLiteral("package-installed-updated")};

    const QIcon backupIconUpgradable(":/icons/package-installed-outdated.png");
    const QIcon backupIconInstalled(":/icons/package-installed-updated.png");

    const QIcon themeIconUpgradable = QIcon::fromTheme(iconUpgradableName, backupIconUpgradable);
    const QIcon themeIconInstalled = QIcon::fromTheme(iconInstalledName, backupIconInstalled);

    const bool forceBackupIcon = (themeIconUpgradable.name() == themeIconInstalled.name());

    qiconInstalled = forceBackupIcon ? backupIconInstalled : themeIconInstalled;
    qiconUpgradable = forceBackupIcon ? backupIconUpgradable : themeIconUpgradable;
    const auto upgradableIcons = {ui->iconUpgradable, ui->iconUpgradable_2, ui->iconUpgradable_3};
    const auto installedIcons = {ui->iconInstalledPackages, ui->iconInstalledPackages_2, ui->iconInstalledPackages_3,
                                 ui->iconInstalledPackages_4, ui->iconInstalledPackages_5};
    for (auto *icon : upgradableIcons) {
        icon->setIcon(qiconUpgradable);
    }
    for (auto *icon : installedIcons) {
        icon->setIcon(qiconInstalled);
    }
}

QHash<QString, VersionNumber> MainWindow::listInstalledVersions()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bool ok = false;
    QHash<QString, VersionNumber> installedVersions = PackageBackend::listInstalledVersions(&ok);
    if (!ok) {
        QMessageBox::critical(this, tr("Error"),
                              tr("The installed-package query returned an error. Please check the log for details."));
    }
    return installedVersions;
}

QUrl MainWindow::getScreenshotUrl(const QString &name)
{
    const QByteArray response = fetchUrl(QUrl(QString("https://screenshots.debian.net/json/package/%1").arg(name)));
    if (response.isEmpty()) {
        return {};
    }

    QJsonDocument jsonDoc = QJsonDocument::fromJson(response);
    if (jsonDoc.isObject()) {
        QJsonObject jsonObj = jsonDoc.object();
        if (jsonObj.contains(QStringLiteral("screenshots")) && jsonObj[QStringLiteral("screenshots")].isArray()) {
            QJsonArray screenshotsArray = jsonObj[QStringLiteral("screenshots")].toArray();
            if (!screenshotsArray.isEmpty()) {
                QJsonObject firstScreenshot = screenshotsArray.first().toObject();
                if (firstScreenshot.contains(QStringLiteral("small_image_url"))) {
                    return QUrl(firstScreenshot[QStringLiteral("small_image_url")].toString());
                }
            }
        }
    }
    return {};
}

void MainWindow::cmdStart()
{
    if (!timer.isActive()) {
        timer.start(100ms);
    }
    setCursor(QCursor(Qt::BusyCursor));
    ui->lineEdit->setFocus();
}

void MainWindow::cmdDone()
{
    timer.stop();
    setCursor(QCursor(Qt::ArrowCursor));
    disableOutput();
#ifdef PACKAGE_BACKEND_PACMAN
    // Reset any pending partial-match carry-over so leftover text from this
    // command's last chunk (e.g. an unmatched trailing "...sudo") can't combine
    // with an unrelated later command's output and produce a false sudo-prompt
    // match in outputAvailable().
    sudoPromptTail.clear();
#endif
    if (!holdProgressForFlatpakRefresh && !holdProgressForAptRefresh) {
        progress->hide();
    }
}

void MainWindow::enableOutput()
{
    connect(&cmd, &Cmd::outputAvailable, this, &MainWindow::outputAvailable, Qt::UniqueConnection);
    connect(&cmd, &Cmd::errorAvailable, this, &MainWindow::outputAvailable, Qt::UniqueConnection);
}

void MainWindow::disableOutput()
{
    disconnect(&cmd, &Cmd::outputAvailable, this, &MainWindow::outputAvailable);
    disconnect(&cmd, &Cmd::errorAvailable, this, &MainWindow::outputAvailable);
}

void MainWindow::displayInfoTestOrBackport(QTreeView *tree, const QModelIndex &index)
{
    QString fileName = (tree == ui->treeMXtest) ? tempDir.filePath("mxPackages") : tempDir.filePath("allPackages");

    QFile file(fileName);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        qWarning() << "Could not open file:" << file.fileName();
        return;
    }

    // Get package name from model
    QString itemName;
    if (auto *proxy = qobject_cast<PackageFilterProxy *>(tree->model())) {
        QModelIndex sourceIndex = proxy->mapToSource(index);
        itemName = sourceIndex.sibling(sourceIndex.row(), TreeCol::Name).data().toString();
    } else {
        itemName = index.sibling(index.row(), TreeCol::Name).data().toString();
    }

    QString msg;
    QTextStream in(&file);
    bool packageFound = false;

    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.startsWith(QLatin1String("Package: "))) {
            if (line == QStringLiteral("Package: ") + itemName) {
                packageFound = true;
                msg += line + '\n';
            } else if (packageFound) {
                break;
            }
        } else if (packageFound) {
            msg += line + '\n';
        }
    }
    auto msgList = msg.split('\n', Qt::SkipEmptyParts);
    if (msgList.isEmpty()) {
        qWarning() << "Package info not found in file:" << file.fileName() << "Show info from enabled repos";
        displayPackageInfo(tree->currentIndex());
        return;
    }
    auto maxNoChars = 2000;        // Around 15-17 lines
    if (msg.size() > maxNoChars) { // Split msg into details if too large
        uchar maxNoLines = 20;     // Cut message after these many lines
        msg = msgList.mid(0, maxNoLines).join('\n');
    }
    QMessageBox info(QMessageBox::NoIcon, tr("Package info"), msg, QMessageBox::Close);

    // Make it wider
    auto *horizontalSpacer = new QSpacerItem(width(), 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    auto *layout = qobject_cast<QGridLayout *>(info.layout());
    if (layout) {
        layout->addItem(horizontalSpacer, 0, 1);
    }
    info.exec();
}

void MainWindow::displayPackageInfo(QTreeView *tree, QPoint pos)
{
    // Use the tree that was passed in, not focusWidget()
    if (!tree) {
        qWarning() << "No tree view";
        return;
    }

    // Get index: prefer indexAt(pos) for mouse clicks, fall back to currentIndex() for keyboard
    QModelIndex currentIdx = tree->indexAt(pos);
    if (!currentIdx.isValid()) {
        currentIdx = tree->currentIndex();
    }

    if (!currentIdx.isValid()) {
        qWarning() << "No valid index";
        return;
    }

    // treePopularApps is handled by treePopularApps_customContextMenuRequested; this
    // path serves the APT trees (Enabled repos, MX Test, Backports), Flatpak and Snap.
    QMenu menu(this);
    // Parented to &menu (not this) so it's destroyed along with the menu below --
    // QMenu::addAction(QAction*) doesn't take ownership, so parenting to the
    // long-lived MainWindow would leak one QAction per invocation.
    auto *action = new QAction(QIcon::fromTheme("dialog-information"), tr("More &info..."), &menu);
    action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I)); // hint only; the QShortcut handles the key
    menu.addAction(action);
    connect(action, &QAction::triggered, this, [this, tree, currentIdx] { displayInfo(tree, currentIdx); });
    menu.exec(tree->mapToGlobal(pos));
}

// Shows the info dialog that fits the tree the row belongs to.
void MainWindow::displayInfo(QTreeView *tree, const QModelIndex &index)
{
    if (!tree || !index.isValid()) {
        return;
    }
    if (tree == ui->treePopularApps) {
        displayPopularInfo(index);
    } else if (tree == ui->treeFlatpak) {
        displayFlatpakInfo(index);
    } else if (tree == ui->treeSnap) {
        displaySnapInfo(index);
    } else if (tree == ui->treeMXtest || tree == ui->treeBackports) {
        displayInfoTestOrBackport(tree, index);
    } else {
        displayPackageInfo(index); // Enabled repos, and AUR on the pacman backend
    }
}

QByteArray MainWindow::fetchUrl(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QNetworkProxyQuery query(url);
    const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
    if (!proxies.isEmpty()) {
        manager.setProxy(proxies.first());
    }
    QNetworkRequest request(url);
    for (const auto &[name, value] : headers) {
        request.setRawHeader(name, value);
    }
    // Bounds the request (Qt aborts it and emits finished() with
    // QNetworkReply::TimeoutError if it fires), so the error() check below
    // correctly distinguishes a timeout from a genuine successful reply --
    // unlike a bare QTimer::singleShot() that only quits the event loop
    // without touching the still in-flight reply. Use the int-milliseconds
    // overload: the std::chrono one needs Qt 6.7+, and this project doesn't
    // pin a Qt minimum above 6.4.
    request.setTransferTimeout(5000);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray data;
    if (reply->error() == QNetworkReply::NoError) {
        data = reply->readAll();
    } else {
        qDebug() << "Download of" << url.url() << "failed:" << qPrintable(reply->errorString());
    }
    reply->deleteLater();
    return data;
}

// Downloads an image and returns it as an inline <img> tag (empty on failure).
QString MainWindow::imageHtml(const QUrl &url, QSize size, Qt::AspectRatioMode mode)
{
    if (!url.isValid() || url.isEmpty()) {
        return {};
    }
    QByteArray raw = fetchUrl(url);
    if (raw.isEmpty()) {
        return {};
    }
    QBuffer inBuffer(&raw);
    QImageReader imageReader(&inBuffer);
    QImage image = imageReader.read();
    if (image.isNull()) {
        qDebug() << "loading screenshot:" << imageReader.errorString();
        return {};
    }
    image = image.scaled(size, mode, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer outBuffer(&png);
    image.save(&outBuffer, "PNG");
    return QStringLiteral("<p><img src='data:image/png;base64, %1'>").arg(QString(png.toBase64()));
}

// Formats store/appstream metadata the same way for Flatpak and Snap.
QString MainWindow::appInfoHtml(const AppInfo &app)
{
    QString msg = QStringLiteral("<b>") + app.title.toHtmlEscaped() + QStringLiteral("</b>");
    if (!app.summary.isEmpty()) {
        msg += QStringLiteral("<br><i>") + app.summary.toHtmlEscaped() + QStringLiteral("</i>");
    }
    msg += app.descriptionHtml;
    QStringList meta;
    if (!app.developer.isEmpty()) {
        meta << tr("Developer: %1").arg(app.developer.toHtmlEscaped());
    }
    if (!app.license.isEmpty()) {
        meta << tr("License: %1").arg(app.license.toHtmlEscaped());
    }
    if (!app.homepage.isEmpty()) {
        const QString link = app.homepage.toHtmlEscaped();
        meta << tr("Homepage: %1").arg(QStringLiteral("<a href='%1'>%1</a>").arg(link));
    }
    if (!meta.isEmpty()) {
        msg += QStringLiteral("<p>") + meta.join(QStringLiteral("<br>"));
    }
    return msg + app.screenshotHtml;
}

void MainWindow::showInfoBox(const QString &text, bool richText)
{
    QMessageBox info(QMessageBox::NoIcon, tr("Package info"), text, QMessageBox::Close, this);
    if (richText) {
        info.setTextFormat(Qt::RichText);
        info.setTextInteractionFlags(Qt::TextBrowserInteraction);
    } else {
        // Make it wider
        auto *horizontalSpacer = new QSpacerItem(width(), 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
        if (auto *layout = qobject_cast<QGridLayout *>(info.layout())) {
            layout->addItem(horizontalSpacer, 0, 1);
        }
    }
    info.exec();
}

void MainWindow::displayFlatpakInfo(const QModelIndex &index)
{
    if (!index.isValid() || !flatpakProxy || !flatpakModel) {
        return;
    }
    const FlatpakData *data = flatpakModel->flatpakAt(flatpakProxy->mapToSource(index).row());
    if (!data || data->canonicalRef.isEmpty()) {
        return;
    }
    const QString appId = data->longName;
    const bool installed = data->status == Status::Installed;
    const QString remote = ui->comboRemote->currentText();

    QString text;
    bool richText = false;
    {
        // The network/Cmd calls below spin nested event loops; disable the window
        // meanwhile so they can't be reentered (see displayPackageInfo()).
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::BusyCursor);
        auto restore = qScopeGuard([this] {
            QApplication::restoreOverrideCursor();
            setEnabled(true);
        });

        // Flathub's appstream API has the user-facing description and screenshots.
        // Runtimes and apps from other remotes aren't there, so those fall back to
        // flatpak's own (technical) info.
        const QJsonObject json
            = QJsonDocument::fromJson(fetchUrl(QUrl(QStringLiteral("https://flathub.org/api/v2/appstream/") + appId)))
                  .object();
        if (!json.value(QStringLiteral("name")).toString().isEmpty()) {
            AppInfo app;
            app.title = json.value(QStringLiteral("name")).toString();
            app.summary = json.value(QStringLiteral("summary")).toString();
            app.descriptionHtml = json.value(QStringLiteral("description")).toString().trimmed();
            app.developer = json.value(QStringLiteral("developer_name")).toString();
            app.license = json.value(QStringLiteral("project_license")).toString();
            app.homepage = json.value(QStringLiteral("urls")).toObject().value(QStringLiteral("homepage")).toString();

            // Pick the default screenshot (else the first), at the largest size that
            // still fits the dialog comfortably (else whatever is listed first).
            const QJsonArray screenshots = json.value(QStringLiteral("screenshots")).toArray();
            QJsonObject shot = screenshots.isEmpty() ? QJsonObject() : screenshots.first().toObject();
            for (const auto &value : screenshots) {
                if (value.toObject().value(QStringLiteral("default")).toBool()) {
                    shot = value.toObject();
                    break;
                }
            }
            QString shotUrl;
            int bestWidth = -1;
            for (const auto &value : shot.value(QStringLiteral("sizes")).toArray()) {
                const QJsonObject size = value.toObject();
                const int w = size.value(QStringLiteral("width")).toVariant().toInt(); // sent as a string
                if (shotUrl.isEmpty() || (w <= 800 && w > bestWidth)) {
                    shotUrl = size.value(QStringLiteral("src")).toString();
                    bestWidth = w <= 800 ? w : -1;
                }
            }
            app.screenshotHtml = imageHtml(QUrl(shotUrl));
            text = appInfoHtml(app);
            richText = true;
        } else {
            QStringList args = installed ? QStringList {"info"} : QStringList {"remote-info", remote};
            args << data->canonicalRef;
            Cmd shell;
            text = shell.getOut("flatpak", flatpakArgsWithScope(fpUser, args), Cmd::QuietMode::Yes).trimmed();
        }
    }
    showInfoBox(text.isEmpty() ? tr("No information available.") : text, richText);
}

void MainWindow::displayPopularInfo(const QModelIndex &index)
{
    // Skip categories (items with no parent in hierarchical model)
    if (!index.isValid() || !index.parent().isValid()) {
        return;
    }

    // Get data from model via index
    QString desc = index.sibling(index.row(), PopCol::Description).data().toString();
    QString installNames = index.sibling(index.row(), PopCol::Name).data(Qt::UserRole).toString();
    QString title = index.sibling(index.row(), PopCol::Name).data().toString();
    QString msg = QStringLiteral("<b>") + title + QStringLiteral("</b><p>") + desc + QStringLiteral("<p>");
    if (!installNames.isEmpty()) {
        msg += tr("Packages to be installed: ") + installNames;
    }

    // Block the tree's signals while the downloads spin their event loops, so a
    // second click on the Info column can't reenter this.
    QString screenshot;
    {
        const QSignalBlocker blocker(ui->treePopularApps);
        const auto isUsable = [](const QUrl &url) {
            return url.isValid() && !url.isEmpty() && url.url() != QLatin1String("none");
        };
        const QString firstPackage = installNames.split(' ').first();
        // Prefer the screenshot URL from the popular-apps list, falling back to
        // screenshots.debian.net if it is missing or fails to download.
        const QUrl listUrl(index.sibling(index.row(), PopCol::Description).data(Qt::UserRole).toString());
        if (isUsable(listUrl)) {
            screenshot = imageHtml(listUrl, QSize(200, 300), Qt::KeepAspectRatioByExpanding);
        }
        if (screenshot.isEmpty()) {
            const QUrl debianUrl = getScreenshotUrl(firstPackage);
            if (isUsable(debianUrl) && debianUrl != listUrl) {
                screenshot = imageHtml(debianUrl, QSize(200, 300), Qt::KeepAspectRatioByExpanding);
            }
        }
        if (screenshot.isEmpty()) {
            qDebug() << "no screenshot for: " << title;
        }
    }
    showInfoBox(msg + screenshot, true);
}

void MainWindow::displayPackageInfo(const QModelIndex &index)
{
    // Get package name from model
    QString packageName = index.sibling(index.row(), TreeCol::Name).data().toString();
    if (packageName.isEmpty()) {
        return;
    }

#ifdef PACKAGE_BACKEND_PACMAN
    if (currentTree == ui->treeAUR) {
        showAurPackageInfo(packageName);
        return;
    }
    // No aptitude/dependency-simulation equivalent on this backend; pacman's own
    // info covers the same purpose for the Enabled Repos tab.
    Cmd shell;
    QString info = shell.getOut({{"LANG", "C"}}, "pacman", {"-Si", "--color", "never", packageName});
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0 || info.trimmed().isEmpty()) {
        info = shell.getOut({{"LANG", "C"}}, "pacman", {"-Qi", "--color", "never", packageName});
    }
    QMessageBox::information(this, tr("Package info"),
                             info.trimmed().isEmpty() ? tr("No information available.") : info.trimmed());
#else
    QString msg;
    QString rawDetails;
    {
        // These aptitude queries spin nested event loops inside Cmd::startAndWait,
        // which keep the window interactive. Disable it (with a busy cursor) for the
        // duration so a second action can't reenter the shared cmd — which would hit
        // its "already running" guard and fail with a misleading error.
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::BusyCursor);
        auto restore = qScopeGuard([this] {
            QApplication::restoreOverrideCursor();
            setEnabled(true);
        });
        msg = cmd.getOut(QStringLiteral("aptitude"), {"show", packageName});
        // Keep last 5 lines from aptitude output. DEBIAN_FRONTEND here is picked by a
        // small conditional pipeline (dpkg -l | grep -sq, twice, with || fallback) that
        // runs *inside* the target environment being queried -- faithfully replicating
        // that command-substitution-plus-branching in C++ would mean reimplementing
        // non-trivial shell semantics for no real benefit, so this one is deliberately
        // left as a shell string rather than converted to argv-vector calls.
        rawDetails = cmd.getOut("DEBIAN_FRONTEND=$(dpkg -l debconf-kde-helper 2>/dev/null | grep -sq ^i && echo kde "
                                "|| dpkg -l debconf-gnome 2>/dev/null | grep -sq ^i && echo gnome "
                                "|| echo noninteractive) aptitude -sy -V -o=Dpkg::Use-Pty=0 install "
                                + shellSingleQuote(packageName));
    }
    const QStringList rawLines = rawDetails.split('\n');
    QString details = rawLines.mid(qMax(0, rawLines.size() - 5)).join('\n');

    auto detailList = details.split('\n');
    auto msgList = msg.split('\n');
    auto maxNoChars = 2000;        // Around 15-17 lines
    if (msg.size() > maxNoChars) { // Split msg into details if too large
        uchar maxNoLines = 17;     // Cut message after these many lines
        msg = msgList.mid(0, maxNoLines).join('\n');
        detailList = msgList.mid(maxNoLines, msgList.length()) + QStringList {} + detailList;
        details = detailList.join('\n');
    }
    if (detailList.size() >= 2) {
        msg += "\n\n" + detailList.at(detailList.size() - 2); // Add info about space needed/freed
    }

    QMessageBox info(QMessageBox::NoIcon, tr("Package info"), msg.trimmed(), QMessageBox::Close);
    info.setDetailedText(details.trimmed());

    // Make it wider
    auto *horizontalSpacer = new QSpacerItem(width(), 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    auto *layout = qobject_cast<QGridLayout *>(info.layout());
    if (layout) {
        layout->addItem(horizontalSpacer, 0, 1);
    }
    info.exec();
#endif
}

void MainWindow::findPopular()
{
    const QString word = ui->searchPopular->text();
    if (word.length() == 1) {
        return;
    }

    if (popularProxy) {
        popularProxy->setSearchText(word);
    }

    // Handle expansion based on search
    if (word.isEmpty()) {
        ui->treePopularApps->collapseAll();
    } else {
        ui->treePopularApps->expandAll();
    }

    // Reapply category spanning AFTER collapse/expand
    applyPopularCategorySpanning();

    // Resize columns except the first one
    if (popularModel) {
        for (int i = 1; i < popularModel->columnCount(); ++i) {
            ui->treePopularApps->resizeColumnToContents(i);
        }
    }
}

void MainWindow::applyPopularCategorySpanning()
{
    if (!popularModel || !ui->treePopularApps) {
        return;
    }

    // Make categories span the first two columns (Icon + Check)
    // When using a proxy, we need to iterate through PROXY rows, not source rows
    // Only span top-level items (categories), not child items (apps)
    for (int i = 0; i < popularProxy->rowCount(); ++i) {
        QModelIndex proxyIndex = popularProxy->index(i, 0);
        // Only span if this is a top-level item (category, no parent)
        if (!proxyIndex.parent().isValid()) {
            ui->treePopularApps->setFirstColumnSpanned(i, QModelIndex(), true);
        }
    }
}

void MainWindow::findPackage()
{
    // Get search text from appropriate search box
    QString word;
    if (currentTree == ui->treeEnabled) {
        word = ui->searchBoxEnabled->text();
    } else if (currentTree == ui->treeMXtest) {
        word = ui->searchBoxMX->text();
    } else if (currentTree == ui->treeBackports) {
        word = ui->searchBoxBP->text();
    } else if (currentTree == ui->treeFlatpak) {
        word = ui->searchBoxFlatpak->text();
    } else if (currentTree == ui->treeSnap) {
        word = ui->searchBoxSnap->text();
    }

    // Skip single character searches
    if (word.length() == 1) {
        return;
    }

    // Set search text on appropriate proxy
    if (currentTree == ui->treeSnap) {
        // In store mode the search box drives `snap find` (on Enter); don't also filter locally.
        if (snapProxy && !snapStoreMode) {
            snapProxy->setSearchText(word);
        }
        if (snapModel) {
            for (int i = 0; i < snapModel->columnCount(); ++i) {
                if (!ui->treeSnap->isColumnHidden(i)) {
                    ui->treeSnap->resizeColumnToContents(i);
                }
            }
        }
    } else if (currentTree == ui->treeFlatpak) {
        if (flatpakProxy) {
            flatpakProxy->setSearchText(word);
        }
        // Resize columns after search for Flatpak
        if (flatpakModel) {
            for (int i = 0; i < flatpakModel->columnCount(); ++i) {
                if (!ui->treeFlatpak->isColumnHidden(i)) {
                    ui->treeFlatpak->resizeColumnToContents(i);
                }
            }
        }
    } else {
        auto *proxy = getCurrentProxy();
        if (proxy) {
            proxy->setSearchText(word);
            // Re-sort after search to maintain order (preserve user's chosen column/order)
            if (currentTree) {
                int sortColumn = currentTree->header()->sortIndicatorSection();
                Qt::SortOrder sortOrder = currentTree->header()->sortIndicatorOrder();

                // If no sort indicator or sorted by checkbox column, default to Package Name
                if (sortColumn < 0 || sortColumn == TreeCol::Check) {
                    sortColumn = TreeCol::Name;
                    sortOrder = Qt::AscendingOrder;
                }

                proxy->sort(sortColumn, sortOrder);
            }
        }
    }

    // Resize columns after search filter is applied (for package tabs)
    if (currentTree != ui->treePopularApps && currentTree != ui->treeFlatpak && currentTree != ui->treeSnap) {
        auto *model = getCurrentModel();
        if (model && currentTree) {
            for (int i = 0; i < model->columnCount(); ++i) {
                if (!currentTree->isColumnHidden(i)) {
                    currentTree->resizeColumnToContents(i);
                }
            }
        }
    }
}

void MainWindow::showOutput()
{
    operationInProgress = true;
    ui->outputBox->clear();
    outputRenderer.reset();
    ui->tabWidget->setTabEnabled(Tab::Output, true);
    ui->tabWidget->setCurrentWidget(ui->tabOutput);
    enableTabs(false);
}

void MainWindow::pushInstall_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    beginOperation();
    showOutput();
    if (currentTree == ui->treeFlatpak) {
        // showOutput() cleared the visible checkboxes; work from changeList. Only
        // install flatpaks that are not already installed (the selection may be mixed).
        QStringList toInstall;
        for (const QString &name : std::as_const(changeList)) {
            const int row = flatpakModel ? flatpakModel->findRowByFullName(name) : -1;
            const FlatpakData *fp = (row >= 0) ? flatpakModel->flatpakAt(row) : nullptr;
            if (!fp || fp->status != Status::Installed) {
                toInstall.append(name);
            }
        }
        changeList = toInstall;
        if (changeList.isEmpty()) {
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }
        // Confirmation dialog — on cancel, restore the tab without claiming success.
        if (!confirmActions(changeList.join(' '), "install")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        QStringList args {"install", "-y"};
        args << fpUser.trimmed();
        args << ui->comboRemote->currentText();
        args += changeList;
        if (cmd.procOnPty(QStringLiteral("flatpak"), args)) {
            appendFlatpakStatusMessage(ui->outputBox, tr("Install complete."));
            displayFlatpaks(true);
            indexFilterFP.clear();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
        } else {
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::critical(this, tr("Error"),
                                  tr("Problem detected while installing, please inspect the console output."));
        }
    } else if (currentTree == ui->treeSnap) {
        // Read changeList, not the model check state: showOutput() above switched to the
        // Output tab, which clears the visible checkboxes via resetCheckboxes(). Only
        // install snaps that are not already installed (the selection may be mixed).
        QStringList toInstall;
        for (const QString &name : std::as_const(changeList)) {
            const int row = snapModel ? snapModel->findSnapRow(name) : -1;
            const SnapData *snap = (row >= 0) ? snapModel->snapAt(row) : nullptr;
            if (!snap || snap->status != Status::Installed) {
                toInstall.append(name);
            }
        }
        if (toInstall.isEmpty()) {
            enableTabs(true);
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            return;
        }
        if (QMessageBox::question(this, tr("Install snaps"),
                                  tr("OK to install the following snap packages?\n\n%1").arg(toInstall.join('\n')))
            != QMessageBox::Yes) {
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            enableTabs(true);
            return;
        }
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        bool success = true;
        QString errorDetails;
        if (toInstall.size() > 1) {
            appendFlatpakStatusMessage(ui->outputBox, tr("Installing packages: %1...").arg(toInstall.join(' ')));
        }
        for (const QString &name : std::as_const(toInstall)) {
            appendFlatpakStatusMessage(ui->outputBox, tr("Installing package: %1...").arg(name));
            QStringList snapArgs {"install"};
            if (snapModel && snapModel->isClassic(name)) {
                snapArgs << "--classic";
            }
            snapArgs << name;
            // procAsRoot reuses the cached MXPI elevation (auth_admin_keep), so a
            // multi-snap install only prompts for the password once.
            if (!cmd.procAsRoot(QStringLiteral("snap"), snapArgs)) {
                success = false;
                errorDetails = cmd.readAllOutput();
                break;
            }
        }
        setCursor(QCursor(Qt::ArrowCursor));
        if (success) {
            appendFlatpakStatusMessage(ui->outputBox, tr("Install complete."));
            displaySnaps(true);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
        } else if (!Cmd::elevationDismissed()) {
            showError(tr("Problem detected while installing a snap. Click \"Show Details\" for more information."),
                      errorDetails);
        } else {
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
        }
        changeList.clear();
        enableTabs(true);
        return;
    } else {
        bool success = false;
        if (currentTree == ui->treePopularApps) {
            success = installPopularApps();
        } else if (currentTabFiltersAutoremovable()) {
            success = markKeep();
        } else {
            success = installSelected();
        }
        rebuildPackageViews();
        if (success && !operationWasCanceled()) {
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
        } else if (!operationWasCanceled()) {
            QMessageBox::critical(this, tr("Error"),
                                  tr("Problem detected while installing, please inspect the console output."));
        } else {
            ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
        }
    }
    enableTabs(true);
}

void MainWindow::pushAbout_clicked()
{
    hide();
    displayAboutMsgBox(
        tr("About %1").arg(windowTitle()),
        "<p align=\"center\"><b><h2>" + windowTitle() + "</h2></b></p><p align=\"center\">" + tr("Version: ")
            + QCoreApplication::applicationVersion() + "</p><p align=\"center\"><h3>"
            + tr("Package Installer for MX Linux")
            + R"(</h3></p><p align="center"><a href="http://mxlinux.org">http://mxlinux.org</a><br /></p><p align="center">)"
            + tr("Copyright (c) MX Linux") + "<br /><br /></p>",
        "/usr/share/doc/mx-packageinstaller/license.html", tr("%1 License").arg(windowTitle()));
    show();
}

void MainWindow::pushHelp_clicked()
{
#ifdef PACKAGE_BACKEND_PACMAN
    displayHelpDoc("/usr/share/doc/mx-packageinstaller/mx-package-installer-pacman.html",
                   tr("%1 Help").arg(windowTitle()));
#else
    displayHelpDoc("/usr/share/doc/mx-packageinstaller/mx-package-installer.html", tr("%1 Help").arg(windowTitle()));
#endif
}

// Resize columns when expanding
void MainWindow::treePopularApps_expanded()
{
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
    ui->treePopularApps->resizeColumnToContents(PopCol::Description);
}

void MainWindow::treePopularApps_itemExpanded(const QModelIndex &index)
{
    // Guard against null proxy
    if (!popularProxy || !popularModel) {
        return;
    }

    // Only update icon for category items (not child apps)
    if (!index.parent().isValid()) {
        // Map proxy index to source model before setting data
        QModelIndex sourceIndex = popularProxy->mapToSource(index);
        QModelIndex iconIndex = sourceIndex.siblingAtColumn(PopCol::Category);
        if (iconIndex.isValid()) {
            popularModel->setData(iconIndex, QIcon::fromTheme("folder-open"), Qt::DecorationRole);
        }

    }
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
    ui->treePopularApps->resizeColumnToContents(PopCol::Description);
}

void MainWindow::treePopularApps_itemCollapsed(const QModelIndex &index)
{
    // Guard against null proxy
    if (!popularProxy || !popularModel) {
        return;
    }

    // Only update icon for category items (not child apps)
    if (!index.parent().isValid()) {
        // Map proxy index to source model before setting data
        QModelIndex sourceIndex = popularProxy->mapToSource(index);
        QModelIndex iconIndex = sourceIndex.siblingAtColumn(PopCol::Category);
        if (iconIndex.isValid()) {
            popularModel->setData(iconIndex, QIcon::fromTheme("folder"), Qt::DecorationRole);
        }
    }
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
    ui->treePopularApps->resizeColumnToContents(PopCol::Description);
}

void MainWindow::pushUninstall_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    beginOperation();

    showOutput();

    QString names;
    QStringList preuninstall;
    QStringList postuninstall;
    if (currentTree == ui->treePopularApps) {
        QModelIndexList checkedItems = popularModel->checkedItems();
        for (const QModelIndex &index : checkedItems) {
            const PopularAppData *app = popularModel->getAppData(index);
            if (!app) {
                continue;
            }

            names += app->uninstallNames.trimmed().replace('\n', ' ') + ' ';
            if (!app->postUninstall.isEmpty()) {
                postuninstall << app->postUninstall;
            }
            if (!app->preUninstall.isEmpty()) {
                preuninstall << app->preUninstall;
            }
            popularModel->setData(index, Qt::Unchecked, Qt::CheckStateRole);
        }
    } else if (currentTree == ui->treeFlatpak) {
        bool success = true;

        // showOutput() cleared the visible checkboxes; work from changeList. Only
        // uninstall flatpaks that are actually installed (the selection may be mixed).
        QStringList toUninstall;
        for (const QString &name : std::as_const(changeList)) {
            const int row = flatpakModel ? flatpakModel->findRowByFullName(name) : -1;
            const FlatpakData *fp = (row >= 0) ? flatpakModel->flatpakAt(row) : nullptr;
            if (fp && fp->status == Status::Installed) {
                toUninstall.append(name);
            }
        }
        changeList = toUninstall;
        if (changeList.isEmpty()) {
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }

        // Confirmation dialog — on cancel, restore the tab without claiming success.
        if (!confirmActions(changeList.join(' '), "remove")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            listFlatpakRemotes();
            ui->comboRemote->setCurrentIndex(0);
            comboRemote_activated();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }

        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        showFlatpakProgress(tr("Uninstalling flatpaks..."));
        QStringList uninstallArgs {"uninstall"};
        uninstallArgs << fpUser.trimmed();
        uninstallArgs << "-y";
        uninstallArgs += changeList;
        if (!cmd.procOnPty(QStringLiteral("flatpak"), uninstallArgs)) {
            success = false;
        }
        if (success) { // Success if all processed successfuly, failure if one failed
            appendFlatpakStatusMessage(ui->outputBox, tr("Uninstall complete."));
            showFlatpakProgress(tr("Refreshing flatpaks..."));
            displayFlatpaks(true);
            indexFilterFP.clear();
            listFlatpakRemotes();
            ui->comboRemote->setCurrentIndex(0);
            comboRemote_activated();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            progress->hide();
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
        } else {
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling, please check output"));
        }
        enableTabs(true);
        return;
    } else if (currentTree == ui->treeSnap) {
        // Read changeList (showOutput() cleared the visible checkboxes). Only remove
        // snaps that are actually installed (the selection may be mixed).
        QStringList toRemove;
        for (const QString &name : std::as_const(changeList)) {
            const int row = snapModel ? snapModel->findSnapRow(name) : -1;
            const SnapData *snap = (row >= 0) ? snapModel->snapAt(row) : nullptr;
            if (snap && snap->status == Status::Installed) {
                toRemove.append(name);
            }
        }
        if (toRemove.isEmpty()) {
            enableTabs(true);
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            return;
        }
        if (QMessageBox::question(this, tr("Remove snaps"),
                                  tr("OK to remove the following snap packages?\n\n%1").arg(toRemove.join('\n')))
            != QMessageBox::Yes) {
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            enableTabs(true);
            return;
        }
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        bool success = true;
        QString errorDetails;
        for (const QString &name : std::as_const(toRemove)) {
            if (!cmd.procAsRoot(QStringLiteral("snap"), {QStringLiteral("remove"), name})) {
                success = false;
                errorDetails = cmd.readAllOutput();
                break;
            }
        }
        setCursor(QCursor(Qt::ArrowCursor));
        if (success) {
            appendFlatpakStatusMessage(ui->outputBox, tr("Uninstall complete."));
            displaySnaps(true);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
        } else if (!Cmd::elevationDismissed()) {
            showError(tr("We encountered a problem removing a snap. Click \"Show Details\" for more information."),
                      errorDetails);
        } else {
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
        }
        changeList.clear();
        enableTabs(true);
        return;
    } else {
        names = changeList.join(' ');
    }

    bool success = uninstall(names, preuninstall, postuninstall);
    rebuildPackageViews();
    if (success && !operationWasCanceled()) {
        QMessageBox::information(this, tr("Success"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    } else if (!operationWasCanceled()) {
        QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling the program"));
    } else {
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    }
    enableTabs(true);
}

void MainWindow::tabWidget_currentChanged(int index)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabText(Tab::Output, tr("Console Output"));
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);

    resetCheckboxes();
    QString searchStr;
    saveSearchText(searchStr, savedComboIndex);
    if (index != Tab::Output) {
        setCurrentTree();
    }

    // Defer heavy work to next event loop iteration so tab switches immediately
    QMetaObject::invokeMethod(this, [this, index, searchStr]() {
        // Guard against stale lambda execution if user switched tabs again
        if (ui->tabWidget->currentIndex() != index) {
            return;
        }
        if (index != Tab::Output && !operationInProgress) {
            beginOperation();
        }

        // Only block signals for non-Output tabs since Output doesn't need tree interaction
        if (index != Tab::Output) {
            currentTree->blockSignals(true);
        }

        auto setTabsEnabled = [this](bool enable) {
            for (auto tab : {Tab::Popular, Tab::EnabledRepos,
#ifdef PACKAGE_BACKEND_PACMAN
                             Tab::AUR,
#endif
                             Tab::Test, Tab::Backports, Tab::Flatpak, Tab::Snap}) {
                if (tab != ui->tabWidget->currentIndex()) {
                    ui->tabWidget->setTabEnabled(tab, enable);
                }
            }
        };
        setTabsEnabled(false);
        switch (index) {
        case Tab::Popular:
            handleTab(searchStr, ui->searchPopular, "", false);
            findPopular(); // ensure proxy filter matches search box (setText may not fire textChanged if text unchanged)
            break;
        case Tab::EnabledRepos:
            handleEnabledReposTab(searchStr);
            break;
#ifdef PACKAGE_BACKEND_PACMAN
        case Tab::AUR:
            handleAurTab(searchStr);
            break;
#endif
        case Tab::Test:
            handleTab(searchStr, ui->searchBoxMX, "test", dirtyTest);
            break;
        case Tab::Backports:
            handleTab(searchStr, ui->searchBoxBP, "backports", dirtyBackports);
            break;
        case Tab::Flatpak:
            handleFlatpakTab(searchStr);
            break;
        case Tab::Snap:
            handleSnapTab(searchStr);
            break;
        case Tab::Output:
            handleOutputTab();
            break;
        }
        if (index != Tab::Output || !operationInProgress) {
            setTabsEnabled(true);
        }
        ui->pushUpgradeAll->setVisible((currentTree == ui->treeEnabled) && (ui->labelNumUpgr->text().toInt() > 0));
    }, Qt::QueuedConnection);
}

void MainWindow::resetCheckboxes()
{
    currentTree->blockSignals(true);
    // Popular apps are processed in a different way, tree is reset after install/removal
    if (currentTree == ui->treePopularApps) {
        if (ui->tabWidget->currentIndex() != Tab::Output) { // Don't clear selections on output tab for pop apps
            if (popularModel && !popularModel->checkedItems().isEmpty()) {
                popularModel->uncheckAll();
            }
        }
    } else if (currentTree == ui->treeFlatpak) {
        currentTree->clearSelection();
        if (flatpakModel) {
            if (!flatpakModel->checkedPackages().isEmpty()) {
                flatpakModel->setAllChecked(false);
            }
        }
    } else if (currentTree == ui->treeSnap) {
        currentTree->clearSelection();
        if (snapModel) {
            if (!snapModel->checkedPackages().isEmpty()) {
                snapModel->setAllChecked(false);
            }
        }
    } else {
        // Package-based tabs (Enabled, Test, Backports)
        currentTree->clearSelection();
        auto *model = getCurrentModel();
        if (model) {
            if (!model->checkedPackages().isEmpty()) {
                model->uncheckAll();
            }
        }
    }
}

void MainWindow::saveSearchText(QString &searchStr, int &filterIdx)
{
    if (currentTree == ui->treePopularApps) {
        searchStr = ui->searchPopular->text();
    } else if (currentTree == ui->treeEnabled) {
        searchStr = ui->searchBoxEnabled->text();
        filterIdx = ui->comboFilterEnabled->currentIndex();
    } else if (currentTree == ui->treeMXtest) {
        searchStr = ui->searchBoxMX->text();
        filterIdx = ui->comboFilterMX->currentIndex();
    } else if (currentTree == ui->treeBackports) {
        searchStr = ui->searchBoxBP->text();
        filterIdx = ui->comboFilterBP->currentIndex();
#ifdef PACKAGE_BACKEND_PACMAN
    } else if (currentTree == ui->treeAUR) {
        searchStr = ui->searchBoxAUR->text();
        filterIdx = ui->comboFilterAUR->currentIndex();
#endif
    } else if (currentTree == ui->treeFlatpak) {
        searchStr = ui->searchBoxFlatpak->text();
    } else if (currentTree == ui->treeSnap) {
        searchStr = ui->searchBoxSnap->text();
    }
}

void MainWindow::resizeCurrentColumns()
{
    auto *model = getCurrentModel();
    if (!model || !currentTree || currentTree == ui->treePopularApps || currentTree == ui->treeFlatpak
        || currentTree == ui->treeSnap) {
        return;
    }
    for (int i = 0; i < model->columnCount(); ++i) {
        if (!currentTree->isColumnHidden(i)) {
            currentTree->resizeColumnToContents(i);
        }
    }
}

bool MainWindow::shouldRefreshFilters(const QString &searchStr)
{
    auto *proxy = getCurrentProxy();
    if (!proxy) {
        qDebug() << "shouldRefreshFilters: no proxy";
        return true;
    }
    const bool statusMatch = proxy->statusFilter() == savedComboIndex;
    const bool searchMatch = proxy->searchText() == searchStr;
    const bool hideMatch = proxy->hideLibraries() == hideLibsChecked;
    return !(statusMatch && searchMatch && hideMatch);
}

void MainWindow::handleEnabledReposTab(const QString &searchStr)
{
    ui->searchBoxEnabled->setText(searchStr);
    changeList.clear();
    if (displayPackagesIsRunning) {
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
        connect(this, &MainWindow::displayPackagesFinished, this, &MainWindow::updateInterface,
                Qt::SingleShotConnection);
    } else if (enabledModel->rowCount() == 0 || dirtyEnabledRepos) {
        if (!buildPackageLists()) {
            QMessageBox::critical(this, tr("Error"),
                                  tr("Could not download the list of packages. Please check your APT sources."));
            currentTree->blockSignals(false);
            return;
        }
    }
    if (!displayPackagesIsRunning) {
        if (ui->comboFilterEnabled->currentIndex() != savedComboIndex) {
            ui->comboFilterEnabled->setCurrentIndex(savedComboIndex);
        }
        if (ui->comboFilterMX->currentIndex() != savedComboIndex) {
            ui->comboFilterMX->setCurrentIndex(savedComboIndex);
        }
        if (ui->comboFilterBP->currentIndex() != savedComboIndex) {
            ui->comboFilterBP->setCurrentIndex(savedComboIndex);
        }
        if (shouldRefreshFilters(searchStr)) {
            filterChanged(ui->comboFilterEnabled->currentText());
        } else {
            updateInterface();
            resizeCurrentColumns();
        }
    }
    if (!ui->searchBoxEnabled->text().isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
    }
    if (!displayPackagesIsRunning) {
        currentTree->blockSignals(false);
    }
}


void MainWindow::handleTab(const QString &searchStr, QLineEdit *searchBox, const QString &warningMessage,
                           bool dirtyFlag)
{
    if (searchBox) {
        searchBox->setText(searchStr);
    }
    if (!warningMessage.isEmpty()) {
        displayWarning(warningMessage);
    }
    changeList.clear();
    auto *model = getCurrentModel();
    if (displayPackagesIsRunning) {
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
        connect(this, &MainWindow::displayPackagesFinished, this, &MainWindow::updateInterface,
                Qt::SingleShotConnection);
    } else if (model && (model->rowCount() == 0 || dirtyFlag)) {
        if (!buildPackageLists()) {
            QMessageBox::critical(this, tr("Error"),
                                  tr("Could not download the list of packages. Please check your APT sources."));
            currentTree->blockSignals(false);
            return;
        }
    }
    if (Tab::Popular != ui->tabWidget->currentIndex() && !displayPackagesIsRunning) {
        if (ui->comboFilterEnabled->currentIndex() != savedComboIndex) {
            ui->comboFilterEnabled->setCurrentIndex(savedComboIndex);
        }
        if (ui->comboFilterMX->currentIndex() != savedComboIndex) {
            ui->comboFilterMX->setCurrentIndex(savedComboIndex);
        }
        if (ui->comboFilterBP->currentIndex() != savedComboIndex) {
            ui->comboFilterBP->setCurrentIndex(savedComboIndex);
        }
        if (shouldRefreshFilters(searchStr)) {
            filterChanged(ui->comboFilterEnabled->currentText());
        } else {
            updateInterface();
            resizeCurrentColumns();
        }
    }

    currentTree->blockSignals(false);
}

void MainWindow::handleFlatpakTab(const QString &searchStr)
{
    // The tab-change handler blocked treeFlatpak's signals before calling us;
    // restore them on every exit path.
    auto unblockGuard = qScopeGuard([this] { ui->treeFlatpak->blockSignals(false); });

    lastIndexClicked = QModelIndex();
    ui->searchBoxFlatpak->setText(searchStr);
    setCurrentTree();
    displayWarning("flatpaks");
    ui->searchBoxFlatpak->setFocus();
    // Binary presence, not checkInstalled()/installedPackages -- see the
    // constructor's flatpak preload for why that cache can't be trusted yet.
    const bool flatpakInstalled = !QStandardPaths::findExecutable(QStringLiteral("flatpak")).isEmpty();
    const bool isUserScope = fpUser.startsWith(QLatin1String("--user"));
    bool systemRemotesAdded = false;
    // Not gated on firstRunFP: the startup preload of displayFlatpaks() already
    // clears that flag before the user ever opens this tab.
    if (!flatpakSetupChecked && flatpakInstalled && !isUserScope) {
        if (!systemFlatpakDefaultsPresent()) {
            setCursor(QCursor(Qt::BusyCursor));
            Cmd helperCmd;
            if (runMxpiLibAsRoot(helperCmd, QStringLiteral("flatpak_add_repos"))) {
                flatpakSetupChecked = true;
                systemRemotesAdded = true;
                invalidateFlatpakRemoteCache();
            } else {
                // Authentication declined or setup failed: fall back to per-user
                // remotes (no admin rights needed) so the tab still works. The
                // combo change triggers the full user-scope setup and redisplay.
                // flatpakSetupChecked stays false: staying in user scope keeps
                // this gate off, but a deliberate return to system scope gets a
                // fresh setup attempt instead of an empty tab until restart.
                ui->comboUser->setCurrentIndex(1);
            }
            setCursor(QCursor(Qt::ArrowCursor));
        } else {
            flatpakSetupChecked = true;
        }
    }
    listFlatpakRemotes();
    if (systemRemotesAdded) {
        displayFlatpaks(true);
    }
    if (!firstRunFP && flatpakInstalled) {
        if (!searchStr.isEmpty()) {
            QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
        }
        if (!displayFlatpaksIsRunning) {
            filterChanged(ui->comboFilterFlatpak->currentText());
        }
        return;
    }
    firstRunFP = false;
    blockInterfaceFP();
    if (!flatpakInstalled) {
        int ans = QMessageBox::question(this, tr("Flatpak not installed"),
                                        tr("Flatpak is not currently installed.\nOK to go ahead and install it?"));
        if (ans == QMessageBox::No) {
#ifdef PACKAGE_BACKEND_PACMAN
            ui->tabWidget->setCurrentIndex(Tab::EnabledRepos);
#else
            ui->tabWidget->setCurrentIndex(Tab::Popular);
#endif
            enableTabs(true);
            return;
        }
        installFlatpak();
    } else {
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        setCursor(QCursor(Qt::ArrowCursor));
        if (ui->comboRemote->currentText().isEmpty()) {
            listFlatpakRemotes();
        }
        if (displayFlatpaksIsRunning) {
            if (!flatpakCancelHidden && pushCancel) {
                pushCancel->setEnabled(false);
                pushCancel->hide();
                flatpakCancelHidden = true;
            }
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
                qApp->processEvents();
            }
        }
        if (!searchStr.isEmpty()) {
            QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
        }
    }
}

void MainWindow::installFlatpak()
{
    beginOperation();
    ui->tabWidget->setTabEnabled(Tab::Output, true);
    ui->tabWidget->setCurrentWidget(ui->tabOutput);
    setCursor(QCursor(Qt::BusyCursor));
    showOutput();
    displayFlatpaksIsRunning = true;
    install("flatpak");
    if (operationWasCanceled()) {
        displayFlatpaksIsRunning = false;
        blockInterfaceFP();
        ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
        enableTabs(true);
        currentTree->blockSignals(false);
        return;
    }
    installedPackages = listInstalled();
    setDirty();
    buildPackageLists();
    if (!checkInstalled("flatpak")) {
        QMessageBox::critical(this, tr("Flatpak not installed"), tr("Flatpak was not installed"));
#ifdef PACKAGE_BACKEND_PACMAN
        ui->tabWidget->setCurrentIndex(Tab::EnabledRepos);
#else
        ui->tabWidget->setCurrentIndex(Tab::Popular);
#endif
        setCursor(QCursor(Qt::ArrowCursor));
        enableTabs(true);
        currentTree->blockSignals(false);
        return;
    }
    Cmd helperCmd;
    runMxpiLibAsRoot(helperCmd, QStringLiteral("flatpak_add_repos"));
    enableOutput();
    invalidateFlatpakRemoteCache();
    listFlatpakRemotes();
    if (displayFlatpaksIsRunning) {
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
    }
    setCursor(QCursor(Qt::ArrowCursor));
    ui->tabWidget->setTabText(Tab::Output, tr("Console Output"));
    ui->tabWidget->blockSignals(true);
    displayFlatpaks(true);
    ui->tabWidget->blockSignals(false);
    QMessageBox::warning(this, tr("Needs re-login"),
                         tr("You might need to logout/login to see installed items in the menu"));
    ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
    enableTabs(true);
}





void MainWindow::handleOutputTab()
{
    // Block signals and clear all search boxes
    const QList<QLineEdit *> searchBoxes
        = {ui->searchPopular,  ui->searchBoxEnabled, ui->searchBoxMX,
           ui->searchBoxBP,    ui->searchBoxFlatpak, ui->searchBoxSnap};

    for (auto searchBox : searchBoxes) {
        searchBox->blockSignals(true);
        searchBox->clear();
        searchBox->blockSignals(false);
    }

    // Disable install/uninstall buttons
    ui->pushInstall->setDisabled(true);
    ui->pushUninstall->setDisabled(true);
}

void MainWindow::filterChanged(const QString &arg1)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    currentTree->blockSignals(true);
    currentTree->setUpdatesEnabled(false);
    updateInterface();

    // Snap tab has its own simple filtering model; handle it up front and return.
    if (currentTree == ui->treeSnap) {
        if (snapModel) {
            snapModel->setAllChecked(false);
        }
        if (arg1 == tr("Search store")) {
            snapStoreMode = true;
            if (!ui->searchBoxSnap->text().isEmpty()) {
                searchSnapStore();
            }
            // Put the cursor in the search box so the user can type a query right away
            // (queued so it survives the combo popup closing and regaining focus).
            QMetaObject::invokeMethod(this, [this] { ui->searchBoxSnap->setFocus(); }, Qt::QueuedConnection);
        } else { // Installed snaps
            snapStoreMode = false;
            ui->searchBoxSnap->blockSignals(true);
            ui->searchBoxSnap->clear();
            ui->searchBoxSnap->blockSignals(false);
            if (snapProxy) {
                snapProxy->setStatusFilter(0);
                snapProxy->setSearchText(QString());
            }
            displaySnaps(true);
        }
        changeList.clear();
        ui->pushInstall->setEnabled(false);
        ui->pushUninstall->setEnabled(false);
        QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
        setSearchFocus();
        currentTree->setUpdatesEnabled(true);
        currentTree->blockSignals(false);
        return;
    }

    // Helper functions
    auto resetTree = [this]() {
        // Optimization: Disable updates during bulk operations
        currentTree->setUpdatesEnabled(false);
        if (currentTree == ui->treeFlatpak) {
            if (flatpakModel) {
                flatpakModel->setAllChecked(false);
            }
            if (flatpakProxy) {
                flatpakProxy->setSearchText(QString());
                flatpakProxy->setStatusFilter(0);
                flatpakProxy->clearAllowedRefs();
            }
        } else {
            auto *model = getCurrentModel();
            auto *proxy = getCurrentProxy();
            if (model) {
                model->uncheckAll();
            }
            if (proxy) {
                proxy->setSearchText(QString());
                proxy->setStatusFilter(0); // Reset status filter to show all packages
            }
        }
        currentTree->setUpdatesEnabled(true);

        QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
        setSearchFocus();
        ui->pushInstall->setEnabled(false);
        ui->pushUninstall->setEnabled(false);
    };

    auto uncheckAllItems = [this]() {
        // Optimization: Disable updates during bulk operations
        currentTree->setUpdatesEnabled(false);
        if (currentTree == ui->treeFlatpak) {
            if (flatpakModel) {
                flatpakModel->setAllChecked(false);
            }
        } else {
            auto *model = getCurrentModel();
            if (model) {
                model->uncheckAll();
            }
        }
        currentTree->setUpdatesEnabled(true);
    };

    auto handleFlatpakFilter = [this, uncheckAllItems](const QStringList &data, bool raw = true) {
        uncheckAllItems();
        displayFilteredFP(data, raw);
    };

    auto updateButtonStates = [this](bool installEnabled, bool uninstallEnabled) {
        ui->pushInstall->setEnabled(installEnabled);
        ui->pushUninstall->setEnabled(uninstallEnabled);
    };

    auto clearChangeListAndButtons = [this, updateButtonStates]() {
        updateButtonStates(false, false);
        changeList.clear();
    };

    auto blockSignalsForAll = [this](bool block) {
        ui->checkHideLibs->blockSignals(block);
        ui->checkHideLibsBP->blockSignals(block);
        ui->checkHideLibsMX->blockSignals(block);
    };



    // Hide and reset all header checkboxes by default
    if (headerEnabled) {
        headerEnabled->setCheckboxVisible(false);
        headerEnabled->setChecked(false);
    }
    if (headerMX) {
        headerMX->setCheckboxVisible(false);
        headerMX->setChecked(false);
    }
    if (headerBP) {
        headerBP->setCheckboxVisible(false);
        headerBP->setChecked(false);
    }

    bool isAutoremovable = (arg1 == tr("Autoremovable"));
    bool shouldHideLibs = !isAutoremovable && hideLibsChecked;

    // Handle Flatpak tree
    if (currentTree == ui->treeFlatpak) {
        if (arg1 == tr("Installed runtimes")) {
            handleFlatpakFilter(installedRuntimesFP, false);
            clearChangeListAndButtons();
        } else if (arg1 == tr("Installed apps")) {
            handleFlatpakFilter(installedAppsFP, false);
            clearChangeListAndButtons();
        } else if (arg1 == tr("All apps")) {
            if (flatpaksApps.isEmpty()) {
                flatpaksApps = listFlatpaks(ui->comboRemote->currentText(), fpUser, "--app");
            }
            handleFlatpakFilter(flatpaksApps);
            clearChangeListAndButtons();
        } else if (arg1 == tr("All runtimes")) {
            if (flatpaksRuntimes.isEmpty()) {
                flatpaksRuntimes = listFlatpaks(ui->comboRemote->currentText(), fpUser, "--runtime");
            }
            handleFlatpakFilter(flatpaksRuntimes);
            clearChangeListAndButtons();
        } else if (arg1 == tr("All available")) {
            resetTree();
            ui->labelNumAppFP->setText(QString::number(flatpakModel->rowCount()));
            clearChangeListAndButtons();
        } else if (arg1 == tr("All installed")) {
            displayFilteredFP(installedAppsFP + installedRuntimesFP);
        } else if (arg1 == tr("Not installed")) {
            flatpakProxy->clearAllowedRefs();
            flatpakProxy->setStatusFilter(Status::NotInstalled);
            ui->pushUninstall->setEnabled(false);
        }
        QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
        setSearchFocus();
    } else if (arg1 == tr("All packages")) {
        savedComboIndex = 0;
        blockSignalsForAll(true);

        ui->checkHideLibs->setChecked(shouldHideLibs);
        ui->checkHideLibsMX->setChecked(shouldHideLibs);
        ui->checkHideLibsBP->setChecked(shouldHideLibs);
        blockSignalsForAll(false);
        if (auto *proxy = getCurrentProxy()) {
            proxy->setHideLibraries(shouldHideLibs);
        }
        resetTree();
        clearChangeListAndButtons();
        ui->pushInstall->setText(isAutoremovable ? tr("Mark keep") : tr("Install"));
    } else {
        blockSignalsForAll(true);
        ui->checkHideLibs->setChecked(shouldHideLibs);
        ui->checkHideLibsMX->setChecked(shouldHideLibs);
        ui->checkHideLibsBP->setChecked(shouldHideLibs);
        blockSignalsForAll(false);
        if (auto *proxy = getCurrentProxy()) {
            proxy->setHideLibraries(shouldHideLibs);
        }

        ui->pushInstall->setText(isAutoremovable ? tr("Mark keep") : tr("Install"));

        const QHash<QString, int> statusMap {{tr("Installed"), Status::Installed},
                                             {tr("Upgradable"), Status::Upgradable},
                                             {tr("Not installed"), Status::NotInstalled},
                                             {tr("Autoremovable"), Status::Autoremovable}};

        if (auto itStatus = statusMap.find(arg1); itStatus != statusMap.end()) {
            savedComboIndex = itStatus.value();
            auto *proxy = getCurrentProxy();
            if (proxy) {
                proxy->setStatusFilter(itStatus.value());
            }
            bool hasVisibleMatches = proxy ? proxy->rowCount() > 0 : false;
            // Show the header checkbox when filtering Upgradable or Autoremovable
            if (itStatus.value() == Status::Upgradable || itStatus.value() == Status::Autoremovable) {
                const QString tip = (itStatus.value() == Status::Upgradable) ? tr("Select/deselect all upgradable")
                                                                             : tr("Select/deselect all autoremovable");
                const bool allowAutoremovableCheckbox = itStatus.value() != Status::Autoremovable || hasVisibleMatches;

                if (currentTree == ui->treeEnabled && headerEnabled) {
                    headerEnabled->setCheckboxVisible(allowAutoremovableCheckbox);
                    if (allowAutoremovableCheckbox) {
                        headerEnabled->setToolTip(tip);
                        headerEnabled->resizeSection(TreeCol::Check, qMax(headerEnabled->sectionSize(0), 22));
                    }
                } else if (currentTree == ui->treeMXtest && headerMX) {
                    headerMX->setCheckboxVisible(allowAutoremovableCheckbox);
                    if (allowAutoremovableCheckbox) {
                        headerMX->setToolTip(tip);
                        headerMX->resizeSection(TreeCol::Check, qMax(headerMX->sectionSize(0), 22));
                    }
                } else if (currentTree == ui->treeBackports && headerBP) {
                    headerBP->setCheckboxVisible(allowAutoremovableCheckbox);
                    if (allowAutoremovableCheckbox) {
                        headerBP->setToolTip(tip);
                        headerBP->resizeSection(TreeCol::Check, qMax(headerBP->sectionSize(0), 22));
                    }
                }
            }
        }
        uncheckAllItems();
        QMetaObject::invokeMethod(this, [this] { findPackage(); }, Qt::QueuedConnection);
        setSearchFocus();
        clearChangeListAndButtons();
    }
    resizeCurrentColumns();
    currentTree->setUpdatesEnabled(true);
    currentTree->blockSignals(false);
}

// Toggle selection of all visible upgradable items in the current tab
void MainWindow::selectAllUpgradable_toggled(bool checked)
{
    QTreeView *tree = nullptr;
    PackageModel *model = nullptr;
    QObject *s = sender();
    if (s == headerEnabled) {
        tree = ui->treeEnabled;
        model = enabledModel;
    } else if (s == headerMX) {
        tree = ui->treeMXtest;
        model = mxtestModel;
    } else if (s == headerBP) {
        tree = ui->treeBackports;
        model = backportsModel;
#ifdef PACKAGE_BACKEND_PACMAN
    } else if (s == headerAUR) {
        tree = ui->treeAUR;
        model = aurModel;
#endif
    } else {
        return;
    }

    // Batch update: avoid emitting itemChanged for every item
    tree->setUpdatesEnabled(false);
    tree->blockSignals(true);
    // Determine desired status based on current filter text
    int targetStatus = Status::Upgradable;
    QString filterText;
    if (tree == ui->treeEnabled) {
        filterText = ui->comboFilterEnabled->currentText();
    } else if (tree == ui->treeMXtest) {
        filterText = ui->comboFilterMX->currentText();
    } else if (tree == ui->treeBackports) {
        filterText = ui->comboFilterBP->currentText();
    }
    if (filterText == tr("Autoremovable")) {
        targetStatus = Status::Autoremovable;
    }

    // Get proxy for the tree to respect filters (search, hide libraries, etc.)
    PackageFilterProxy *proxy = nullptr;
    if (tree == ui->treeEnabled) {
        proxy = enabledProxy;
    } else if (tree == ui->treeMXtest) {
        proxy = mxtestProxy;
    } else if (tree == ui->treeBackports) {
        proxy = backportsProxy;
    }

    // Only check VISIBLE rows (respecting search and other filters)
    if (proxy) {
        QVector<int> visibleRows = proxy->visibleSourceRows();
        model->setCheckedForVisible(visibleRows, checked);
    }

    // Rebuild changeList and update buttons once, instead of per-item
    changeList = model->checkedPackageNames();

    // Update action buttons coherently after batch toggle
    ui->pushInstall->setEnabled(!changeList.isEmpty());
    if (tree != ui->treeFlatpak) {
        ui->pushUninstall->setEnabled(checkInstalled(changeList));
        if (targetStatus == Status::Autoremovable) {
            ui->pushInstall->setText(tr("Mark keep"));
        } else {
            ui->pushInstall->setText(checkUpgradable(changeList) ? tr("Upgrade") : tr("Install"));
        }
    }

    tree->blockSignals(false);
    tree->setUpdatesEnabled(true);
}

void MainWindow::onPackageCheckStateChanged(const QString &packageName, Qt::CheckState state)
{
    buildChangeList(packageName, state);
}

void MainWindow::onFlatpakCheckStateChanged(const QString &fullName, Qt::CheckState state, int status)
{
    buildFlatpakChangeList(fullName, state, status);
}

void MainWindow::onPopularItemChanged(const QModelIndex &index)
{
    Q_UNUSED(index)

    // Check all checked items to determine button states
    bool hasCheckedItems = false;
    bool allCheckedAreInstalled = true;

    QModelIndexList checkedItems = popularModel->checkedItems();
    hasCheckedItems = !checkedItems.isEmpty();

    for (const QModelIndex &idx : checkedItems) {
        const PopularAppData *app = popularModel->getAppData(idx);
        if (app && !app->isInstalled) {
            allCheckedAreInstalled = false;
            break;
        }
    }

    // Update button states based on checked items
    ui->pushInstall->setEnabled(hasCheckedItems);
    ui->pushUninstall->setEnabled(hasCheckedItems && allCheckedAreInstalled);
    ui->pushInstall->setText(hasCheckedItems && allCheckedAreInstalled ? tr("Reinstall") : tr("Install"));
}

// Build the changeList when selecting an item in the tree (for APT packages)
void MainWindow::buildChangeList(const QString &packageName, Qt::CheckState state)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    /* if all apps are uninstalled (or some installed) -> enable Install, disable Uinstall
     * if all apps are installed or upgradable -> enable Uninstall, enable Install
     * if all apps are upgradable -> change Install label to Upgrade;
     */

    if (state == Qt::Checked) {
        ui->pushInstall->setEnabled(true);
        changeList.append(packageName);
    } else {
        changeList.removeOne(packageName);
    }

    ui->pushUninstall->setEnabled(checkInstalled(changeList));
    ui->pushInstall->setText(checkUpgradable(changeList) ? tr("Upgrade") : tr("Install"));
    if (currentTabFiltersAutoremovable()) {
        ui->pushInstall->setText(tr("Mark keep"));
    }

    if (changeList.isEmpty()) {
        ui->pushInstall->setEnabled(false);
        ui->pushUninstall->setEnabled(false);
    }
}

// Build the changeList for Flatpak packages
void MainWindow::buildFlatpakChangeList(const QString &fullName, Qt::CheckState state, int /*status*/)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    if (changeList.isEmpty() && indexFilterFP.isEmpty()) {
        indexFilterFP = ui->comboFilterFlatpak->currentText();
    }

    if (state == Qt::Checked) {
        changeList.append(fullName);
    } else {
        changeList.removeOne(fullName);
    }

    // Enable each action based on the whole selection, not just the last toggle:
    // Install if any selected flatpak is not installed, Uninstall if any is installed.
    // The install/uninstall handlers split the selection by status accordingly.
    bool anyInstalled = false;
    bool anyNotInstalled = false;
    for (const QString &selectedName : std::as_const(changeList)) {
        const int row = flatpakModel ? flatpakModel->findRowByFullName(selectedName) : -1;
        const FlatpakData *fp = (row >= 0) ? flatpakModel->flatpakAt(row) : nullptr;
        if (fp && fp->status == Status::Installed) {
            anyInstalled = true;
        } else {
            anyNotInstalled = true;
        }
    }

    ui->pushInstall->setText(tr("Install"));
    ui->pushInstall->setEnabled(!changeList.isEmpty() && anyNotInstalled);
    ui->pushUninstall->setEnabled(!changeList.isEmpty() && anyInstalled);

    if (changeList.isEmpty()) {
        ui->comboFilterFlatpak->setCurrentText(indexFilterFP);
        indexFilterFP.clear();
    }
    ui->treeFlatpak->setFocus();
}

// Force repo upgrade — shared implementation for APT tabs
void MainWindow::forceUpdateAptTab(QLineEdit *searchBox, QComboBox *filterCombo)
{
    beginOperation();
    searchBox->clear();
    filterCombo->setCurrentIndex(0);
    buildPackageLists(true);
    if (displayPackagesIsRunning) {
        // pacman only: the fetch this just kicked off hasn't completed yet --
        // its own completion runs displayPackages(), which already emits
        // displayPackagesFinished(); wait for that instead of acting on stale data.
        connect(this, &MainWindow::displayPackagesFinished, this, &MainWindow::updateInterface,
                Qt::SingleShotConnection);
    } else {
        updateInterface();
    }
}

void MainWindow::pushForceUpdateEnabled_clicked()
{
    forceUpdateAptTab(ui->searchBoxEnabled, ui->comboFilterEnabled);
}

void MainWindow::pushForceUpdateMX_clicked()
{
    forceUpdateAptTab(ui->searchBoxMX, ui->comboFilterMX);
}

void MainWindow::pushForceUpdateFP_clicked()
{
    ui->searchBoxFlatpak->clear();
    if (!flatpakCancelHidden && pushCancel) {
        pushCancel->setEnabled(false);
        pushCancel->hide();
        flatpakCancelHidden = true;
    }
    holdProgressForFlatpakRefresh = true;
    progress->show();
    if (!cmd.proc("flatpak", {"update", "--appstream"})) {
        holdProgressForFlatpakRefresh = false;
        progress->hide();
        if (flatpakCancelHidden && pushCancel) {
            pushCancel->show();
            pushCancel->setEnabled(true);
            flatpakCancelHidden = false;
        }
        QMessageBox::critical(this, tr("Error"),
                              tr("Problem detected during last operation, please inspect the console output."));
        updateInterface();
        return;
    }
    displayFlatpaks(true);
    updateInterface();
}

void MainWindow::pushForceUpdateBP_clicked()
{
    forceUpdateAptTab(ui->searchBoxBP, ui->comboFilterBP);
}

// Hide/unhide lib/-dev packages — shared implementation
void MainWindow::applyHideLibs(bool checked, QTreeView *tree, PackageFilterProxy *proxy, QComboBox *filterCombo,
                                const QList<QCheckBox *> &peerCheckboxes)
{
    tree->setUpdatesEnabled(false);
    hideLibsChecked = checked;
    settings.setValue("HideLibs", checked);
    for (auto *cb : peerCheckboxes) {
        cb->setChecked(checked);
    }
    proxy->setHideLibraries(checked);
    filterChanged(filterCombo->currentText());
    tree->setUpdatesEnabled(true);
}

void MainWindow::checkHideLibs_toggled(bool checked)
{
    applyHideLibs(checked, ui->treeEnabled, enabledProxy, ui->comboFilterEnabled,
                  {ui->checkHideLibsMX, ui->checkHideLibsBP});
}

void MainWindow::pushUpgradeAll_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    beginOperation();
    showOutput();

    QStringList names;
    for (int i = 0; i < enabledModel->rowCount(); ++i) {
        const PackageData *pkg = enabledModel->packageAt(i);
        if (pkg && pkg->status == Status::Upgradable) {
            names.append(pkg->name);
        }
    }
    bool success = install(names.join(' '));
    rebuildPackageViews();
    if (success && !operationWasCanceled()) {
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    } else if (!operationWasCanceled()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Problem detected while installing, please inspect the console output."));
    } else {
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    }
    enableTabs(true);
}

// Pressing Enter or buttonEnter will do the same thing
void MainWindow::pushEnter_clicked()
{
    if (currentTree == ui->treeFlatpak
        && ui->lineEdit->text().isEmpty()) { // Add "Y" as default response for flatpaks to work like apt-get
        cmd.write("y");
    }
    lineEdit_returnPressed();
}

void MainWindow::lineEdit_returnPressed()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    const QString input = ui->lineEdit->text();
    cmd.write(input.toUtf8() + '\n');
#ifdef PACKAGE_BACKEND_PACMAN
    // Never echo back what was typed while masked (a sudo password, per
    // outputAvailable()'s detection below) -- echoing the raw text here would leak
    // it into the Output tab regardless of the QLineEdit's own echo mode.
    if (!lineEditMasked) {
        ui->outputBox->appendPlainText(input + '\n');
    }
    setLineEditMasked(false);
#else
    ui->outputBox->appendPlainText(input + '\n');
#endif
    ui->lineEdit->clear();
    ui->lineEdit->setFocus();
}

#ifdef PACKAGE_BACKEND_PACMAN
void MainWindow::setLineEditMasked(bool masked)
{
    if (!ui || !ui->lineEdit) {
        return;
    }
    lineEditMasked = masked;
    ui->lineEdit->setEchoMode(masked ? QLineEdit::Password : QLineEdit::Normal);
    if (lineEditToggleMaskAction) {
        const QSignalBlocker blocker(lineEditToggleMaskAction);
        lineEditToggleMaskAction->setChecked(masked);
        const QIcon icon = QIcon::fromTheme(masked ? "view-hidden" : "view-visible", QIcon::fromTheme("view-visible"));
        if (!icon.isNull()) {
            lineEditToggleMaskAction->setIcon(icon);
        }
        lineEditToggleMaskAction->setToolTip(masked ? tr("Show input") : tr("Hide input"));
    }
}
#endif

void MainWindow::pushCancel_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (cmd.state() != QProcess::NotRunning) {
        if (QMessageBox::warning(this, tr("Quit?"),
                                 tr("Process still running, quitting might leave the system in an unstable "
                                    "state.<p><b>Are you sure you want to exit MX Package Installer?</b>"),
                                 QMessageBox::Yes, QMessageBox::No)
            == QMessageBox::No) {
            return;
        }
    }
    cleanup();
    QApplication::quit();
}

void MainWindow::checkHideLibsMX_clicked(bool checked)
{
    applyHideLibs(checked, ui->treeMXtest, mxtestProxy, ui->comboFilterMX,
                  {ui->checkHideLibs, ui->checkHideLibsBP});
}

void MainWindow::checkHideLibsBP_clicked(bool checked)
{
    applyHideLibs(checked, ui->treeBackports, backportsProxy, ui->comboFilterBP,
                  {ui->checkHideLibs, ui->checkHideLibsMX});
}

void MainWindow::checkRepoOnlyMX_clicked(bool checked)
{
    ui->treeMXtest->setUpdatesEnabled(false);
    settings.setValue("RepoOnly", checked);
    ui->checkRepoOnlyBP->setChecked(checked);
    mxtestProxy->setRepoOnly(checked);
    backportsProxy->setRepoOnly(checked);
    filterChanged(ui->comboFilterMX->currentText());
    ui->labelNumApps_2->setText(QString::number(mxtestProxy->rowCount()));
    ui->treeMXtest->setUpdatesEnabled(true);
}

void MainWindow::checkRepoOnlyBP_clicked(bool checked)
{
    ui->treeBackports->setUpdatesEnabled(false);
    settings.setValue("RepoOnly", checked);
    ui->checkRepoOnlyMX->setChecked(checked);
    mxtestProxy->setRepoOnly(checked);
    backportsProxy->setRepoOnly(checked);
    filterChanged(ui->comboFilterBP->currentText());
    ui->labelNumApps_3->setText(QString::number(backportsProxy->rowCount()));
    ui->treeBackports->setUpdatesEnabled(true);
}

// On change flatpak remote
void MainWindow::comboRemote_activated(int /*index*/)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    lastIndexClicked = QModelIndex();
    displayFlatpaks(true);
}

void MainWindow::pushUpgradeFP_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    enableOutput();
    if (cmd.procOnPty(QStringLiteral("flatpak"), {"update", fpUser.trimmed()})) {
        appendFlatpakStatusMessage(ui->outputBox, tr("Update complete."));
        displayFlatpaks(true);
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
    } else {
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::critical(this, tr("Error"),
                              tr("Problem detected while installing, please inspect the console output."));
    }
    enableTabs(true);
}

void MainWindow::pushRemotes_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ManageRemotes dialog(this, fpUser);
    dialog.exec();
    if (dialog.isChanged()) {
        invalidateFlatpakRemoteCache();
        listFlatpakRemotes();
        displayFlatpaks(true);
    }
    if (!dialog.getInstallRef().isEmpty()) {
        showOutput();
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        QStringList args {"install", "-y"};
        args << dialog.getUser().trimmed();
        args << "--from" << dialog.getInstallRef();
        if (cmd.procOnPty(QStringLiteral("flatpak"), args)) {
            appendFlatpakStatusMessage(ui->outputBox, tr("Install complete."));
            invalidateFlatpakRemoteCache();
            listFlatpakRemotes();
            displayFlatpaks(true);
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->blockSignals(true);
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            ui->tabWidget->blockSignals(false);
        } else {
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::critical(this, tr("Error"),
                                  tr("Problem detected while installing, please inspect the console output."));
        }
        enableTabs(true);
    }
}

void MainWindow::comboUser_currentIndexChanged(int index)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (index == 0) {
        fpUser = "--system ";
    } else {
        fpUser = "--user ";
        static bool updated = false;
        if (!updated) {
            setCursor(QCursor(Qt::BusyCursor));
            enableOutput();
            Cmd helperCmd;
            runMxpiLib(helperCmd, QStringLiteral("flatpak_add_repos_user"));
            setCursor(QCursor(Qt::ArrowCursor));
            updated = true;
        }
    }
    lastIndexClicked = QModelIndex();
    invalidateFlatpakRemoteCache();
    listFlatpakRemotes();
    displayFlatpaks(true);
}

void MainWindow::treePopularApps_customContextMenuRequested(QPoint pos)
{
    QModelIndex index = ui->treePopularApps->indexAt(pos);
    if (!index.isValid() || !index.parent().isValid()) { // skip invalid and categories
        return;
    }
    QMenu menu(this);
    // Parented to &menu (not this) so it's destroyed along with the menu below --
    // QMenu::addAction(QAction*) doesn't take ownership, so parenting to the
    // long-lived MainWindow would leak one QAction per invocation.
    auto *action = new QAction(QIcon::fromTheme("dialog-information"), tr("More &info..."), &menu);
    action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I)); // hint only; the QShortcut handles the key
    menu.addAction(action);
    connect(action, &QAction::triggered, this, [this, index] { displayPopularInfo(index); });
    menu.exec(ui->treePopularApps->mapToGlobal(pos));
}

// Process keystrokes
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (cmd.state() != QProcess::NotRunning) {
        if (QMessageBox::warning(this, tr("Quit?"),
                                 tr("Process still running, quitting might leave the system in an unstable "
                                    "state.<p><b>Are you sure you want to exit MX Package Installer?</b>"),
                                 QMessageBox::Yes, QMessageBox::No)
            == QMessageBox::No) {
            event->ignore();
            return;
        }
        cleanup();
    }
    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        pushCancel_clicked();
    } else if (event->matches(QKeySequence::Find)
               || (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_F)) {
        if (ui->tabWidget->currentWidget() == ui->tabPopular) {
            ui->searchPopular->setFocus();
        } else if (ui->tabWidget->currentWidget() == ui->tabEnabled) {
            ui->searchBoxEnabled->setFocus();
        } else if (ui->tabWidget->currentWidget() == ui->tabMXtest) {
            ui->searchBoxMX->setFocus();
        } else if (ui->tabWidget->currentWidget() == ui->tabBackports) {
            ui->searchBoxBP->setFocus();
#ifdef PACKAGE_BACKEND_PACMAN
        } else if (ui->tabWidget->currentWidget() == ui->tabAUR) {
            ui->searchBoxAUR->setFocus();
#endif
        } else if (ui->tabWidget->currentWidget() == ui->tabFlatpak) {
            ui->searchBoxFlatpak->setFocus();
        } else if (ui->tabWidget->currentWidget() == ui->tabSnap) {
            ui->searchBoxSnap->setFocus();
        }
    }
}

void MainWindow::pushRemoveUnused_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    enableOutput();
    showFlatpakProgress(tr("Uninstalling flatpaks..."));
    if (cmd.procOnPty(QStringLiteral("flatpak"), {"uninstall", "--unused", "-y"})) {
        appendFlatpakStatusMessage(ui->outputBox, tr("Uninstall complete."));
        showFlatpakProgress(tr("Refreshing flatpaks..."));
        displayFlatpaks(true);
        progress->hide();
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
    } else {
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::critical(this, tr("Error"),
                              tr("Problem detected during last operation, please inspect the console output."));
    }
    enableTabs(true);
}

QString MainWindow::getMXTestRepoUrl()
{
    // Try to get test repo URL directly
    QString output;
    if (cmd.pipeline({
            {"apt-get", {"update", "--print-uris"}},
            {"tac", {}},
            {"grep", {"-m1", "-oP", "https?://.*/mx/testrepo/dists/(?=" + verName + "/test/)"}},
        },
        &output)) {
        return output;
    }

    // Fall back to deriving from main repo URL
    if (cmd.pipeline({
            {"apt-get", {"update", "--print-uris"}},
            {"tac", {}},
            {"grep", {"-m1", "-oE", "https?://.*/mx/repo/dists/" + verName + "/main/"}},
            {"sed", {"-e", "s:/mx/repo/dists/" + verName + "/main/:/mx/testrepo/dists/:"}},
            {"grep", {"-oE", "https?://.*/mx/testrepo/dists/"}},
        },
        &output)) {
        return output;
    }

    // Return default URL if nothing else works
    return "https://mxrepo.com/mx/testrepo/dists/";
}

void MainWindow::pushRemoveAutoremovable_clicked()
{
    beginOperation();
    QString names = PackageBackend::autoremovableCandidates(cmd).join(' ');
    QMessageBox::warning(this, tr("Warning"),
                         tr("Potentially dangerous operation.\nPlease make sure you check "
                            "carefully the list of packages to be removed."));
    showOutput();
    bool success = uninstall(names);
    rebuildPackageViews();
    if (success && !operationWasCanceled()) {
        QMessageBox::information(this, tr("Success"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    } else if (!operationWasCanceled()) {
        QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling the program"));
    } else {
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    }
    enableTabs(true);
    ui->tabWidget->setCurrentIndex(Tab::EnabledRepos);
}
