#include "cmd.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QUuid>

#include <csignal>
#include <memory>
#include <vector>

#include <unistd.h>

namespace
{
// Overlays environment on top of the current process's environment (as opposed
// to replacing it outright) so callers only need to name the handful of
// variables (LANG, DEBIAN_FRONTEND, ...) they actually care about overriding.
QProcessEnvironment environmentWithOverrides(const QHash<QString, QString> &overrides)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        env.insert(it.key(), it.value());
    }
    return env;
}

// Every command gets its own session (and therefore process group), matching
// startAndWait() below: package managers frequently spawn helpers, and this
// lets a whole subtree be cancelled together rather than just its launcher.
void makeNewSession(QProcess &process)
{
#ifdef Q_OS_UNIX
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#else
    process.setChildProcessModifier([] { ::setsid(); });
#endif
#endif
}

// Signals `process`'s whole process group (SIGTERM, then SIGKILL if it hasn't
// exited after terminateTimeoutMs) rather than just its own pid, so descendants
// it spawned (e.g. apt-get's own helpers) are reached too -- each pipeline()
// stage gets its own session via makeNewSession() above specifically so this is
// meaningful per-stage, matching what startAndWait()'s single-process case
// already does for terminateAndKill(). A plain QProcess::kill()/terminate() call
// only ever reaches the one directly-spawned pid.
void terminateProcessGroup(QProcess &process, int terminateTimeoutMs)
{
    const qint64 pid = process.processId();
    const auto signalGroup = [pid](int signal) {
        return pid > 0 && ::kill(-static_cast<pid_t>(pid), signal) == 0;
    };
    if (!signalGroup(SIGTERM)) {
        process.terminate();
    }
    if (!process.waitForFinished(terminateTimeoutMs)) {
        if (!signalGroup(SIGKILL)) {
            process.kill();
        }
        process.waitForFinished(TerminateTimeoutMs);
    }
}

// Shell-quotes a single argv element for embedding in the one inner command
// string that `script -c` (procOnPty()'s implementation) requires -- script has
// no argv-vector mode, so this is the one place Cmd still has to build a shell
// string, deliberately kept minimal and local to that use.
QString shellQuoteForPty(QString text)
{
    text.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + text + QLatin1Char('\'');
}

// Directory for the auth-success marker. Prefer the caller's private runtime
// dir (/run/user/<uid>, mode 0700): the root helper can create the marker there
// and this unprivileged process can still remove it afterwards. In the
// world-writable /tmp fallback a root-owned marker cannot be unlinked from here,
// so it may briefly linger, but that only affects the check's cleanup.
QString markerDirectory()
{
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDir.isEmpty() && QDir(runtimeDir).exists()) {
        return runtimeDir;
    }
    return QDir::tempPath();
}

// Set by handleElevationError() whenever an elevated call anywhere is dismissed
// by the user, and cleared by resetElevationDismissed(). GUI is single-threaded,
// so a plain static is enough to let unrelated Cmd instances share this state.
bool elevationWasDismissed = false;
} // namespace

Cmd::Cmd(QObject *parent)
    : QProcess(parent),
      elevate {elevationTool()},
      helper {QStringLiteral(HELPER_PATH)}
{
    connect(this, &Cmd::readyReadStandardOutput, [this] { emit outputAvailable(readAllStandardOutput()); });
    connect(this, &Cmd::readyReadStandardError, [this] { emit errorAvailable(readAllStandardError()); });
    connect(this, &Cmd::outputAvailable, [this](const QString &out) { outBuffer += out; });
    connect(this, &Cmd::errorAvailable, [this](const QString &err) { outBuffer += err; });
}

// pkexec is the only supported elevation tool: the helper's polkit policy and its
// auth-marker check (isValidMarkerPath(), keyed on PKEXEC_UID) both depend on it.
QString Cmd::elevationTool()
{
    if (QFile::exists("/usr/bin/pkexec")) return QStringLiteral("/usr/bin/pkexec");
    return {};
}

