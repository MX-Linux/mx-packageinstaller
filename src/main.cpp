/**********************************************************************
 *  main.cpp
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
#include <QApplication>
#include <QCommandLineParser>
#include <QDate>
#include <QDebug>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <unistd.h>

#include "log.h"
#include "mainwindow.h"

#ifndef VERSION
    #define VERSION "?.?.?.?"
#endif

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qWarning("mx-packageinstaller: no display available (DISPLAY and WAYLAND_DISPLAY are both unset); "
                "a graphical session is required to run this program.");
        return EXIT_FAILURE;
    }

    if (getuid() == 0) {
        qputenv("XDG_RUNTIME_DIR", "/run/user/0");
        qunsetenv("SESSION_MANAGER");
    }
    // Set Qt platform to XCB (X11) if not already set and we're in X11 environment
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        if (!qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
            qputenv("QT_QPA_PLATFORM", "xcb");
        }
    }

    QApplication app(argc, argv);
    if (getuid() == 0) {
        qputenv("HOME", "/root");
    }
    qputenv("TERM", "dumb");

    QApplication::setApplicationName("mx-packageinstaller");

    QApplication::setWindowIcon(QIcon::fromTheme(QApplication::applicationName()));
    QApplication::setOrganizationName("MX-Linux");
    QApplication::setApplicationVersion(VERSION);

    QTranslator qtTran;
    if (qtTran.load("qt_" + QLocale().name(), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTran);
    }

    QTranslator qtBaseTran;
    if (qtBaseTran.load("qtbase_" + QLocale().name(), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtBaseTran);
    }

    QTranslator appTran;
    if (appTran.load(QApplication::applicationName() + "_" + QLocale().name(),
                     "/usr/share/" + QApplication::applicationName() + "/locale")) {
        QApplication::installTranslator(&appTran);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QObject::tr("MX Package Installer is a tool used for managing packages on MX Linux\n\
    - installs popular programs from different sources\n\
    - installs programs from the MX Test repo\n\
    - installs programs from Debian Backports repo\n\
    - installs flatpaks"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {{"s", "skip-online-check"}, QObject::tr("Skip online check if it falsely reports lack of internet access.")});
    parser.process(app);

    // Root guard
    QFile loginUidFile {"/proc/self/loginuid"};
    if (loginUidFile.open(QIODevice::ReadOnly)) {
        QString loginUid = QString(loginUidFile.readAll()).trimmed();
        loginUidFile.close();
        if (loginUid == QLatin1String("0")) {
            QMessageBox::critical(
                nullptr, QObject::tr("Error"),
                QObject::tr(
                    "You seem to be logged in as root, please log out and log in as normal user to use this program."));
            return EXIT_FAILURE;
        }
    }
    if (getuid() != 0) {
        if (!QFile::exists("/usr/bin/pkexec")) {
            QMessageBox::critical(nullptr, QObject::tr("Error"),
                                  QObject::tr("You must run this program with admin access."));
            return EXIT_FAILURE;
        }
    }
    Log startLog(Log::defaultLogPath());
    MainWindow w(parser);
    w.show();
    return QApplication::exec();
}
