/**********************************************************************
 *  helperengine.cpp
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

#include "helperengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <optional>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace HelperEngine
{
namespace
{
volatile sig_atomic_t activeChildProcessGroup = 0;

extern "C" void forwardTerminationToChild(int signal)
{
    const sig_atomic_t pid = activeChildProcessGroup;
    if (pid > 0) {
        ::kill(-static_cast<pid_t>(pid), signal);
    }
}
} // namespace

void installTerminationHandlers()
{
    struct sigaction action {};
    action.sa_handler = forwardTerminationToChild;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
}

void writeAndFlush(FILE *stream, const QByteArray &data)
{
    if (!data.isEmpty()) {
        std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stream);
        std::fflush(stream);
    }
}

QString resolveBinary(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isExecutable()) {
            return candidate;
        }
    }
    return {};
}

namespace
{
void createProcessSession(QProcess &process)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession
                                     | QProcess::UnixProcessFlag::ResetSignalHandlers);
#else
    process.setChildProcessModifier([] {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGHUP, &action, nullptr);
        ::setsid();
    });
#endif
}

void terminateProcessGroup(QProcess &process)
{
    const qint64 pid = process.processId();
    const auto signalGroup = [pid](int signal) {
        return pid > 0 && ::kill(-static_cast<pid_t>(pid), signal) == 0;
    };
    if (!signalGroup(SIGTERM)) {
        process.terminate();
    }
    if (!process.waitForFinished(10000)) {
        if (!signalGroup(SIGKILL)) {
            process.kill();
        }
        process.waitForFinished(2000);
    }
}
} // namespace

ProcessResult runProcess(const QString &program, const QStringList &args,
                         const QHash<QString, QString> &environment, bool relayStdout, bool relayStderr,
                         bool cancelOnStdinEof)
{
    ProcessResult result;

    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        env.insert(it.key(), it.value());
    }
    process.setProcessEnvironment(env);
    createProcessSession(process);
    process.start(program, args, QIODevice::ReadWrite);
    if (!process.waitForStarted()) {
        result.standardError = QString("Failed to start %1").arg(program).toUtf8();
        result.exitCode = 127;
        return result;
    }

    result.started = true;
    activeChildProcessGroup = static_cast<sig_atomic_t>(process.processId());

    QFile stdinFile;
    const bool stdinOpen = stdinFile.open(stdin, QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!stdinOpen) {
        process.closeWriteChannel();
    }

    // QSocketNotifier requires a valid descriptor; stdinFile.handle() returns -1
    // when stdinOpen is false, so only construct it when there's a real fd to watch
    // instead of building one with an invalid descriptor and immediately disabling it.
    std::optional<QSocketNotifier> stdinNotifier;
    if (stdinOpen) {
        stdinNotifier.emplace(stdinFile.handle(), QSocketNotifier::Read);
        QObject::connect(&*stdinNotifier, &QSocketNotifier::activated, [&](QSocketDescriptor) {
            const QByteArray data = stdinFile.read(4096);
            if (data.isEmpty()) {
                stdinNotifier->setEnabled(false);
                if (cancelOnStdinEof) {
                    result.cancelled = true;
                    terminateProcessGroup(process);
                } else {
                    process.closeWriteChannel();
                }
            } else {
                process.write(data);
            }
        });
    }

    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        QCoreApplication::processEvents();

        const QByteArray stdoutChunk = process.readAllStandardOutput();
        if (!stdoutChunk.isEmpty()) {
            result.standardOutput += stdoutChunk;
            if (relayStdout) {
                writeAndFlush(stdout, stdoutChunk);
            }
        }

        const QByteArray stderrChunk = process.readAllStandardError();
        if (!stderrChunk.isEmpty()) {
            result.standardError += stderrChunk;
            if (relayStderr) {
                writeAndFlush(stderr, stderrChunk);
            }
        }
    }

    result.exitStatus = process.exitStatus();
    result.exitCode = process.exitCode();
    activeChildProcessGroup = 0;
    return result;
}

ProcessResult runProcessOnPty(const QString &program, const QStringList &args,
                              const QHash<QString, QString> &environment, bool cancelOnStdinEof)
{
    ProcessResult result;

    // Materialize argv and the environment overrides before forking; the child must
    // avoid heap allocation between fork() and exec().
    const QByteArray programBytes = program.toLocal8Bit();
    QList<QByteArray> argStorage;
    argStorage.reserve(args.size());
    for (const QString &arg : args) {
        argStorage.append(arg.toLocal8Bit());
    }
    std::vector<char *> argv;
    argv.reserve(static_cast<size_t>(args.size()) + 2);
    argv.push_back(const_cast<char *>(programBytes.constData()));
    for (QByteArray &arg : argStorage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    QList<QPair<QByteArray, QByteArray>> envOverrides;
    envOverrides.reserve(environment.size());
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        envOverrides.append({it.key().toLocal8Bit(), it.value().toLocal8Bit()});
    }

    // Hand the child a roomy terminal so progress bars are not truncated.
    struct winsize ws {};
    ws.ws_row = 24;
    ws.ws_col = 120;

    int masterFd = -1;
    const pid_t pid = forkpty(&masterFd, nullptr, nullptr, &ws);
    if (pid < 0) {
        result.standardError = QByteArrayLiteral("Failed to allocate pseudo-terminal");
        result.exitCode = 127;
        return result;
    }

    if (pid == 0) {
        // Child: the helper is single-threaded, so applying the environment with
        // setenv() before exec() is safe here.
        for (const auto &envVar : envOverrides) {
            setenv(envVar.first.constData(), envVar.second.constData(), 1);
        }
        // forkpty leaves the pty master open in the child; close it (and any other
        // inherited descriptors above stdio) so the privileged child only keeps the
        // terminal it needs. Best effort: ignore failure on kernels without close_range.
        close_range(STDERR_FILENO + 1, ~0U, 0);
        execv(programBytes.constData(), argv.data());
        _exit(127); // exec failed
    }

    result.started = true;
    activeChildProcessGroup = static_cast<sig_atomic_t>(pid);

    const int stdinFd = fileno(stdin);
    bool stdinOpen = stdinFd >= 0;
    bool cancellationRequested = false;
    bool killSent = false;
    QElapsedTimer cancellationTimer;
    char buffer[4096];

    for (;;) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(masterFd, &readFds);
        int maxFd = masterFd;
        if (stdinOpen) {
            FD_SET(stdinFd, &readFds);
            maxFd = std::max(maxFd, stdinFd);
        }

        timeval timeout {};
        timeval *timeoutPtr = nullptr;
        if (cancellationRequested) {
            timeout.tv_usec = 100000;
            timeoutPtr = &timeout;
        }
        const int selectResult = select(maxFd + 1, &readFds, nullptr, nullptr, timeoutPtr);
        if (selectResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (selectResult == 0) {
            if (!killSent && cancellationTimer.elapsed() >= 10000) {
                ::kill(-pid, SIGKILL);
                killSent = true;
            }
            continue;
        }

        if (FD_ISSET(masterFd, &readFds)) {
            const ssize_t count = read(masterFd, buffer, sizeof(buffer));
            if (count > 0) {
                const QByteArray chunk = QByteArray::fromRawData(buffer, static_cast<int>(count));
                result.standardOutput += chunk;
                writeAndFlush(stdout, chunk);
            } else if (count == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                break; // EIO on Linux once the child has closed the slave side
            }
        }

        if (stdinOpen && FD_ISSET(stdinFd, &readFds)) {
            const ssize_t count = read(stdinFd, buffer, sizeof(buffer));
            if (count > 0) {
                ssize_t written = 0;
                while (written < count) {
                    const ssize_t bytes = write(masterFd, buffer + written, static_cast<size_t>(count - written));
                    if (bytes <= 0) {
                        break;
                    }
                    written += bytes;
                }
            } else if (count == 0) {
                if (cancelOnStdinEof) {
                    result.cancelled = true;
                    cancellationRequested = true;
                    cancellationTimer.start();
                    ::kill(-pid, SIGTERM);
                } else {
                // Our stdin hit EOF. In canonical mode the child only sees EOF
                // once we send the terminal's VEOF character; without this a
                // child that reads stdin to EOF would hang waiting for input
                // that never comes. (Skipped in raw mode, where VEOF would just
                // be delivered as a stray data byte.)
                struct termios tio {};
                if (tcgetattr(masterFd, &tio) == 0 && (tio.c_lflag & ICANON) != 0) {
                    const char eofByte = static_cast<char>(tio.c_cc[VEOF]);
                    if (write(masterFd, &eofByte, 1) < 0) {
                        // best effort: nothing useful to do if this fails
                    }
                }
                }
                stdinOpen = false;
            } else if (errno != EINTR && errno != EAGAIN) {
                stdinOpen = false; // read error; stop forwarding
            }
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(masterFd);
    activeChildProcessGroup = 0;

    if (WIFEXITED(status)) {
        result.exitStatus = QProcess::NormalExit;
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitStatus = QProcess::CrashExit;
        result.exitCode = 1;
    }
    return result;
}

int relayResult(const ProcessResult &result)
{
    if (!result.started) {
        return result.exitCode;
    }
    if (result.cancelled) {
        return 143;
    }
    return result.exitStatus == QProcess::NormalExit ? result.exitCode : 1;
}

// The marker path comes from the (unprivileged) caller. Restrict it to a marker
// directly in the caller's runtime directory (/run/user/<PKEXEC_UID>) or the
// temp directory before creating it as root.
bool isValidMarkerPath(const QString &path)
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    if (!name.startsWith(QLatin1String("mx-pkg-helper-")) || !name.endsWith(QLatin1String(".marker"))) {
        return false;
    }
    const QString dir = info.absolutePath();
    const QString pkexecUid = qEnvironmentVariable("PKEXEC_UID");
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (digits.match(pkexecUid).hasMatch() && dir == QStringLiteral("/run/user/") + pkexecUid) {
        return true;
    }
    return dir == QDir::tempPath();
}

// This is the sole privileged marker creator used by both the helper and the
// policy-authorized shell scripts. O_EXCL rejects every pre-existing path
// (including symlinks), while O_NOFOLLOW provides an explicit second guard
// against following a symlink during the create operation. A pre-created marker
// consequently aborts the operation rather than risking a root write through an
// attacker-controlled path; UUID marker names and the private runtime directory
// make that denial of service impractical during normal operation.
bool createMarker(const QString &path)
{
    if (!isValidMarkerPath(path)) {
        writeAndFlush(stderr, QString("Invalid marker path: %1\n").arg(path).toUtf8());
        return false;
    }

    const QByteArray nativePath = QFile::encodeName(path);
    const int fd = ::open(nativePath.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd == -1) {
        writeAndFlush(stderr, QString("Could not create marker file %1: %2\n")
                                   .arg(path, QString::fromLocal8Bit(std::strerror(errno)))
                                   .toUtf8());
        return false;
    }

    if (::close(fd) == -1) {
        writeAndFlush(stderr, QString("Could not close marker file %1: %2\n")
                                   .arg(path, QString::fromLocal8Bit(std::strerror(errno)))
                                   .toUtf8());
        return false;
    }
    return true;
}
namespace
{
constexpr qint64 logLimit = 1024 * 1024;
constexpr qint64 historyLimit = 5 * logLimit;

enum class ReadResult { Read, Missing, Failed };

ReadResult readLogTail(const QString &path, uid_t owner, qint64 limit, QByteArray &data, struct stat &info)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT || errno == ELOOP ? ReadResult::Missing : ReadResult::Failed;
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return ReadResult::Failed;
    }
    if (::fstat(fd, &info) != 0) {
        return ReadResult::Failed;
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != owner) {
        return ReadResult::Missing;
    }
    const qint64 length = qMin<qint64>(info.st_size, limit);
    if (!file.seek(info.st_size - length)) {
        return ReadResult::Failed;
    }
    data = file.read(length);
    return file.error() == QFileDevice::NoError ? ReadResult::Read : ReadResult::Failed;
}

bool publishLog(const QString &path, const QByteArray &data, mode_t mode)
{
    QTemporaryFile temp(path + ".XXXXXX");
    if (!temp.open() || temp.write(data) != data.size() || !temp.flush()
        || ::fchmod(temp.handle(), mode & 0644) != 0 || ::fsync(temp.handle()) != 0) {
        return false;
    }
    // POSIX rename atomically replaces even a legacy symlink at the destination.
    return ::rename(QFile::encodeName(temp.fileName()).constData(), QFile::encodeName(path).constData()) == 0;
}
} // namespace

bool archiveLog(const QStringList &sources, uid_t owner, const QString &destination)
{
    QLockFile lock(destination + ".lock");
    if (!lock.tryLock(5000)) {
        return false;
    }
    for (const QString &source : sources) {
        struct stat sourceInfo {};
        QByteArray data;
        const auto result = readLogTail(source, owner, logLimit, data, sourceInfo);
        if (result == ReadResult::Missing) {
            continue;
        }
        if (result == ReadResult::Failed) {
            return false;
        }

        struct stat previousInfo {};
        QByteArray previous;
        const auto previousResult = readLogTail(destination, geteuid(), logLimit, previous, previousInfo);
        if (previousResult == ReadResult::Failed) {
            return false;
        }
        if (previousResult == ReadResult::Read) {
            const QString historyPath = destination + ".old";
            const QByteArray separator = "-----------------------------------------------------------\nMXPI SESSION\n"
                                         "-----------------------------------------------------------\n\n";
            QByteArray history;
            struct stat historyInfo {};
            const auto historyResult = readLogTail(historyPath, geteuid(), historyLimit - separator.size() - previous.size(),
                                                   history, historyInfo);
            if (historyResult == ReadResult::Failed
                || !publishLog(historyPath, history + separator + previous,
                               previousInfo.st_mode & (historyResult == ReadResult::Read ? historyInfo.st_mode : 0644))) {
                return false;
            }
        }
        if (!publishLog(destination, data, sourceInfo.st_mode)) {
            return false;
        }

        // Unlink with the caller's permissions, and leave any replacement session log alone.
        const uid_t privilegedUid = geteuid();
        if (::seteuid(owner) != 0) {
            return false;
        }
        struct stat current {};
        const QByteArray path = QFile::encodeName(source);
        const bool removed = ::lstat(path.constData(), &current) != 0 ? errno == ENOENT
                             : current.st_dev != sourceInfo.st_dev || current.st_ino != sourceInfo.st_ino
                                   || ::unlink(path.constData()) == 0;
        return ::seteuid(privilegedUid) == 0 && removed;
    }
    return false;
}
} // namespace HelperEngine