QString Cmd::getOut(const QString &cmd, QuietMode quiet)
{
    outBuffer.clear();
    run(cmd, quiet);
    return outBuffer.trimmed();
}

QString Cmd::getOut(const QString &cmd, const QStringList &args, QuietMode quiet, bool discardStderr)
{
    QString output;
    proc(cmd, args, &output, nullptr, quiet, discardStderr);
    return output;
}

QString Cmd::getOut(const QHash<QString, QString> &environment, const QString &cmd, const QStringList &args,
                    QuietMode quiet, bool discardStderr)
{
    QString output;
    procWithEnv(environment, cmd, args, &output, nullptr, quiet, discardStderr);
    return output;
}

QStringList Cmd::helperExecArgs(const QString &cmd, const QStringList &args, const QHash<QString, QString> &environment) const
{
    QStringList helperArgs {"exec"};
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        helperArgs << "--env" << (it.key() + '=' + it.value());
    }
    helperArgs << cmd;
    helperArgs += args;
    return helperArgs;
}

bool Cmd::helperProc(const QStringList &helperArgs, QString *output, const QByteArray *input, QuietMode quiet)
{
    outBuffer.clear();
    if (getuid() != 0 && elevate.isEmpty()) {
        qWarning() << "No elevation helper available";
        return false;
    }

    const QString program = (getuid() == 0) ? helper : elevate;
    QStringList programArgs = helperArgs;
    // An elevated helper cannot be signalled by the unprivileged GUI after
    // pkexec changes its uid. For operations without stdin input, opt into the
    // helper's EOF cancellation protocol so closeWriteChannel() can request a
    // privileged child-process-group shutdown instead.
    if (input == nullptr) {
        programArgs.prepend(QStringLiteral("--cancel-on-eof"));
    }
    if (getuid() != 0) {
        programArgs.prepend(helper);
    }
    return startAndWait(program, programArgs, output, input, quiet, getuid() != 0, {}, input == nullptr);
}

bool Cmd::proc(const QString &cmd, const QStringList &args, QString *output, const QByteArray *input, QuietMode quiet,
              bool discardStderr)
{
    return startAndWait(cmd, args, output, input, quiet, false, {}, false, {}, discardStderr);
}

bool Cmd::procWithEnv(const QHash<QString, QString> &environment, const QString &cmd, const QStringList &args,
                      QString *output, const QByteArray *input, QuietMode quiet, bool discardStderr)
{
    return startAndWait(cmd, args, output, input, quiet, false, {}, false, environment, discardStderr);
}

bool Cmd::procAsRoot(const QString &cmd, const QStringList &args, QString *output, const QByteArray *input, QuietMode quiet)
{
    return helperProc(helperExecArgs(cmd, args), output, input, quiet);
}

bool Cmd::procAsRootWithEnv(const QHash<QString, QString> &environment, const QString &cmd, const QStringList &args,
                            QString *output, const QByteArray *input, QuietMode quiet)
{
    return helperProc(helperExecArgs(cmd, args, environment), output, input, quiet);
}

bool Cmd::procScriptAsRoot(const QString &path, const QStringList &args, QString *output, const QByteArray *input,
                           QuietMode quiet)
{
    if (getuid() == 0) {
        return proc(path, args, output, input, quiet);
    }
    if (elevate.isEmpty()) {
        qWarning() << "No elevation helper available";
        return false;
    }

    QStringList elevatedArgs {path};
    elevatedArgs += args;
    return startAndWait(elevate, elevatedArgs, output, input, quiet, true);
}

bool Cmd::run(const QString &cmd, QuietMode quiet)
{
    return startAndWait("/bin/bash", {"-c", cmd}, nullptr, nullptr, quiet, false);
}

