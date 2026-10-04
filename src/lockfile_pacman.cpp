/**********************************************************************
 *  lockfile_pacman.cpp
 **********************************************************************
 * Copyright (C) 2014-2026 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *
 * This file is part of MX Package Installer.
 *
 * This program is free software: you can redistribute it and/or modify
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

#include "lockfile.h"

#include <QFile>
#include <QMessageBox>

#include "cmd.h"

namespace {

const QString &pacmanLockPath()
{
    static const QString path = QStringLiteral("/var/lib/pacman/db.lck");
    return path;
}

// libalpm holds its lock by creating db.lck (O_CREAT|O_EXCL) and removing it when
// done; it never takes an fcntl/lockf lock on the file. The file's existence is
// the lock, exactly as pacman itself treats it.
bool pacmanDbLocked()
{
    return QFile::exists(pacmanLockPath());
}

} // namespace

bool LockFile::isLocked()
{
    return pacmanDbLocked();
}

// Check if the file is locked and pop up a message
bool LockFile::isLockedGUI()
{
    if (Cmd::elevationDismissed()) {
        return true;
    }
    const QString proc = getLockingProcess();
    if (Cmd::elevationDismissed()) {
        // The lock check itself required elevation and the user dismissed it.
        // Cmd already showed the "Administrator Access Required" message; block
        // the operation without adding a misleading lock warning.
        return true;
    }
    if (!proc.isEmpty()) {
        QMessageBox::warning(nullptr, QObject::tr("Warning"),
                             QObject::tr("Pacman database is locked by another program: %1"
                                         "\nClose the program, or wait until it is done processing and try again.")
                                 .arg(proc));
        return true;
    }
    return false;
}

QString LockFile::getLockingProcess() const
{
    if (!pacmanDbLocked()) {
        return {};
    }
    const QString holder = Cmd().lockingProcessAsRoot(pacmanLockPath(), Cmd::QuietMode::Yes);
    if (!holder.isEmpty() || Cmd::elevationDismissed() || !pacmanDbLocked()) {
        return holder;
    }
    // No process holds db.lck, but pacman still refuses to run while it exists (a lock
    // left behind by an interrupted pacman), so the database stays locked.
    return QObject::tr("no running process (stale lock file %1)").arg(pacmanLockPath());
}
