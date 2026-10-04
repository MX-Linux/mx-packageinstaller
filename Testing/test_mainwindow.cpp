// Integration tests for MainWindow's asynchronous orchestration -- the only test
// target in this suite that actually instantiates MainWindow. Needs a QApplication
// (it's a QDialog) and, since it launches real QtConcurrent::run() background work
// and shells out to real subprocesses (apt-cache/dpkg/pacman, whichever the build's
// PACKAGE_BACKEND selects), a headless platform plugin -- see main() below. Every
// MainWindow constructed here passes testSkipFlatpakPreload=true (a test-only
// constructor parameter, never a production command-line option -- see
// mainwindow.h/.cpp) so none of that background work ever reaches real flatpak
// remotes -- one of its fallback paths can genuinely touch the network, which a
// test run must not do.
//
// This is a whitebox test: MainWindow exposes no public API at all (see
// mainwindow.h), so exercising internal orchestration state directly needs a
// testability seam. mainwindow.h grants this file friend access, gated behind
// MXPI_TESTING (defined only by this test target, never by the production build --
// see Testing/CMakeLists.txt), plus two small, behavior-preserving extractions
// (MainWindow::isFlatpakGenerationStale() and, pacman-only,
// MainWindow::handleInstalledPackagesArrived()) pulled out of inline lambdas
// specifically so they're callable/inspectable here without racing real
// background-thread timing.
#include <QtTest>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalBlocker>
#include <QStandardPaths>

#include <atomic>
#include <memory>

#include "../src/mainwindow.h"
#include "ui_mainwindow.h"

#ifdef PACKAGE_BACKEND_PACMAN
    #include "../src/packageinfo.h"
#endif