bool Cmd::procOnPty(const QString &cmd, const QStringList &args, QuietMode quiet)
{
    QStringList quoted;
    quoted.reserve(args.size() + 1);
    quoted << shellQuoteForPty(cmd);
    for (const QString &arg : args) {
        quoted << shellQuoteForPty(arg);
    }
    const QString innerCommand = quoted.join(QLatin1Char(' '));
    // Only `script` itself is exec'd directly here -- the "/bin/bash -c" layer
    // Cmd::run() would otherwise wrap around it is gone. `shellCommand` is passed
    // through purely so quiet==No still logs a single readable line instead of a
    // three-element argv dump.
    return startAndWait(QStringLiteral("script"),
                        {QStringLiteral("-qefc"), innerCommand, QStringLiteral("/dev/null")}, nullptr, nullptr, quiet,
                        false, innerCommand);
}

bool Cmd::pipeline(const QList<PipelineStage> &stages, QString *output, QuietMode quiet)
{
    outBuffer.clear();
    helperMarkerPath.clear();
    if (stages.isEmpty()) {
        qWarning() << "Cmd::pipeline() called with no stages";
        return false;
    }
    if (state() != QProcess::NotRunning) {
        qDebug() << "Process already running:" << program() << arguments();
        return false;
    }
    elevatedOperation = false;

    if (quiet == QuietMode::No) {
        QStringList description;
        description.reserve(stages.size());
        for (const auto &stage : stages) {
            QStringList words {stage.program};
            words += stage.args;
            description << words.join(QLatin1Char(' '));
        }
        qDebug().noquote() << description.join(QStringLiteral(" | "));
    }

    // All but the last stage are disposable, unmonitored QProcess helpers, wired
    // stdout-to-stdin via Qt's own pipe support; see the declaration in cmd.h for
    // the full rationale and the caveats (approximate exit-status semantics).
    const qsizetype upstreamCount = stages.size() - 1;
    std::vector<std::unique_ptr<QProcess>> upstream;
    upstream.reserve(static_cast<size_t>(upstreamCount));
    for (qsizetype i = 0; i < upstreamCount; ++i) {
        upstream.push_back(std::make_unique<QProcess>());
    }
    // Made reachable to terminateAndKill() (see activePipelineUpstream's
    // declaration) for the whole time any of these might be running -- cleared
    // unconditionally on every exit path below via this scope guard, since the
    // pointers become dangling the moment `upstream` itself is destroyed at the
    // end of this function.
    for (const auto &stageProcess : upstream) {
        activePipelineUpstream.append(stageProcess.get());
    }
    auto clearActiveUpstream = qScopeGuard([this] { activePipelineUpstream.clear(); });

    for (qsizetype i = 0; i < upstreamCount; ++i) {
        QProcess *stageProcess = upstream[static_cast<size_t>(i)].get();
        stageProcess->setProcessEnvironment(environmentWithOverrides(stages[i].environment));
        makeNewSession(*stageProcess);
        // An upstream stage's stderr is not read anywhere: forward it straight to
        // this process's own stderr (matching a shell pipeline, where only stdout
        // is redirected) rather than let it sit in an unread pipe buffer.
        stageProcess->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QProcess *next = (i + 1 < upstreamCount) ? upstream[static_cast<size_t>(i + 1)].get()
                                                  : static_cast<QProcess *>(this);
        stageProcess->setStandardOutputProcess(next);
    }

    setProcessEnvironment(environmentWithOverrides(stages.constLast().environment));
    // See startAndWait()'s matching reset: this must run unconditionally so a Cmd
    // that previously discarded stderr via proc()/getOut() doesn't silently keep
    // discarding it here too -- pipeline() has no per-stage discardStderr concept
    // of its own, so the last stage always captures stderr normally.
    setStandardErrorFile(QString());
    makeNewSession(*this);

    QEventLoop loop;
    connect(this, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);

    for (qsizetype i = 0; i < upstreamCount; ++i) {
        upstream[static_cast<size_t>(i)]->start(stages[i].program, stages[i].args);
    }
    start(stages.constLast().program, stages.constLast().args);

    bool allStarted = waitForStarted();
    for (auto &stageProcess : upstream) {
        allStarted = stageProcess->waitForStarted() && allStarted;
    }
    if (!allStarted) {
        for (auto &stageProcess : upstream) {
            if (stageProcess->state() != QProcess::NotRunning) {
                terminateProcessGroup(*stageProcess, TerminateTimeoutMs);
            }
        }
        if (state() != QProcess::NotRunning) {
            terminateProcessGroup(*this, TerminateTimeoutMs);
        }
        elevatedOperation = false;
        if (output) {
            *output = outBuffer.trimmed();
        }
        emit done();
        return false;
    }

    loop.exec();

    // The last stage has finished; upstream stages should exit on their own once
    // they observe EOF/SIGPIPE with nothing left reading their output (or have
    // already been signaled by terminateAndKill(), if that's what unblocked
    // loop.exec() above). Wait briefly, then clean up any stragglers -- including
    // their own descendants, via terminateProcessGroup() -- so a pipeline never
    // leaks a process.
    for (auto &stageProcess : upstream) {
        if (stageProcess->state() != QProcess::NotRunning && !stageProcess->waitForFinished(500)) {
            terminateProcessGroup(*stageProcess, TerminateTimeoutMs);
        }
    }

    elevatedOperation = false;
    if (output) {
        *output = outBuffer.trimmed();
    }
    emit done();
    return (exitStatus() == QProcess::NormalExit && exitCode() == 0);
}

