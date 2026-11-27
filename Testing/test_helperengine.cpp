#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cerrno>
#include <csignal>
#include <cstdio>

#include <unistd.h>

#include "src/helperengine.h"

using namespace HelperEngine;

// Exercises the process-spawning/stdin/marker scaffolding that src/helper.cpp
// delegates to (see src/helperengine.h). This is the same code that runs as
// root under pkexec in production, but none of it actually requires
// privilege -- only the *caller* (pkexec) is what elevates it -- so it can be
// driven directly and unprivileged here.
class HelperEngineTest : public QObject
{
    Q_OBJECT

private slots:
    void cancelOnStdinEofKillsWholeProcessGroup();
    void noCancelOnStdinEofOnlyClosesWriteChannel();
    void markerCreationRoundTrip();
    void markerCreationRejectsPreexistingPath();
    void markerCreationRejectsSymlink();
    void markerPathValidation();
    void archiveKeepsBoundedTails();
    void archiveRequiresOwnedRegularFile();
};

namespace
{
// Point our own stdin at /dev/null so the QSocketNotifier in runProcess() sees
// an immediate, deterministic EOF regardless of however ctest itself invokes
// this binary (a tty, a pipe, or already-closed stdin).
void forceStdinToImmediateEof()
{
    QVERIFY2(std::freopen("/dev/null", "r", stdin) != nullptr, "failed to redirect stdin to /dev/null");
}

bool pidIsAlive(qint64 pid)
{
    errno = 0;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}
} // namespace

void HelperEngineTest::cancelOnStdinEofKillsWholeProcessGroup()
{
    forceStdinToImmediateEof();

    // The shell backgrounds a grandchild `sleep`, prints its pid, then waits.
    // With cancelOnStdinEof set and stdin already at EOF, runProcess() should
    // terminate the whole process group essentially immediately -- long before
    // the 30-second sleep would finish on its own -- and that must take the
    // grandchild down too, not just the immediate /bin/sh child.
    const ProcessResult result = runProcess(QStringLiteral("/bin/sh"),
                                            {"-c", "sleep 30 & child=$!; echo $child; wait"}, {}, /*relayStdout=*/false,
                                            /*relayStderr=*/false, /*cancelOnStdinEof=*/true);

    QVERIFY(result.started);
    QVERIFY(result.cancelled);
    QCOMPARE(relayResult(result), 143);

    bool validPid = false;
    const qint64 grandchildPid = QString::fromUtf8(result.standardOutput).trimmed().toLongLong(&validPid);
    QVERIFY(validPid);
    QVERIFY(grandchildPid > 0);

    // Give the kernel a moment to actually reap/deliver the signal, then
    // confirm the grandchild is gone -- proof the SIGTERM reached the whole
    // process group (created via setsid()/CreateNewSession), not just the
    // shell we spawned directly.
    QTest::qWait(200);
    QVERIFY(!pidIsAlive(grandchildPid));
}

void HelperEngineTest::noCancelOnStdinEofOnlyClosesWriteChannel()
{
    forceStdinToImmediateEof();

    // `cat` immediately sees EOF on its own stdin once the child's write
    // channel is closed (rather than the process being killed), so it exits
    // on its own and "still-alive" gets printed. If runProcess() instead
    // terminated the process group on stdin EOF, the shell would be killed
    // before ever reaching the echo.
    const ProcessResult result = runProcess(QStringLiteral("/bin/sh"), {"-c", "cat >/dev/null; echo still-alive"}, {},
                                            /*relayStdout=*/false, /*relayStderr=*/false, /*cancelOnStdinEof=*/false);

    QVERIFY(result.started);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(relayResult(result), 0);
    QVERIFY(QString::fromUtf8(result.standardOutput).contains(QStringLiteral("still-alive")));
}

void HelperEngineTest::markerCreationRoundTrip()
{
    // isValidMarkerPath() only accepts the system temp dir or /run/user/<uid>,
    // matching what the helper actually does in its unprivileged (non-
    // /run/user) fallback case -- the same one cmd.cpp's markerDirectory()
    // falls back to when XDG_RUNTIME_DIR is unavailable.
    const QString tempPath = QDir::tempPath() + QStringLiteral("/mx-pkg-helper-roundtrip-test.marker");
    QFile::remove(tempPath);

    QVERIFY(isValidMarkerPath(tempPath));
    QVERIFY(createMarker(tempPath));
    QVERIFY(QFile::exists(tempPath));

    QFile::remove(tempPath);
}