namespace {
// Matches main.cpp's own option registration; --skip-online-check makes
// MainWindow::isOnline() short-circuit instead of making a real network call, so
// any code path this file exercises that happens to touch it stays hermetic.
void setupTestParser(QCommandLineParser &parser)
{
    parser.addOption({{"s", "skip-online-check"}, QStringLiteral("Skip online check.")});
    parser.process(QStringList {QStringLiteral("test_mainwindow"), QStringLiteral("--skip-online-check")});
}

#ifdef PACKAGE_BACKEND_PACMAN
// Mirrors pacmanAvailablePackages()'s own invocation. If this fails (e.g. no sync
// repositories configured at all), MainWindow's constructor/downloadPackageList()
// hits its failure path and pops a QMessageBox::critical -- a QDialog::exec() call
// with nothing to click in a headless offscreen test run, hanging forever.
bool pacmanHasUsableRepos()
{
    QProcess process;
    process.start(QStringLiteral("/bin/bash"), {"-c", QStringLiteral("LANG=C pacman -Ss --color never")});
    if (!process.waitForFinished(10000)) {
        process.kill();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
#endif
} // namespace

class TestMainWindowAsync : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void testDestructionDuringBackgroundWork();
    void testFlatpakGenerationStaleness();
    void testMarkKeepFollowsCurrentTabFilter();
#ifdef PACKAGE_BACKEND_PACMAN
    void testEnabledReposRenderedWithoutInstalledFlagFlow();
#endif
};

void TestMainWindowAsync::initTestCase()
{
    // Belt-and-suspenders: also enforced by CTest's ENVIRONMENT property (see
    // Testing/CMakeLists.txt) and by main() below when run standalone.
    QVERIFY2(QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM")).contains(QLatin1String("offscreen")),
             "This test must run with QT_QPA_PLATFORM=offscreen");
#ifdef PACKAGE_BACKEND_PACMAN
    // Every test in this file constructs a real MainWindow. On this backend its
    // constructor's setup() lands on the Enabled Repos tab, which synchronously
    // cascades into downloadPackageList() -- if pacman has no usable sync
    // repositories configured at all, that fails and pops a QMessageBox::critical
    // (a QDialog::exec() call with nothing to click in a headless test run),
    // hanging forever rather than failing cleanly. QSKIP here skips every test in
    // this class (documented QtTest behavior for initTestCase()), rather than
    // guarding each test function individually.
    if (!pacmanHasUsableRepos()) {
        QSKIP("pacman has no usable sync repositories configured in this environment; "
              "constructing MainWindow would hit downloadPackageList()'s failure path, "
              "which pops a blocking QMessageBox::critical with nothing to dismiss it in "
              "a headless test run.");
    }
#endif
}

// See commit f69eb50d ("Make QtConcurrent background workers safe against window
// destruction"): five QtConcurrent::run() futures launched from the constructor and
// downloadPackageList() capture a shared_ptr<QMutex>/shared_ptr<bool> "destructing"
// pair by value and check them immediately before their one and only touch of
// `this`; ~MainWindow() locks the same mutex and sets the flag first, before
// anything else.
//
// This is deterministic, not a timing-dependent stress test: the MXPI_TESTING-only
// BackgroundWorkerTestHooks (see mainwindow.h/.cpp) let the constructor's first
// background preload worker (AptCache's, apt-only, or installedPackages',
// pacman-only -- whichever this build targets) signal that it has finished its
// real work and is one step from touching the guard (beforeGuardCheck), then
// block there until released. That gives this test full control over the
// interleaving: wait for the signal (so "destroy while the worker is in flight"
// is a guaranteed fact, not a matter of timing luck), destroy the window, then
// release the worker. A correct implementation locks the guard, sees
// destructing == true, and returns; afterGuardPassed only fires on the opposite,
// buggy branch (the one about to touch the now-dangling `this` via
// QMetaObject::invokeMethod()), so asserting it never fires is a direct, positive
// check that the guard actually suppressed the touch -- not just "nothing crashed
// this time," which a silent use-after-free could pass even in a broken build.
// workerFinished (via a scope guard covering both branches) gives the test a real
// completion signal to wait for before checking afterGuardPassed, rather than
// assuming the worker resumed and finished within some fixed time budget after
// being released -- a descheduled/slow-scheduled worker could otherwise miss a
// short window and let a broken guard pass unnoticed.
//
// All shared state is heap-allocated (a shared_ptr) rather than captured by
// reference from this function's stack: the worker's own copy of the hooks (via
// BackgroundWorkerTestHooks, captured by value into its lambda, same as
// guardMutex/destructingFlag already are) keeps it alive independently of
// whatever this function does or how it returns, including on the assertion
// failure path below -- QVERIFY2 returns immediately on failure, so a
// qScopeGuard is what guarantees the window still gets destroyed and the worker
// still gets released (rather than blocking forever on a semaphore) even then.
void TestMainWindowAsync::testDestructionDuringBackgroundWork()
{
    struct SharedState
    {
        QSemaphore workerReachedCheckpoint;
        QSemaphore releaseWorker;
        QSemaphore workerFinished;
        std::atomic<bool> guardWasBypassed {false};
    };
    auto state = std::make_shared<SharedState>();

    QCommandLineParser parser;
    setupTestParser(parser);
    auto *window = new MainWindow(
        parser, nullptr,
        MainWindow::BackgroundWorkerTestHooks {
            /*beforeGuardCheck=*/[state] {
                state->workerReachedCheckpoint.release();
                state->releaseWorker.acquire();
            },
            /*afterGuardPassed=*/[state] { state->guardWasBypassed = true; },
            /*workerFinished=*/[state] { state->workerFinished.release(); },
        },
        /*testSkipFlatpakPreload=*/true);

    // No matter how this function exits from here on -- including either
    // assertion below failing -- the window must be destroyed and the worker
    // released, so a worker that reaches the checkpoint late can never block
    // forever on a semaphore this function already walked away from.
    auto cleanup = qScopeGuard([&] {
        delete window;
        state->releaseWorker.release();
    });

    QVERIFY2(state->workerReachedCheckpoint.tryAcquire(1, 10000),
             "background preload worker never reached the test checkpoint");

    cleanup.dismiss();
    delete window; // must set destructing=true without waiting for the blocked worker
    state->releaseWorker.release();

    // Wait for the worker's own real completion signal -- fired via a scope
    // guard covering both branches of the guard check, so this proves the
    // worker actually finished running rather than assuming it did within some
    // fixed time budget (which a descheduled/slow-scheduled worker could miss,
    // letting a broken guard pass unnoticed).
    QVERIFY2(state->workerFinished.tryAcquire(1, 10000),
             "background preload worker never signaled completion after being released");
    QVERIFY2(!state->guardWasBypassed,
             "worker touched the guarded completion path after MainWindow was destroyed -- "
             "the destruction guard did not hold");
}

// See flatpakRequestGeneration's declaration in mainwindow.h and its use in the
// constructor's Flatpak preload chain / displayFlatpaks(): a monotonic counter,
// bumped by every synchronous Flatpak (re)load, that background preload
// completions compare against their own captured value so a slow, now-superseded
// fetch can't clobber a newer result.
//
// Driving the real end-to-end race (a slow background preload completion actually
// arriving after a newer synchronous reload) deterministically would mean
// controlling real subprocess-completion timing, which isn't practical here --
// see testDestructionDuringBackgroundWork() above for why that's already
// accepted as inherently racy elsewhere in this file. Instead, this calls the
// exact comparison production code uses (isFlatpakGenerationStale(), extracted
// from the inline check for exactly this reason) around a real synchronous
// reload, which is deterministic and exercises the real logic rather than a
// re-implementation of it.
void TestMainWindowAsync::testFlatpakGenerationStaleness()
{
    QCommandLineParser parser;
    setupTestParser(parser);
    MainWindow window(parser, nullptr, {}, /*testSkipFlatpakPreload=*/true);

    // testSkipFlatpakPreload keeps the constructor's own Flatpak preload chain
    // from running at all, so the generation starts at its initial value here --
    // captured rather than assumed, since that's an implementation detail this
    // test shouldn't hardcode.
    const int generationBeforeReload = window.flatpakRequestGeneration;
    QVERIFY(!window.isFlatpakGenerationStale(generationBeforeReload));

    // A synchronous reload -- the same kind comboRemote_activated()/
    // comboUser_currentIndexChanged() trigger -- always bumps the generation before
    // doing anything else (see displayFlatpaks()).
    window.displayFlatpaks(true);
    const int generationAfterReload = window.flatpakRequestGeneration;

    QVERIFY(generationAfterReload > generationBeforeReload);
    // A completion that captured the OLD generation (e.g. the constructor's own
    // startup preload, if it were still in flight) is now stale and must not
    // publish over the newer, synchronous result above.
    QVERIFY(window.isFlatpakGenerationStale(generationBeforeReload));
    // ...but one that captured the current generation is not.
    QVERIFY(!window.isFlatpakGenerationStale(generationAfterReload));
}

// Install turns into "Mark keep" only when the *current* package tab filters on
// Autoremovable. Each tab has its own filter combo, and the combos are synced only
// on a tab switch, so a stale Enabled Repos combo must not decide for MX Test.
// pushInstall_clicked() picks markKeep() vs installSelected() with the same
// predicate that sets this label.
void TestMainWindowAsync::testMarkKeepFollowsCurrentTabFilter()
{
    QCommandLineParser parser;
    setupTestParser(parser);
    MainWindow window(parser, nullptr, {}, /*testSkipFlatpakPreload=*/true);

    // Signals blocked: this sets the combos' state without running filterChanged().
    const auto setFilter = [](QComboBox *combo, const QString &text) {
        const QSignalBlocker blocker(combo);
        combo->setCurrentText(text);
        QCOMPARE(combo->currentText(), text);
    };
    const QString package = QStringLiteral("mxpi-test-package");
    window.currentTree = window.ui->treeMXtest;
    window.changeList.clear();

    setFilter(window.ui->comboFilterEnabled, MainWindow::tr("Autoremovable"));
    setFilter(window.ui->comboFilterMX, MainWindow::tr("Not installed"));
    window.buildChangeList(package, Qt::Checked);
    QVERIFY(window.ui->pushInstall->text() != MainWindow::tr("Mark keep"));
    window.buildChangeList(package, Qt::Unchecked);

    setFilter(window.ui->comboFilterEnabled, MainWindow::tr("All packages"));
    setFilter(window.ui->comboFilterMX, MainWindow::tr("Autoremovable"));
    window.buildChangeList(package, Qt::Checked);
    QCOMPARE(window.ui->pushInstall->text(), MainWindow::tr("Mark keep"));
}

#ifdef PACKAGE_BACKEND_PACMAN
// See enabledReposRenderedWithoutInstalled's declaration in mainwindow.h (pacman
// only): installedPackages ("pacman -Qi") and the Enabled Repos list ("pacman -Ss")
// are two independent background fetches with no join between them. If the Enabled
// Repos view renders before installedPackages is ready, createPackageDataList()'s
// merge of installed-but-not-in-repo packages silently finds nothing to add;
// displayPackages() sets this flag when that happens, and
// handleInstalledPackagesArrived() (called once installedPackages actually lands)
// reacts to it by forcing dirtyEnabledRepos so the next render redoes the merge.
//
// The real end-to-end trigger for the "still on that tab" branch cascades into
// handleEnabledReposTab() -> buildPackageLists() -> a real "pacman -Ss" -- not
// safely drivable here: this sandbox has no configured pacman repositories at all
// ("pacman -Ss" here returns "error: no usable package repositories configured"),
// and if that ever surfaces as a failure, downloadPackageList() pops a
// QMessageBox::critical, which would hang a headless test run waiting for a click
// that never comes. So this exercises the flag state machine itself directly --
// the actual mechanism this whole fix is about -- without following it all the way
// through a real repo re-fetch.
void TestMainWindowAsync::testEnabledReposRenderedWithoutInstalledFlagFlow()
{
    QCommandLineParser parser;
    setupTestParser(parser);
    MainWindow window(parser, nullptr, {}, /*testSkipFlatpakPreload=*/true);

    // 1) Rendering the Enabled Repos tab while installedPackages is still empty
    //    (as it plausibly would be while the startup preload is in flight) must
    //    remember that it rendered without that data.
    window.installedPackages.clear();
    window.currentTree = window.ui->treeEnabled;
    window.enabledReposRenderedWithoutInstalled = false;
    window.displayPackages();
    QVERIFY(window.enabledReposRenderedWithoutInstalled);

    // 2) Once installedPackages actually arrives, handleInstalledPackagesArrived()
    //    must clear that flag and mark the view dirty -- regardless of which tab is
    //    current when it fires. Keep currentTree off the Enabled Repos tab here so
    //    this doesn't cascade into handleEnabledReposTab()/a real "pacman -Ss" (see
    //    the function comment above); that branch is a plain, untested `if` guard,
    //    not exercised by this assertion.
    window.currentTree = nullptr;
    window.dirtyEnabledRepos = false;
    window.installedPackages.insert(QStringLiteral("some-pkg"), PackageInfo {});
    window.handleInstalledPackagesArrived();
    QVERIFY(!window.enabledReposRenderedWithoutInstalled);
    QVERIFY(window.dirtyEnabledRepos);

    // 3) A no-op call (flag already clear) must leave dirtyEnabledRepos alone.
    window.dirtyEnabledRepos = false;
    window.handleInstalledPackagesArrived();
    QVERIFY(!window.dirtyEnabledRepos);
}
#endif

int main(int argc, char *argv[])
{
    // Headless: no X11/Wayland display is assumed to be available wherever this
    // suite runs. CTest sets this too (see Testing/CMakeLists.txt's ENVIRONMENT
    // property), but set it here as well so the binary is also safe to run
    // directly, not just via ctest.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    // Distinct from the real app's identity, and QStandardPaths::setTestModeEnabled()
    // redirects generic config/data locations to a temporary directory -- between
    // the two, MainWindow's QSettings members can't touch the real user config.
    QApplication::setOrganizationName(QStringLiteral("MX-Linux-Tests"));
    QApplication::setApplicationName(QStringLiteral("mx-packageinstaller-test-mainwindow"));
    QStandardPaths::setTestModeEnabled(true);

    TestMainWindowAsync tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_mainwindow.moc"