bool Cmd::runHookAsRoot(const QString &script, QuietMode quiet)
{
    return helperProc({"run-hook", script}, nullptr, nullptr, quiet);
}

QString Cmd::lockingProcessAsRoot(const QString &path, QuietMode quiet)
{
    QString output;
    if (!helperProc({"locking-process", path}, &output, nullptr, quiet)) {
        if (Cmd::elevationDismissed()) {
            return {};
        }
        return output.trimmed().isEmpty() ? QStringLiteral("unknown process") : output.trimmed();
    }
    return output.trimmed();
}

bool Cmd::writeFileAsRoot(const QString &path, const QString &content, QuietMode quiet)
{
    return helperProc({"write-file", path, content}, nullptr, nullptr, quiet);
}

bool Cmd::startAndWait(const QString &program, const QStringList &arguments, QString *output, const QByteArray *input,
                       QuietMode quiet, bool elevated, const QString &shellCommand, bool protectedOperation,
                       const QHash<QString, QString> &environmentOverrides, bool discardStderr)
{
    outBuffer.clear();
    helperMarkerPath.clear();
    if (state() != QProcess::NotRunning) {
        qDebug() << "Process already running:" << this->program() << this->arguments();
        return false;
    }
    elevatedOperation = protectedOperation;

    if (quiet == QuietMode::No) {
        if (shellCommand.isEmpty()) {
            qDebug() << program << arguments;
        } else {
            qDebug().noquote() << shellCommand;
        }
    }

    setProcessEnvironment(environmentWithOverrides(environmentOverrides));
    // Always set this explicitly, not just when discarding: QProcess's channel
    // redirection persists across start() calls on the same object, so without an
    // else branch here, a Cmd that ever discarded stderr once (e.g. a Flatpak
    // listing) would silently keep discarding it on every later call, including
    // ones that need to capture a real failure's stderr.
    setStandardErrorFile(discardStderr ? QProcess::nullDevice() : QString());
    QStringList launchArgs = arguments;
    if (elevated) {
        helperMarkerPath = markerDirectory() + QStringLiteral("/mx-pkg-helper-")
                           + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".marker");
        // pkexec sanitizes the environment, so hand the marker path to the target
        // as explicit leading arguments.
        if (!launchArgs.isEmpty()) {
            launchArgs.insert(1, helperMarkerPath);
            launchArgs.insert(1, QStringLiteral("--marker"));
        }
    }

    QEventLoop loop;
    connect(this, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);
    makeNewSession(*this);
    start(program, launchArgs);
    if (!waitForStarted()) {
        if (elevated) {
            QFile::remove(helperMarkerPath);
            helperMarkerPath.clear();
        }
        elevatedOperation = false;
        if (output) {
            *output = outBuffer.trimmed();
        }
        emit done();
        return false;
    }
    if (input) {
        if (!input->isEmpty()) {
            write(*input);
        }
        closeWriteChannel();
    }
    loop.exec();

    if (elevated) {
        if (isAuthenticationDismissed()) {
            handleElevationError();
        }
        QFile::remove(helperMarkerPath);
        helperMarkerPath.clear();
    }
    elevatedOperation = false;

    if (output) {
        *output = outBuffer.trimmed();
    }

    emit done();
    return (exitStatus() == QProcess::NormalExit && exitCode() == 0);
}

