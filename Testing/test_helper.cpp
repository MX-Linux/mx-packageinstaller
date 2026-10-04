#include <QDir>
#include <QFile>
#include <QProcess>
#include <QtTest>

// Drives the actual, fully-linked `helper` binary (see src/helper.cpp) as a
// subprocess, exactly the way src/cmd.cpp's Cmd::startAndWait()/helperProc()
// invoke it -- same argv shape, same "--marker <path> [--cancel-on-eof]
// <action> ..." contract -- but run directly as the unprivileged user running
// ctest, never through pkexec. That's a faithful test of the helper's
// externally-observable argv/exit-code contract for paths that don't
// inherently require root: allow-list rejection and the marker-creation
// mechanism used to distinguish a dismissed polkit prompt from a genuine
// command failure (see Cmd::isAuthenticationDismissed()/classifyAuthDismissed()
// in src/cmd.cpp, covered from the caller side in test_cmd.cpp).
class HelperTest : public QObject
{
    Q_OBJECT

private slots:
    void markerIsCreatedAndCommandSucceeds();
    void invalidMarkerPathAbortsBeforeRunningAnything();
    void disallowedCommandIsRejected();
    void cLocaleOverrideIsAccepted();
    void otherLocaleOverrideIsRejected();
    void unknownActionIsRejected();
};

namespace
{
QString helperBinaryPath()
{
    return QStringLiteral(HELPER_BINARY_PATH);
}
} // namespace

void HelperTest::markerIsCreatedAndCommandSucceeds()
{
    const QString markerPath = QDir::tempPath() + QStringLiteral("/mx-pkg-helper-binarytest-ok.marker");
    QFile::remove(markerPath);

    // Same argv order Cmd::startAndWait()/helperProc() actually construct:
    // --marker <path> --cancel-on-eof <action> ...
    QProcess process;
    process.start(helperBinaryPath(), {"--marker", markerPath, "--cancel-on-eof", "exec", "ps", "-e", "--no-headers"});
    QVERIFY(process.waitForStarted());
    // Deliberately leave our write channel open: with --cancel-on-eof, closing
    // it is what tells the helper to terminate its child's process group (see
    // Cmd::terminateAndKill()), which is not what we want while merely waiting
    // for a normal, successful completion.
    QVERIFY(process.waitForFinished(10000));

    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    // The marker proves the helper actually started running (this is exactly
    // what lets the caller distinguish "polkit dismissed" from "command
    // failed" when pkexec itself reports exit code 126/127) -- and unlike
    // Cmd::startAndWait(), which deletes it once the caller has inspected it,
    // the helper itself never removes it.
    QVERIFY(QFile::exists(markerPath));

    QFile::remove(markerPath);
}

void HelperTest::invalidMarkerPathAbortsBeforeRunningAnything()
{
    // Not inside the temp dir or /run/user/<uid>: isValidMarkerPath() must
    // reject this, and createMarker() failing must abort main() with exit 1
    // before ever dispatching to the "exec ps" action below.
    const QString markerPath = QStringLiteral("/mx-pkg-helper-binarytest-should-not-exist.marker");
    QFile::remove(markerPath);

    QProcess process;
    process.start(helperBinaryPath(), {"--marker", markerPath, "exec", "ps"});
    QVERIFY(process.waitForStarted());
    // No child ever gets spawned in this path (createMarker() fails first),
    // so stdin handling is irrelevant here.
    QVERIFY(process.waitForFinished(10000));

    QCOMPARE(process.exitCode(), 1);
    QVERIFY(!QFile::exists(markerPath));
}

void HelperTest::disallowedCommandIsRejected()
{
    QProcess process;
    process.start(helperBinaryPath(), {"--cancel-on-eof", "exec", "rm", "-rf", "/nonexistent"});
    QVERIFY(process.waitForStarted());
    // The allow-list check rejects "rm" before any child ever gets spawned,
    // so stdin handling is irrelevant here.
    QVERIFY(process.waitForFinished(10000));

    QCOMPARE(process.exitCode(), 127);
    QVERIFY(QString::fromUtf8(process.readAllStandardError()).contains(QStringLiteral("not allowed")));
}

// LockFile's apt frontend-lock probe runs "apt-get check" with LANG=C and LC_ALL=C
// so it can parse apt's message. The helper used to reject both variables, which
// failed that probe before apt-get ever ran.
void HelperTest::cLocaleOverrideIsAccepted()
{
    QProcess process;
    process.start(helperBinaryPath(), {"--cancel-on-eof", "exec", "--env", "LANG=C", "--env", "LC_ALL=C", "ps",
                                       "-e", "--no-headers"});
    QVERIFY(process.waitForStarted());
    // Write channel left open on purpose: see markerIsCreatedAndCommandSucceeds().
    QVERIFY(process.waitForFinished(10000));

    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
}

// The locale variables are only allowed with the value the probe needs; anything
// else (another locale name, or a path to locale data) is rejected before a child
// is spawned.
void HelperTest::otherLocaleOverrideIsRejected()
{
    QProcess process;
    process.start(helperBinaryPath(), {"--cancel-on-eof", "exec", "--env", "LC_ALL=/tmp/locale", "ps"});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(10000));

    QCOMPARE(process.exitCode(), 1);
    QVERIFY(QString::fromUtf8(process.readAllStandardError()).contains(QStringLiteral("not allowed")));
}

void HelperTest::unknownActionIsRejected()
{
    QProcess process;
    process.start(helperBinaryPath(), {"--cancel-on-eof", "not-a-real-action"});
    QVERIFY(process.waitForStarted());
    // main() rejects the unrecognized action before any child ever gets
    // spawned, so stdin handling is irrelevant here.
    QVERIFY(process.waitForFinished(10000));

    QCOMPARE(process.exitCode(), 1);
    QVERIFY(QString::fromUtf8(process.readAllStandardError()).contains(QStringLiteral("Unsupported helper action")));
}

QTEST_GUILESS_MAIN(HelperTest)

#include "test_helper.moc"
