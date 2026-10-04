/**********************************************************************
 *  mxpackageinstaller.h
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
#pragma once

#include <QCloseEvent>
#include <QCommandLineParser>
#include <QDomDocument>
#include <QFile>
#include <QHash>
#include <QMessageBox>
#include <QModelIndex>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardItem>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>

#include <functional>
#include <memory>

#include "aptcache.h"
#include "cmd.h"
#include "checkableheaderview.h"
#include "lockfile.h"
#include "models/flatpakfilterproxy.h"
#include "models/flatpakmodel.h"
#include "models/packagefilterproxy.h"
#include "models/packagemodel.h"
#include "models/popularfilterproxy.h"
#include "models/popularmodel.h"
#include "models/snapfilterproxy.h"
#include "models/snapmodel.h"
#include "outputrender.h"
#include "remotes.h"
#include "versionnumber.h"

namespace Ui
{
class MainWindow;
}

class QComboBox;
class QNetworkReply;

namespace Tab
{
// AUR sits between EnabledRepos and Test to match its position in the shared
// mainwindow.ui (inserted there to mirror the arch branch's original tab order).
// It's hidden via setTabVisible() for the apt backend, which has no AUR tab logic.
enum { Popular, EnabledRepos, AUR, Test, Backports, Flatpak, Snap, Output };
}

namespace Release
{
enum { Jessie = 8, Stretch, Buster, Bullseye, Bookworm, Trixie };
}

struct PopularInfo {
    QString category;
    QString name;
    QString description;
    QString installable;
    QString screenshot;
    QString preInstall;
    QString postInstall;
    QString installNames;
    QString uninstallNames;
    QString postUninstall;
    QString preUninstall;
};

constexpr uint KiB = 1024;
constexpr uint MiB = KiB * 1024;
constexpr uint GiB = MiB * 1024;

class MainWindow : public QDialog
{
    Q_OBJECT

#ifdef MXPI_TESTING
    // Test-only seam: Testing/test_mainwindow.cpp needs to inspect/drive otherwise-
    // private async-orchestration state directly (flatpakRequestGeneration,
    // enabledReposRenderedWithoutInstalled, currentTree, etc.) to exercise it
    // deterministically instead of racing real background subprocess timing.
    // MXPI_TESTING is only ever defined by that one test target, never in the
    // production build, so this has no effect there.
    friend class TestMainWindowAsync;
#endif

public:
#ifdef MXPI_TESTING
    // Test-only hooks for the constructor's first background preload worker
    // (AptCache's, apt-only, or installedPackages', pacman-only), captured BY
    // VALUE into the worker lambda itself -- same as guardMutex/destructingFlag
    // already are -- so the worker never needs to read this via `this` (no new
    // data race), and each hook stays valid for as long as the worker's own copy
    // does regardless of what the test function that set it up does afterward.
    //   beforeGuardCheck: called immediately after the worker's real (apt/pacman)
    //     work finishes, immediately before it locks destructionGuardMutex/checks
    //     destructing -- its one and only touch of `this`. Lets a test pause the
    //     worker there, destroy the window while it's provably still in flight,
    //     then resume it, instead of racing real subprocess/thread-pool timing.
    //   afterGuardPassed: called only if the guard check let the worker through
    //     (destructing was false) -- i.e., only on the branch that's about to
    //     touch `this` via QMetaObject::invokeMethod(). In a correct test
    //     scenario (window already destroyed, destructing == true) this must
    //     never fire; a test asserting it stays unset is what actually proves
    //     the guard suppressed the touch, rather than merely hoping a
    //     use-after-free happens to crash or get caught by a sanitizer.
    //   workerFinished: called unconditionally once the guarded section (lock,
    //     check, and whichever of the two branches above applies) has fully run
    //     -- via a scope guard, so it fires the same way regardless of which
    //     branch was taken. Lets a test wait for a real completion signal before
    //     asserting on afterGuardPassed, instead of assuming the worker resumed
    //     and finished within some fixed time budget after being released.
    struct BackgroundWorkerTestHooks
    {
        std::function<void()> beforeGuardCheck;
        std::function<void()> afterGuardPassed;
        std::function<void()> workerFinished;
    };

    // Test-only extra parameters, never present in a production build (the whole
    // signature reverts to the plain one below when MXPI_TESTING isn't defined).
    // testSkipFlatpakPreload prevents both the constructor's own Flatpak preload
    // chain and, once set, any later displayFlatpaks() call from going on to
    // touch real flatpak remotes -- one of that code's fallback paths ("flatpak
    // update --appstream") can genuinely reach the network, which a test run
    // must never do. Neither parameter is ever surfaced as a command-line
    // option; a production build has no way to set either at all.
    explicit MainWindow(const QCommandLineParser &argParser, QWidget *parent = nullptr,
                        BackgroundWorkerTestHooks testHooks = {}, bool testSkipFlatpakPreload = false);
#else
    explicit MainWindow(const QCommandLineParser &argParser, QWidget *parent = nullptr);
#endif
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

signals:
    void displayPackagesFinished();

private slots:
    void checkUncheckItem();
    void cleanup();
    void cmdDone();
    void cmdStart();
    void disableOutput();
    void displayInfoTestOrBackport(QTreeView *tree, const QModelIndex &index);
    void displayPackageInfo(QTreeView *tree, QPoint pos);
    void displayPackageInfo(const QModelIndex &index);
    void displayInfo(QTreeView *tree, const QModelIndex &index);
    void displayFlatpakInfo(const QModelIndex &index);
    void displaySnapInfo(const QModelIndex &index);
    void displayPopularInfo(const QModelIndex &index);
    void enableOutput();
    void filterChanged(const QString &arg1);
    void findPackage();
    void findPopular();
    void outputAvailable(const QString &output);
    void showOutput();
    void updateBar();

    void checkHideLibsBP_clicked(bool checked);
    void checkHideLibsMX_clicked(bool checked);
    void checkHideLibs_toggled(bool checked);
    void checkRepoOnlyMX_clicked(bool checked);
    void checkRepoOnlyBP_clicked(bool checked);
    void selectAllUpgradable_toggled(bool checked);
    void comboRemote_activated(int index = 0);
    void comboUser_currentIndexChanged(int index);
    void lineEdit_returnPressed();
    void pushAbout_clicked();
    void pushCancel_clicked();
    void pushEnter_clicked();
    void pushForceUpdateBP_clicked();
    void pushForceUpdateEnabled_clicked();
    void pushForceUpdateMX_clicked();
    void pushForceUpdateFP_clicked();
#ifdef PACKAGE_BACKEND_PACMAN
    void pushForceUpdateAUR_clicked();
#endif
    void pushHelp_clicked();
    void pushInstall_clicked();
    void pushRemotes_clicked();
    void pushRemoveAutoremovable_clicked();
    void pushRemoveUnused_clicked();
    void pushUninstall_clicked();
    void pushUpgradeAll_clicked();
    void pushUpgradeFP_clicked();
    void pushRefreshSnap_clicked();
    void pushUpgradeSnap_clicked();
    void pushSetupSnapd_clicked();
    void searchSnapStore();
    void tabWidget_currentChanged(int index);
    void onPackageCheckStateChanged(const QString &packageName, Qt::CheckState state);
    void onFlatpakCheckStateChanged(const QString &fullName, Qt::CheckState state, int status);
    void onSnapCheckStateChanged(const QString &name, Qt::CheckState state, int status);
    void onPopularItemChanged(const QModelIndex &index);
    void treePopularApps_customContextMenuRequested(QPoint pos);
    void treePopularApps_expanded();
    void treePopularApps_itemCollapsed(const QModelIndex &index);
    void treePopularApps_itemExpanded(const QModelIndex &index);

private:
    Ui::MainWindow *ui;
    OutputRender::OutputRenderer outputRenderer;

    QString indexFilterFP;
    bool cleanupDone {false};
    bool dirtyBackports {true};
    bool dirtyEnabledRepos {true};
    bool dirtyTest {true};
    bool displayFlatpaksIsRunning {false};
    bool displayPackagesIsRunning {false};
    bool firstRunFP {true};
    bool flatpakSetupChecked {false};
    bool firstRunSnap {true};
    bool snapStoreMode {false};
    bool hideLibsChecked {true};
    bool testInitiallyEnabled {false};
    bool updatedOnce {false};
    bool operationInProgress {false};
    bool warningBackports {false};
    bool warningFlatpaks {false};
    bool warningTest {false};
#ifdef PACKAGE_BACKEND_PACMAN
    bool warningAur {false};
    // pacman -Qtdq is fast on its own, but running it automatically as part of
    // the very first package-list display adds one more synchronous subprocess
    // round-trip to boot. Skip just that first automatic check; later ones
    // (after an install/uninstall or manual refresh) run normally.
    bool skipInitialAutoremovableCheck {true};
    // installedPackages ("pacman -Qi") and the Enabled Repos list ("pacman -Ss")
    // are two independent background fetches kicked off from the constructor with
    // no join between them (see MainWindow::MainWindow). If the Enabled Repos view
    // renders (displayPackages()) before installedPackages is ready,
    // createPackageDataList()'s merge of installed-but-not-in-repo packages
    // silently finds nothing to add. Set when that happens; the installedPackages
    // fetch's own completion checks this and re-triggers the view once the data
    // it was missing has actually arrived.
    bool enabledReposRenderedWithoutInstalled {false};
    // Reacts to enabledReposRenderedWithoutInstalled once installedPackages actually
    // arrives (see the constructor's installedPackages fetch completion, the only
    // caller). Pulled out into its own method purely so the interaction with
    // dirtyEnabledRepos/handleEnabledReposTab() is independently testable (see
    // Testing/test_mainwindow.cpp) without waiting on a real "pacman -Qi" fetch.
    void handleInstalledPackagesArrived();
#endif
    int savedComboIndex {0};
    // Bumped every time a new Flatpak load (the constructor's startup preload, or a
    // synchronous displayFlatpaks(true) from comboRemote_activated()/
    // comboUser_currentIndexChanged()/etc.) starts. Background completions capture
    // the value current at their own start and compare it before publishing results,
    // so a slow, now-superseded preload can't overwrite a newer selection's data
    // (see setupFlatpakDisplay() and the constructor's Flatpak preload chain).
    int flatpakRequestGeneration {0};
    // True if a background Flatpak preload/fetch stage that captured `myGeneration`
    // at its own start has since been superseded by a newer synchronous load. Pulled
    // out of the constructor's completion lambdas into its own method purely so it's
    // independently testable (see Testing/test_mainwindow.cpp) without needing to
    // drive a real, timing-dependent background race.
    [[nodiscard]] bool isFlatpakGenerationStale(int myGeneration) const
    {
        return myGeneration != flatpakRequestGeneration;
    }

    Cmd cmd;
    LockFile lockFile;
    // Every QtConcurrent::run() worker the constructor and downloadPackageList() kick
    // off (installedPackages, the Flatpak remote/catalog preload chain, the pacman -Ss
    // repo fetch) captures its own copy of these two by value -- never `this->...` --
    // specifically so it stays valid even if `this` doesn't. A worker locks the mutex
    // and checks the flag immediately before its one and only touch of `this`
    // (QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)); ~MainWindow() locks
    // the same mutex and sets the flag first thing, before anything else runs. That
    // ordering makes the two mutually exclusive: either the destructor's lock acquire
    // happens first and every worker that checks afterward sees the flag set and skips
    // touching `this` entirely, or a worker's lock acquire happens first, and the
    // destructor can't proceed until that worker's (still-valid, since `this` can't
    // have started destructing yet) invokeMethod call has already returned. Plain
    // reliance on Qt's "queued calls to a destroyed receiver are dropped" guarantee
    // isn't enough here, since that only covers a call *already delivered* to the
    // queue before destruction -- not one still racing to be issued.
    std::shared_ptr<QMutex> destructionGuardMutex {std::make_shared<QMutex>()};
    std::shared_ptr<bool> destructing {std::make_shared<bool>(false)};
#ifdef MXPI_TESTING
    // See the MXPI_TESTING constructor overload's declaration.
    bool testSkipFlatpakPreload {false};
#endif
    QHash<QString, VersionNumber> listInstalledVersions();
    QIcon qiconInstalled;
    QIcon qiconUpgradable;
    QList<PopularInfo> popularApps;
    QLocale locale;
    QHash<QString, PackageInfo> backportsList;
    QHash<QString, PackageInfo> enabledList;
    QHash<QString, PackageInfo> installedPackages;
    QHash<QString, PackageInfo> mxList;
    QProgressBar *bar {};
    QProgressDialog *progress {};
    QPushButton *pushCancel {};
    QSettings dictionary;
    QSettings settings;
    QString arch;
    QString fpUser;
    uchar debianVersion {Release::Bookworm};
    QString verName;
    QStringList changeList;
    QStringList flatpaks;
    QStringList flatpaksApps;
    QStringList flatpaksRuntimes;
    QStringList installedAppsFP;
    QStringList installedRuntimesFP;
    QStringList cachedInstalledFlatpaks; // Raw lines from flatpak list --columns=ref,size
    QString cachedInstalledScope;
    QHash<QString, QString> cachedInstalledSizeMap; // canonical ref -> size string
    mutable QStringList cachedFlatpakRemotes;
    mutable QString cachedFlatpakRemotesScope;
    mutable bool cachedFlatpakRemotesFetched {false};
    bool cachedInstalledFetched {false};
    bool holdProgressForAptRefresh {false};
    bool holdProgressForFlatpakRefresh {false};
    bool downloadCancelRequested {false};
    bool operationCanceled {false};
    bool flatpakCancelHidden {false};
    bool flatpakUiBlocked {false};
    bool suppressCmdOutput {false};
#ifdef PACKAGE_BACKEND_PACMAN
    // AUR installs run paru unprivileged (see install()), which elevates the actual
    // pacman step itself via sudo -- unlike apt-get/plain pacman, which always go
    // through the privileged helper and pkexec's own native password dialog. That
    // sudo prompt surfaces as plain text in this app's own Output tab, read back via
    // ui->lineEdit, so it needs its own masking rather than relying on pkexec.
    bool lineEditMasked {false};
    QAction *lineEditToggleMaskAction {nullptr};
    QAction *lineEditClearAction {nullptr};
    void setLineEditMasked(bool masked);
    // Bounded carry-over from the previous outputAvailable() chunk, so the sudo-prompt
    // regex can match text straddling a readyReadStandardOutput chunk boundary instead
    // of only ever seeing one chunk in isolation.
    QString sudoPromptTail;
#endif
    QTemporaryDir tempDir;
    QTimer timer;
    QTreeView *currentTree {}; // current/calling tree
    QModelIndex lastIndexClicked {};
    QUrl getScreenshotUrl(const QString &name);
    const QCommandLineParser &args;
    const QString tempList {QStringLiteral("/etc/apt/sources.list.d/mxpitemp.list")};

    // Models
    PackageModel *enabledModel {nullptr};
    PackageModel *mxtestModel {nullptr};
    PackageModel *backportsModel {nullptr};
    FlatpakModel *flatpakModel {nullptr};
    SnapModel *snapModel {nullptr};
    PopularModel *popularModel {nullptr};
#ifdef PACKAGE_BACKEND_PACMAN
    PackageModel *aurModel {nullptr};
#endif

    // Filter proxies
    PackageFilterProxy *enabledProxy {nullptr};
    PackageFilterProxy *mxtestProxy {nullptr};
    PackageFilterProxy *backportsProxy {nullptr};
    FlatpakFilterProxy *flatpakProxy {nullptr};
    SnapFilterProxy *snapProxy {nullptr};
    PopularFilterProxy *popularProxy {nullptr};
#ifdef PACKAGE_BACKEND_PACMAN
    PackageFilterProxy *aurProxy {nullptr};
    CheckableHeaderView *headerAUR {nullptr};
    QHash<QString, PackageInfo> aurList;
    QString cachedParuPath;
    bool cachedParuPathFetched {false};

    [[nodiscard]] QString getParuPath();
    [[nodiscard]] bool buildAurList(const QString &searchTerm);
    void handleAurTab(const QString &searchStr);
    void showAurPackageInfo(const QString &packageName);
#endif

    QNetworkAccessManager manager;
    // Store/appstream metadata shown by the Flatpak and Snap "More info" dialogs.
    struct AppInfo {
        QString title;
        QString summary;
        QString descriptionHtml;
        QString developer;
        QString license;
        QString homepage;
        QString screenshotHtml;
    };
    [[nodiscard]] QByteArray fetchUrl(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers = {});
    [[nodiscard]] QString imageHtml(const QUrl &url, QSize size = QSize(400, 300),
                                    Qt::AspectRatioMode mode = Qt::KeepAspectRatio);
    [[nodiscard]] QString appInfoHtml(const AppInfo &app);
    void showInfoBox(const QString &text, bool richText);
    QNetworkReply *activeDownloadReply {nullptr};

    [[nodiscard]] QHash<QString, PackageInfo> listInstalled();
    [[nodiscard]] QString categoryTranslation(const QString &item);
    [[nodiscard]] QString getMXTestRepoUrl();
    [[nodiscard]] QString getArchOption() const;
    [[nodiscard]] QString getLocalizedName(const QDomElement &element) const;
    [[nodiscard]] QString mapArchToFormat(const QString &arch) const;
    [[nodiscard]] QStringList listFlatpaks(const QString &remote, const QString &scope, const QString &type = QString()) const;
    [[nodiscard]] FlatpakData createFlatpakData(const QString &item, const QStringList &installedAll) const;
    [[nodiscard]] PackageData createPackageData(const QString &name, const QString &version,
                                                const QString &description) const;
    [[nodiscard]] bool checkInstalled(const QVariant &names) const;
    [[nodiscard]] bool checkUpgradable(const QStringList &nameList) const;
    [[nodiscard]] bool isOnline();
    [[nodiscard]] bool isPackageInstallable(const QString &installable, const QString &modArch) const;
    [[nodiscard]] static QString getDebianVerName(uchar version = 0);
    [[nodiscard]] static uchar getDebianVerNum();
    [[nodiscard]] static uchar showVersionDialog(const QString &message);
    [[nodiscard]] QVector<PackageData> createPackageDataList(QHash<QString, PackageInfo> *list) const;

    // Per-APT-tab context to replace repetitive tree-identity switches
    struct AptTabContext {
        PackageModel *model = nullptr;
        PackageFilterProxy *proxy = nullptr;
        QHash<QString, PackageInfo> *list = nullptr;
        QComboBox *filterCombo = nullptr;
        CheckableHeaderView *header = nullptr;
    };
    // True once either a confirmation dialog was canceled or an elevated call was
    // dismissed anywhere during the current operation. Callers use this to abort
    // and revert to the calling tab quietly instead of reporting a generic error.
    [[nodiscard]] bool operationWasCanceled() const { return operationCanceled || Cmd::elevationDismissed(); }
    void beginOperation()
    {
        operationCanceled = false;
        Cmd::resetElevationDismissed();
    }
    [[nodiscard]] AptTabContext aptTab(const QTreeView *tree);
    [[nodiscard]] AptTabContext currentAptTab();
    // Package trees that have an AptTabContext, for code that handles all of them.
    [[nodiscard]] QList<QTreeView *> aptTrees() const;
    // Whether the current package tab's own filter combo is set to "Autoremovable",
    // i.e. whether Install acts as "Mark keep" there.
    [[nodiscard]] bool currentTabFiltersAutoremovable();
    [[nodiscard]] QHash<QString, PackageInfo> *getCurrentList();
    [[nodiscard]] PackageModel *getCurrentModel();
    [[nodiscard]] PackageFilterProxy *getCurrentProxy();

    bool buildPackageLists(bool forceDownload = false);
    [[nodiscard]] bool confirmActions(const QString &names, const QString &action);

    [[nodiscard]] bool downloadAndUnzip(const QString &url, QFile &file);
    [[nodiscard]] bool downloadAndUnzip(const QString &url, const QString &repoName, const QString &branch,
                                        const QString &format, QFile &file);
    [[nodiscard]] bool downloadFile(const QString &url, QFile &file);
    [[nodiscard]] bool downloadPackageList(bool forceDownload = false);
    bool install(const QString &names);
    [[nodiscard]] bool installBatch(const QStringList &nameList);
    [[nodiscard]] bool installPopularApp(const QString &name);
    [[nodiscard]] bool installPopularApps();
    [[nodiscard]] bool installSelected();
    [[nodiscard]] bool markKeep();
    [[nodiscard]] bool readPackageList(bool forceDownload = false);
    [[nodiscard]] bool uninstall(const QString &names, const QStringList &preUninstall = {},
                                 const QStringList &postUninstall = {});
    bool updateApt();
    [[nodiscard]] static QString convert(quint64 bytes);
    [[nodiscard]] static quint64 convert(const QString &size, bool *ok = nullptr);
    void blockInterfaceFP();
    void buildChangeList(const QString &packageName, Qt::CheckState state);
    void buildFlatpakChangeList(const QString &fullName, Qt::CheckState state, int status);
    void cancelDownload();
    void centerWindow();
    void clearUi();
    void displayAutoremovable(const QTreeView *tree);
    void ensureAutoremovableShown(const QTreeView *tree);
#ifndef PACKAGE_BACKEND_PACMAN
    void applyPreloadedEnabledList(QHash<QString, PackageInfo> loadedList);
#endif
    void updateAutoremovable(PackageModel *model);
    void displayFilteredFP(QStringList list, bool raw = false);
    void displayFlatpaks(bool forceUpdate = false);
    void displayPackages();
    void displayPopularApps();
    void displayWarning(const QString &repo);
    void enableTabs(bool enable);
    void forceUpdateAptTab(QLineEdit *searchBox, QComboBox *filterCombo);
    void applyHideLibs(bool checked, QTreeView *tree, PackageFilterProxy *proxy, QComboBox *filterCombo,
                       const QList<QCheckBox *> &peerCheckboxes);
    void finalizeFlatpakDisplay();
    void formatFlatpakTree();
    void removeDuplicatesFP();
    void applyPopularCategorySpanning();
    void handleEnabledReposTab(const QString &searchStr);
    void handleFlatpakTab(const QString &searchStr);
    void handleSnapTab(const QString &searchStr);
    void handleOutputTab();
    void handleTab(const QString &searchStr, QLineEdit *searchBox, const QString &warningMessage, bool dirtyFlag);
    void resizeCurrentColumns();
    [[nodiscard]] bool shouldRefreshFilters(const QString &searchStr);
    void hideColumns();
    void ifDownloadFailed() const;
    void installFlatpak();
    void showError(const QString &message, const QString &details = {});
    void displaySnaps(bool forceUpdate = false);
    void loadSnapData();
    void populateSnapTree();
    void updateSnapCounts();
    void setupSnapd();
    void buildSnapChangeList(const QString &name, Qt::CheckState state, int status);
    [[nodiscard]] QStringList listInstalledSnaps() const;
    [[nodiscard]] QVector<SnapData> parseSnapList(const QString &output, bool installed) const;
    [[nodiscard]] static bool isSystemdInit();
    [[nodiscard]] bool isSnapdReady() const;
    void invalidateFlatpakRemoteCache();
    void listFlatpakRemotes() const;
    void listSizeInstalledFP();
    void loadFlatpakData();
    void processFlatpakData(const QStringList &remoteEntries, const QStringList &allInstalled, const QString &scope);
    void loadPmFiles();
    void populateFlatpakTree();
    void processDoc(const QDomDocument &doc);
    void refreshPopularApps();
    void resetCheckboxes();
    void saveSearchText(QString &searchStr, int &filterIdx);
    void setConnections();
    void setCurrentTree();
    void setDirty();
    void rebuildPackageViews();
    void setIcons();
    void setProgressDialog();
    void setSearchFocus() const;
    void setup();
    void setupFlatpakDisplay();
    void showFlatpakProgress(const QString &label);
    void setupModels();
    void updateFlatpakCounts(uint totalCount);
    void updateInterface();
    void updatePackageStatuses();
    // Header checkbox helpers
    CheckableHeaderView *headerEnabled {nullptr};
    CheckableHeaderView *headerMX {nullptr};
    CheckableHeaderView *headerBP {nullptr};
};