// Return true when the operation (including its child process group) has stopped.
bool Cmd::terminateAndKill()
{
    if (state() != QProcess::NotRunning) {
        if (elevatedOperation) {
            // Once pkexec has changed uid, an unprivileged UI cannot signal the
            // root process group. --cancel-on-eof tells the helper to terminate
            // its own tree; this also cancels the operation if the GUI crashes.
            closeWriteChannel();
            waitForFinished(ElevatedTerminateTimeoutMs);
            return state() == QProcess::NotRunning;
        }

        const qint64 pid = processId();
        const auto signalProcessGroup = [pid](int signal) {
            return pid > 0 && ::kill(-static_cast<pid_t>(pid), signal) == 0;
        };

        if (!signalProcessGroup(SIGTERM)) {
            // pkexec may already have changed the child to root, in which case
            // an unprivileged signal is denied. The helper watches this EOF for
            // --cancel-on-eof operations and terminates its own child group.
            closeWriteChannel();
            terminate();
        }
        if (!waitForFinished(TerminateTimeoutMs)) {
            if (!signalProcessGroup(SIGKILL)) {
                kill();
            }
            waitForFinished(TerminateTimeoutMs);
        }
    }
    // If a pipeline() call is currently in flight (reentrantly cancelled from,
    // e.g., a Cancel button's slot while pipeline()'s own nested QEventLoop is
    // pumping events -- see activePipelineUpstream's declaration), its upstream
    // stages are each in their own session and so aren't reached by the handling
    // above, which only ever signals `this` (the pipeline's last stage). Signal
    // each of them too, so cancelling mid-pipeline actually stops the whole
    // chain -- including their own descendants -- rather than leaving upstream
    // producers running after the user believes the operation was cancelled.
    // Empty (a no-op loop) whenever no pipeline() call is active.
    for (QProcess *stageProcess : std::as_const(activePipelineUpstream)) {
        if (stageProcess->state() != QProcess::NotRunning) {
            terminateProcessGroup(*stageProcess, TerminateTimeoutMs);
        }
    }
    return state() == QProcess::NotRunning;
}

QString Cmd::readAllOutput() const
{
    return outBuffer.trimmed();
}

bool Cmd::isAuthenticationDismissed() const
{
    return classifyAuthDismissed(exitStatus(), exitCode(), helperMarkerPath.isEmpty(),
                                 QFile::exists(helperMarkerPath));
}

bool Cmd::classifyAuthDismissed(QProcess::ExitStatus exitStatus, int exitCode, bool markerPathEmpty,
                                bool markerExists)
{
    if (exitStatus != QProcess::NormalExit || markerPathEmpty) {
        return false;
    }
    // pkexec returns 126 or 127 when auth is dismissed (varies by version).
    if (exitCode != 126 && exitCode != 127) {
        return false;
    }
    // The helper creates a marker file when it starts. If the file exists, auth succeeded
    // (helper ran). If it doesn't exist with exit code 126/127, auth was dismissed.
    return !markerExists;
}

void Cmd::handleElevationError()
{
    elevationWasDismissed = true;
    QMessageBox::critical(nullptr, tr("Administrator Access Required"),
                          tr("This operation requires administrator privileges. Try the action again and "
                             "enter your password when prompted."));
}

bool Cmd::elevationDismissed()
{
    return elevationWasDismissed;
}

void Cmd::resetElevationDismissed()
{
    elevationWasDismissed = false;
}