void HelperEngineTest::markerCreationRejectsPreexistingPath()
{
    const QString path = QDir::tempPath() + QStringLiteral("/mx-pkg-helper-preexisting-test.marker");
    QFile::remove(path);

    QVERIFY(createMarker(path));
    // A second creation at the same path must fail (O_EXCL): this is exactly
    // the mechanism that turns a pre-planted marker into a denial of the
    // operation rather than a root-owned write following an attacker's path.
    QVERIFY(!createMarker(path));

    QFile::remove(path);
}

void HelperEngineTest::markerCreationRejectsSymlink()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.path() + QStringLiteral("/target");
    const QString linkPath = QDir::tempPath() + QStringLiteral("/mx-pkg-helper-symlink-test.marker");
    QFile::remove(linkPath);

    QVERIFY(QFile::link(target, linkPath));
    // O_NOFOLLOW (and O_EXCL, since the symlink itself already exists as a
    // directory entry) must reject writing through the symlink.
    QVERIFY(!createMarker(linkPath));
    QVERIFY(!QFile::exists(target));

    QFile::remove(linkPath);
}

void HelperEngineTest::markerPathValidation()
{
    // Wrong name pattern.
    QVERIFY(!isValidMarkerPath(QDir::tempPath() + QStringLiteral("/not-a-marker.txt")));
    QVERIFY(!isValidMarkerPath(QDir::tempPath() + QStringLiteral("/mx-pkg-helper-no-suffix")));

    // Right name, wrong directory.
    QVERIFY(!isValidMarkerPath(QStringLiteral("/etc/mx-pkg-helper-evil.marker")));

    // Right name, temp dir: valid.
    QVERIFY(isValidMarkerPath(QDir::tempPath() + QStringLiteral("/mx-pkg-helper-ok.marker")));

    // Right name, /run/user/<PKEXEC_UID> when PKEXEC_UID is set to digits.
    qputenv("PKEXEC_UID", "4242");
    QVERIFY(isValidMarkerPath(QStringLiteral("/run/user/4242/mx-pkg-helper-ok.marker")));
    // A different uid's runtime dir must not validate.
    QVERIFY(!isValidMarkerPath(QStringLiteral("/run/user/1000/mx-pkg-helper-ok.marker")));
    qunsetenv("PKEXEC_UID");

    // With PKEXEC_UID unset (or non-numeric), the /run/user branch never
    // matches, regardless of what directory is named.
    QVERIFY(!isValidMarkerPath(QStringLiteral("/run/user/4242/mx-pkg-helper-ok.marker")));
    qputenv("PKEXEC_UID", "not-a-number");
    QVERIFY(!isValidMarkerPath(QStringLiteral("/run/user/not-a-number/mx-pkg-helper-ok.marker")));
    qunsetenv("PKEXEC_UID");
}

void HelperEngineTest::archiveKeepsBoundedTails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString src = dir.filePath("session");
    const QString dest = dir.filePath("current");
    constexpr qsizetype mib = 1024 * 1024;
    auto write = [](const QString &path, const QByteArray &contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.flush();
    };
    const QByteArray tail(mib, 's');
    QVERIFY(write(src, "discard" + tail));
    QVERIFY(write(dest, "previous"));
    QVERIFY(write(dest + ".old", QByteArray(5 * mib, 'h')));
    QVERIFY(archiveLog({src}, geteuid(), dest));
    QVERIFY(!QFileInfo::exists(src));
    QFile current(dest);
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), tail);
    QFile history(dest + ".old");
    QVERIFY(history.open(QIODevice::ReadOnly));
    QCOMPARE(history.size(), 5 * mib);
    QVERIFY(history.readAll().endsWith("previous"));
}

void HelperEngineTest::archiveRequiresOwnedRegularFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile source(dir.filePath("session"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("session"), 7);
    source.close();
    const QString link = dir.filePath("link");
    QVERIFY(source.link(link));
    const QString dest = dir.filePath("current");
    QVERIFY(!archiveLog({link}, geteuid(), dest));
    QVERIFY(!archiveLog({source.fileName()}, geteuid() == 0 ? 1 : 0, dest));
    QVERIFY(!QFileInfo::exists(dest));
    // Replacing a legacy destination link also leaves the linked file intact.
    QVERIFY(QFile::link(source.fileName(), dest));
    QVERIFY(archiveLog({source.fileName()}, geteuid(), dest));
    QVERIFY(!QFileInfo(dest).isSymLink());
}

QTEST_GUILESS_MAIN(HelperEngineTest)

#include "test_helperengine.moc"
