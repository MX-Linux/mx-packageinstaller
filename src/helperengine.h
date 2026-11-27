/**********************************************************************
 *  helperengine.h
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

#pragma once

// The backend-agnostic scaffolding behind src/helper.cpp: spawning a child
// (optionally on a pty), forwarding this process's stdin to it, killing its
// whole process group on cancellation, and the auth-success marker file
// mechanics. Split out of helper.cpp (which retains the command allow-list,
// argv dispatch, and main()) purely so this part is a normal translation unit
// that Testing/test_helperengine.cpp can link directly and exercise
// unprivileged, without spawning the real (pkexec-invoked) helper binary.

#include <QByteArray>
#include <QHash>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <sys/types.h>

namespace HelperEngine
{
struct ProcessResult
{
    bool started = false;
    bool cancelled = false;
    int exitCode = 1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QByteArray standardOutput;
    QByteArray standardError;
};

// Installs SIGTERM/SIGHUP handlers that forward the signal on to whichever
// child process group is currently active (tracked internally), so killing
// this process also tears down what it spawned. Call once at startup.
void installTerminationHandlers();

void writeAndFlush(FILE *stream, const QByteArray &data);

// Returns the first candidate that exists and is executable, or an empty
// string if none are.
[[nodiscard]] QString resolveBinary(const QStringList &candidates);

// Runs `program` with `args` via QProcess, relaying stdout/stderr to our own
// streams (when requested) while also capturing them, and forwarding our own
// stdin to the child until it hits EOF. On stdin EOF: if cancelOnStdinEof is
// set, the child's whole process group is terminated and result.cancelled is
// set; otherwise the child's write channel is merely closed (the child keeps
// running and sees EOF on its own stdin).
[[nodiscard]] ProcessResult runProcess(const QString &program, const QStringList &args,
                                       const QHash<QString, QString> &environment = {}, bool relayStdout = true,
                                       bool relayStderr = true, bool cancelOnStdinEof = false);

// As runProcess(), but attaches the child to a pseudo-terminal so tools that
// gate their progress output on isatty() (notably `snap` and `pacman`) emit
// it live instead of staying silent until completion. Same stdin-EOF
// cancellation contract as runProcess().
[[nodiscard]] ProcessResult runProcessOnPty(const QString &program, const QStringList &args,
                                            const QHash<QString, QString> &environment, bool cancelOnStdinEof);

// Maps a ProcessResult to a process exit code: 143 (128 + SIGTERM) when the
// operation was cancelled, otherwise the child's own exit code (or 1 on an
// abnormal exit).
[[nodiscard]] int relayResult(const ProcessResult &result);

// Restricts a caller-supplied marker path to a recognizable
// "mx-pkg-helper-*.marker" name directly inside the caller's private runtime
// directory (/run/user/<PKEXEC_UID>) or the system temp directory, before it
// is ever opened as root.
[[nodiscard]] bool isValidMarkerPath(const QString &path);

// Atomically creates the marker file at `path`, which must satisfy
// isValidMarkerPath(). O_EXCL|O_NOFOLLOW reject any pre-existing path
// (including a symlink), so a pre-planted marker aborts the operation rather
// than letting a root-owned write follow an attacker-controlled path.
[[nodiscard]] bool createMarker(const QString &path);
// The maintenance action supplies fixed source and destination paths.
[[nodiscard]] bool archiveLog(const QStringList &sources, uid_t owner, const QString &destination);
} // namespace HelperEngine
