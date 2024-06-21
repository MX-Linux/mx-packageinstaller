/**********************************************************************
 *  MainWindow.cpp
 **********************************************************************
 * Copyright (C) 2017-2023 MX Authors
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
#include "ui_mainwindow.h"

#include <QDebug>
#include <QFileInfo>
#include <QImageReader>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QProgressBar>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QTextStream>
#include <QtGlobal>
#include <QtXml/QtXml>

#include "about.h"
#include "aptcache.h"
#include "versionnumber.h"
#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;

MainWindow::MainWindow(const QCommandLineParser &arg_parser, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow),
      dictionary("/usr/share/mx-packageinstaller-pkglist/category.dict", QSettings::IniFormat),
      args {arg_parser},
      reply(nullptr)
{
    qDebug().noquote() << QCoreApplication::applicationName() << "version:" << QCoreApplication::applicationVersion();
    ui->setupUi(this);
    setProgressDialog();

    connect(&timer, &QTimer::timeout, this, &MainWindow::updateBar);
    connect(&cmd, &Cmd::started, this, &MainWindow::cmdStart);
    connect(&cmd, &Cmd::done, this, &MainWindow::cmdDone);
    connect(&cmd, &Cmd::outputAvailable, [](const QString &out) { qDebug() << out.trimmed(); });
    connect(&cmd, &Cmd::errorAvailable, [](const QString &out) { qWarning() << out.trimmed(); });
    setWindowFlags(Qt::Window); // For the close, min and max buttons

    // Start displayPackage and displayFlatpaks in the background
    QTimer::singleShot(0, this, [this] {
        setup();
        QApplication::processEvents();
        {
            AptCache cache;
            enabled_list = cache.getCandidates();
            displayPackages();
        }
        if (arch != "i386" && checkInstalled("flatpak")) {
            if (!Cmd().run("flatpak remote-list --system --columns=name | grep -qw flathub", true)) {
                Cmd().runAsRoot(
                    "flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo");
            }
            if (!Cmd().run("flatpak remote-list --system --columns=name | grep -qw flathub-verified", true)) {
                Cmd().runAsRoot("flatpak remote-add --if-not-exists --subset=verified flathub-verified "
                                "https://flathub.org/repo/flathub.flatpakrepo");
            }
            displayFlatpaks();
        }
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setup()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->blockSignals(true);
    ui->pushRemoveOrphan->setHidden(true);

    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    ui->outputBox->setFont(font);

    fp_ver = getVersion("flatpak");
    FPuser = "--system ";

    arch = AptCache::getArch();
    ver_name = getDebianVerName();

    if (arch == "i386") {
        ui->tabWidget->setTabEnabled(Tab::Flatpak, false); // setTabVisible is available only on Qt >= 5.15
        ui->tabWidget->setTabToolTip(Tab::Flatpak, tr("Flatpak tab is disabled on 32-bit."));
    }

    if (!QFile::exists("/etc/apt/sources.list.d/mx.list")) {
        ui->tabWidget->setTabEnabled(Tab::Test, false); // setTabVisible is available only on Qt >= 5.15
        ui->tabWidget->setTabToolTip(Tab::Test, tr("Could not find MX sources."));
    }

    lock_file = new LockFile("/var/lib/dpkg/lock");

    test_initially_enabled
        = cmd.run("apt-get update --print-uris | grep -m1 -qE '/mx/testrepo/dists/" + ver_name + "/test/'");

    setWindowTitle(tr("MX Package Installer"));
    hideColumns();
    setIcons();
    loadPmFiles();
    refreshPopularApps();
    setConnections();

    ui->searchPopular->setFocus();
    currentTree = ui->treePopularApps;

    ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), false);
    ui->tabWidget->blockSignals(false);
    ui->pushUpgradeAll->setVisible(false);

    const auto size = this->size();
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
        if (isMaximized()) { // Add option to resize if maximized
            resize(size);
            centerWindow();
        }
    }

    // Check/uncheck tree items spacebar press or double-click
    auto *shortcutToggle = new QShortcut(Qt::Key_Space, this);
    connect(shortcutToggle, &QShortcut::activated, this, &MainWindow::checkUncheckItem);

    QList listTree {ui->treePopularApps, ui->treeEnabled, ui->treeMXtest, ui->treeBackports, ui->treeFlatpak};
    for (const auto &tree : listTree) {
        if (tree != ui->treeFlatpak) {
            tree->setContextMenuPolicy(Qt::CustomContextMenu);
        }
        connect(tree, &QTreeWidget::itemDoubleClicked, [tree](QTreeWidgetItem *item) { tree->setCurrentItem(item); });
        connect(tree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::checkUncheckItem);
        connect(tree, &QTreeWidget::customContextMenuRequested, this,
                [this, tree](QPoint pos) { displayPackageInfo(tree, pos); });
    }
}

bool MainWindow::uninstall(const QString &names, const QString &preuninstall, const QString &postuninstall)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setCurrentWidget(ui->tabOutput);

    bool success = true;
    // Simulate install of selections and present for confirmation
    // if user selects cancel, break routine but return success to avoid error message
    if (!confirmActions(names, "remove")) {
        return true;
    }

    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Uninstalling packages..."));
    enableOutput();

    if (!preuninstall.isEmpty()) {
        qDebug() << "Pre-uninstall";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Running pre-uninstall operations..."));
        enableOutput();
        if (lock_file->isLockedGUI()) {
            return false;
        }
        success = cmd.runAsRoot(preuninstall);
    }

    if (success) {
        enableOutput();
        if (lock_file->isLockedGUI()) {
            return false;
        }
        success = cmd.runAsRoot("DEBIAN_FRONTEND=$(dpkg -l debconf-kde-helper 2>/dev/null | grep -sq ^i "
                                "&& echo kde || echo gnome) apt-get -o=Dpkg::Use-Pty=0 remove -y "
                                + names); // use -y since there is a confirm dialog already
    }

    if (success && !postuninstall.isEmpty()) {
        qDebug() << "Post-uninstall";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Running post-uninstall operations..."));
        enableOutput();
        if (lock_file->isLockedGUI()) {
            return false;
        }
        success = cmd.runAsRoot(postuninstall);
    }
    return success;
}

bool MainWindow::updateApt()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (lock_file->isLockedGUI()) {
        return false;
    }
    ui->tabOutput->isVisible() // Don't display in output if calling to refresh from tabs
        ? ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Refreshing sources..."))
        : progress->show();
    if (!timer.isActive()) {
        timer.start(100ms);
    }

    enableOutput();
    if (cmd.runAsRoot("apt-get update -o=Dpkg::Use-Pty=0 -o Acquire::http:Timeout=10 -o Acquire::https:Timeout=10 -o "
                      "Acquire::ftp:Timeout=10")) {
        qDebug() << "sources updated OK";
        updated_once = true;
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
quint64 MainWindow::convert(const QString &size)
{
    QString number = size.section(QChar(160), 0, 0);
    QString unit = size.section(QChar(160), 1).toUpper();
    double value = number.toDouble();
    if (unit == "KB") {
        return static_cast<quint64>(value * KiB);
    } else if (unit == "MB") {
        return static_cast<quint64>(value * MiB);
    } else if (unit == "GB") {
        return static_cast<quint64>(value * GiB);
    } else { // Bytes
        return static_cast<quint64>(value);
    }
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

    QStringList list;
    if (fp_ver < VersionNumber("1.0.1")) { // Older version doesn't display all apps
                                           // and runtimes without specifying them
        list = cmd.getOut("flatpak -d list  " + FPuser + "--app |tr -s ' ' |cut -f1,5,6 -d' '").split('\n');
        QStringList runtimes
            = cmd.getOut("flatpak -d list " + FPuser + "--runtime|tr -s ' '|cut -f1,5,6 -d' '").split('\n');
        if (!runtimes.isEmpty()) {
            list << runtimes;
        }
        for (QTreeWidgetItemIterator it(ui->treeFlatpak); (*it) != nullptr; ++it) {
            for (const QString &item : qAsConst(list)) {
                QString name = item.section(' ', 0, 0);
                QString size = item.section(' ', 1);
                if (name == (*it)->data(FlatCol::FullName, Qt::UserRole)) {
                    (*it)->setText(FlatCol::Size, size);
                }
            }
        }
    } else if (fp_ver < VersionNumber("1.2.4")) {
        list = cmd.getOut("flatpak -d list " + FPuser + "|tr -s ' '|cut -f1,5").split('\n');
    } else {
        list = cmd.getOut("flatpak list " + FPuser + "--columns app,size").split('\n');
    }

    auto total = std::accumulate(list.cbegin(), list.cend(), quint64(0),
                                 [](quint64 acc, const QString &item) { return acc + convert(item.section('\t', 1)); });
    ui->labelNumSize->setText(convert(total));
}

// Block interface while updating Flatpak list
void MainWindow::blockInterfaceFP(bool block)
{
    ui->tabWidget->widget(Tab::Flatpak)->setEnabled(!block);
    ui->comboRemote->setDisabled(block);
    ui->comboFilterFlatpak->setDisabled(block);
    ui->comboUser->setDisabled(block);
    ui->searchBoxFlatpak->setDisabled(block);
    ui->treeFlatpak->setDisabled(block);
    ui->frameFP->setDisabled(block);
    ui->iconInstalledPackages_4->setDisabled(block);
    ui->labelRepo->setDisabled(block);
    block ? setCursor(QCursor(Qt::BusyCursor)) : setCursor(QCursor(Qt::ArrowCursor));
}

void MainWindow::updateInterface() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QApplication::setOverrideCursor(QCursor(Qt::ArrowCursor));
    progress->hide();

    QList<QTreeWidgetItem *> upgr_list;
    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        auto userData = (*it)->data(TreeCol::Status, Qt::UserRole);
        if (userData == Status::Upgradable) {
            upgr_list.append(*it);
        }
    }
    QList<QTreeWidgetItem *> inst_list;
    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        auto userData = (*it)->data(TreeCol::Status, Qt::UserRole);
        if (userData == Status::Installed) {
            inst_list.append(*it);
        }
    }
    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        (*it)->setHidden(false);
    }

    if (currentTree == ui->treeEnabled) {
        ui->labelNumApps->setText(QString::number(currentTree->topLevelItemCount()));
        ui->labelNumUpgr->setText(QString::number(upgr_list.count()));
        ui->labelNumInst->setText(QString::number(inst_list.count() + upgr_list.count()));
        ui->pushUpgradeAll->setVisible(!upgr_list.isEmpty());
        ui->pushForceUpdateEnabled->setEnabled(true);
        ui->searchBoxEnabled->setFocus();
    } else if (currentTree == ui->treeMXtest) {
        ui->labelNumApps_2->setText(QString::number(currentTree->topLevelItemCount()));
        ui->labelNumUpgrMX->setText(QString::number(upgr_list.count()));
        ui->labelNumInstMX->setText(QString::number(inst_list.count() + upgr_list.count()));
        ui->pushForceUpdateMX->setEnabled(true);
        ui->searchBoxMX->setFocus();
    } else if (currentTree == ui->treeBackports) {
        ui->labelNumApps_3->setText(QString::number(currentTree->topLevelItemCount()));
        ui->labelNumUpgrBP->setText(QString::number(upgr_list.count()));
        ui->labelNumInstBP->setText(QString::number(inst_list.count() + upgr_list.count()));
        ui->pushForceUpdateBP->setEnabled(true);
        ui->searchBoxBP->setFocus();
    }
}

uchar MainWindow::getDebianVerNum()
{
    QFile file {"/etc/debian_version"};
    QStringList list;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString line = in.readLine();
        list = line.split('.');
        file.close();
    } else {
        qCritical() << "Could not open /etc/debian_version:" << file.errorString() << "Assumes Bullseye";
        return Release::Bullseye;
    }
    bool ok = false;
    int ver = list.at(0).toInt(&ok);
    if (ok) {
        return ver;
    } else {
        QString verName = list.at(0).split('/').at(0);
        if (verName == "bullseye") {
            return Release::Bullseye;
        } else if (verName == "bookworm") {
            return Release::Bookworm;
        } else {
            qCritical() << "Unknown Debian version:" << ver << "Assumes Bullseye";
            return Release::Bullseye;
        }
    }
}

QString MainWindow::getDebianVerName()
{
    switch (getDebianVerNum()) {
    case Release::Jessie:
        return "jessie";
    case Release::Stretch:
        return "stretch";
    case Release::Buster:
        return "buster";
    case Release::Bullseye:
        return "bullseye";
    case Release::Bookworm:
        return "bookworm";
    case Release::Trixie:
        return "trixie";
    default:
        qWarning() << "Error: Invalid Debian version, assumes Bullseye";
        return "bullseye";
    }
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
    if (locale.name() == "en_US") {
        return item; // No need for translation
    }
    QStringList tagCandidates = {locale.name(), locale.name().section('_', 0, 0)};
    for (const auto &tag : tagCandidates) {
        dictionary.beginGroup(item);
        QString translation = dictionary.value(tag).toString().toLatin1();
        dictionary.endGroup();

        if (!translation.isEmpty()) {
            return translation;
        }
    }
    return item; // Return original item if no translation found
}

QString MainWindow::getArchOption() const
{
    static const QMap<QString, QString> archMap = {
        {"amd64", "--arch=x86_64 "}, {"i386", "--arch=i386 "}, {"armhf", "--arch=arm "}, {"arm64", "--arch=aarch64 "}};
    return archMap.value(arch, QString());
}

void MainWindow::updateBar()
{
    QApplication::processEvents();
    bar->setValue((bar->value() + 1) % bar->maximum() + 1);
}

void MainWindow::checkUncheckItem()
{
    auto *currentTreeWidget = qobject_cast<QTreeWidget *>(focusWidget());

    if (!currentTreeWidget || !currentTreeWidget->currentItem() || currentTreeWidget->currentItem()->childCount() > 0) {
        return;
    }
    const auto col = (currentTreeWidget == ui->treePopularApps) ? static_cast<uchar>(PopCol::Check)
                                                                : static_cast<uchar>(TreeCol::Check);
    const auto newCheckState
        = (currentTreeWidget->currentItem()->checkState(col) == Qt::Checked) ? Qt::Unchecked : Qt::Checked;

    currentTreeWidget->currentItem()->setCheckState(col, newCheckState);
}

void MainWindow::outputAvailable(const QString &output)
{
    ui->outputBox->moveCursor(QTextCursor::End);
    if (output.contains('\r')) {
        ui->outputBox->moveCursor(QTextCursor::Up, QTextCursor::KeepAnchor);
        ui->outputBox->moveCursor(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    }
    ui->outputBox->insertPlainText(output);
    ui->outputBox->verticalScrollBar()->setValue(ui->outputBox->verticalScrollBar()->maximum());
}

void MainWindow::loadPmFiles()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    const QString pmFolderPath {"/usr/share/mx-packageinstaller-pkglist"};
    const QStringList pmFileList = QDir(pmFolderPath).entryList({"*.pm"});

    QDomDocument doc;
    for (const QString &fileName : pmFileList) {
        QFile file(pmFolderPath + '/' + fileName);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qDebug() << "Could not open file:" << file.fileName();
        } else if (!doc.setContent(&file)) {
            qDebug() << "Could not load document:" << fileName << "-- not valid XML?";
        } else {
            processDoc(doc);
        }
        file.close();
    }
}

// Process dom documents (from .pm files)
void MainWindow::processDoc(const QDomDocument &doc)
{
    PopularInfo info;
    QDomElement root = doc.firstChildElement("app");
    QDomElement element = root.firstChildElement();
    while (!element.isNull()) {
        const QString tagName = element.tagName();
        QString trimmedText = element.text().trimmed();

        if (tagName == "category") {
            info.category = categoryTranslation(trimmedText);
        } else if (tagName == "name") {
            info.name = trimmedText;
        } else if (tagName == "description") {
            info.description = getLocalizedName(element);
        } else if (tagName == "installable") {
            info.installable = trimmedText;
        } else if (tagName == "screenshot") {
            info.screenshot = trimmedText;
        } else if (tagName == "preinstall") {
            info.preInstall = trimmedText;
        } else if (tagName == "install_package_names") {
            info.installNames = trimmedText.replace('\n', ' ');
        } else if (tagName == "postinstall") {
            info.postInstall = trimmedText;
        } else if (tagName == "uninstall_package_names") {
            info.uninstallNames = trimmedText;
        } else if (tagName == "postuninstall") {
            info.postUninstall = trimmedText;
        } else if (tagName == "preuninstall") {
            info.preUninstall = trimmedText;
        }
        element = element.nextSiblingElement();
    }
    const QString modArch = mapArchToFormat(arch);
    if (!isPackageInstallable(info.installable, modArch)) {
        return;
    }
    popular_apps.append(info);
}

QString MainWindow::mapArchToFormat(const QString &arch) const
{
    static const QMap<QString, QString> archMapping
        = {{"amd64", "64"}, {"i386", "32"}, {"armhf", "armhf"}, {"arm64", "armsixtyfour"}};

    return archMapping.value(arch, QString());
}

bool MainWindow::isPackageInstallable(const QString &installable, const QString &modArch) const
{
    return installable.split(',').contains(modArch) || installable == "all";
}

void MainWindow::refreshPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    disableOutput();
    ui->treePopularApps->clear();
    ui->searchPopular->clear();
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);
    installed_packages = listInstalled();
    displayPopularApps();
}

// In case of duplicates add extra name to disambiguate
void MainWindow::removeDuplicatesFP() const
{
    QTreeWidgetItemIterator it(ui->treeFlatpak);
    QTreeWidgetItem *prevItem = nullptr;
    QSet<QString> namesSet;

    // Find and mark duplicates
    while ((*it) != nullptr) {
        QString currentName = (*it)->text(FlatCol::Name);
        if (namesSet.contains(currentName)) {
            // Mark both occurrences as duplicate
            if (prevItem) {
                prevItem->setData(FlatCol::Duplicate, Qt::UserRole, true);
            }
            (*it)->setData(FlatCol::Duplicate, Qt::UserRole, true);
        } else {
            namesSet.insert(currentName);
        }
        prevItem = *it;
        ++it;
    }
    // Rename duplicates to use more context
    for (QTreeWidgetItemIterator it(ui->treeFlatpak); (*it) != nullptr; ++it) {
        if ((*it)->data(FlatCol::Duplicate, Qt::UserRole).toBool()) {
            QString longName = (*it)->text(FlatCol::LongName);
            (*it)->setText(FlatCol::Name, longName.section('.', -2));
        }
    }
}

void MainWindow::setConnections() const
{
    connect(QApplication::instance(), &QApplication::aboutToQuit, this, &MainWindow::cleanup, Qt::QueuedConnection);
    // Connect search boxes
    connect(ui->searchPopular, &QLineEdit::textChanged, this, &MainWindow::findPopular);
    connect(ui->searchBoxEnabled, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);
    connect(ui->searchBoxMX, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);
    connect(ui->searchBoxBP, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);
    connect(ui->searchBoxFlatpak, &QLineEdit::textChanged, this, &MainWindow::findPackageOther);

    // Connect combo filters
    connect(ui->comboFilterEnabled, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterMX, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterBP, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
    connect(ui->comboFilterFlatpak, &QComboBox::currentTextChanged, this, &MainWindow::filterChanged);
}

void MainWindow::setProgressDialog()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    progress = new QProgressDialog(this);
    bar = new QProgressBar(progress);
    bar->setMaximum(bar->maximum());
    pushCancel = new QPushButton(tr("Cancel"));
    connect(pushCancel, &QPushButton::clicked, this, &MainWindow::cancelDownload);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                             | Qt::WindowStaysOnTopHint);
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
    switch (ui->tabWidget->currentIndex()) {
    case Tab::EnabledRepos:
        ui->searchBoxEnabled->setFocus();
        break;
    case Tab::Test:
        ui->searchBoxMX->setFocus();
        break;
    case Tab::Backports:
        ui->searchBoxBP->setFocus();
        break;
    case Tab::Flatpak:
        ui->searchBoxFlatpak->setFocus();
        break;
    default:
        break;
    }
}

void MainWindow::displayPopularApps() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    QMap<QString, QTreeWidgetItem *> categoryMap;

    for (const auto &item : popular_apps) {
        QTreeWidgetItem *topLevelItem = nullptr;

        // Check if the category already exists, if not, create it
        if (!categoryMap.contains(item.category)) {
            topLevelItem = new QTreeWidgetItem();
            topLevelItem->setText(PopCol::Icon, item.category);
            ui->treePopularApps->addTopLevelItem(topLevelItem);

            QFont font;
            font.setBold(true);
            topLevelItem->setFont(PopCol::Icon, font);
            topLevelItem->setIcon(PopCol::Icon, QIcon::fromTheme("folder"));
            topLevelItem->setFirstColumnSpanned(true);

            categoryMap.insert(item.category, topLevelItem);
        } else {
            topLevelItem = categoryMap.value(item.category);
        }

        // Add package name as childItem to treePopularApps
        auto *childItem = new QTreeWidgetItem(topLevelItem);
        childItem->setText(PopCol::Name, item.name);
        childItem->setIcon(PopCol::Info, QIcon::fromTheme("dialog-information"));
        childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
        childItem->setCheckState(PopCol::Check, Qt::Unchecked);
        childItem->setText(PopCol::Description, item.description);
        childItem->setText(PopCol::InstallNames, item.installNames);

        childItem->setData(PopCol::UninstallNames, Qt::UserRole, item.uninstallNames);
        childItem->setData(PopCol::Screenshot, Qt::UserRole, item.screenshot);
        childItem->setData(PopCol::PostUninstall, Qt::UserRole, item.postUninstall);
        childItem->setData(PopCol::PreUninstall, Qt::UserRole, item.preUninstall);
        if (checkInstalled(item.uninstallNames)) {
            childItem->setIcon(PopCol::Check, qicon_installed);
        }
    }

    for (uchar i = 0; i < ui->treePopularApps->columnCount(); ++i) {
        ui->treePopularApps->resizeColumnToContents(i);
    }

    ui->treePopularApps->sortItems(PopCol::Icon, Qt::AscendingOrder);
    connect(ui->treePopularApps, &QTreeWidget::itemClicked, this, &MainWindow::displayPopularInfo,
            Qt::UniqueConnection);
}

void MainWindow::displayFilteredFP(QStringList list, bool raw)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->treeFlatpak->blockSignals(true);

    QMutableStringListIterator i(list);
    if (raw) { // Raw format that needs to be edited
        if (fp_ver < VersionNumber("1.2.4")) {
            while (i.hasNext()) {
                i.setValue(i.next().section('\t', 0, 0)); // Remove size
            }
        } else {
            while (i.hasNext()) {
                i.setValue(i.next().section('\t', 1, 1).section('/', 1)); // Remove version and size
            }
        }
    }
    uint total = 0;
    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        if (list.contains((*it)->data(FlatCol::FullName, Qt::UserRole).toString())) {
            ++total;
            (*it)->setHidden(false);
            (*it)->setData(0, Qt::UserRole, true); // Displayed flag
            if ((*it)->checkState(FlatCol::Check) == Qt::Checked
                && (*it)->data(FlatCol::Status, Qt::UserRole) == Status::Installed) {
                ui->pushUninstall->setEnabled(true);
                ui->pushInstall->setEnabled(false);
            } else {
                ui->pushUninstall->setEnabled(false);
                ui->pushInstall->setEnabled(true);
            }
        } else {
            (*it)->setHidden(true);
            (*it)->setData(0, Qt::UserRole, false); // Displayed flag
            if ((*it)->checkState(FlatCol::Check) == Qt::Checked) {
                (*it)->setCheckState(FlatCol::Check, Qt::Unchecked); // Uncheck hidden item
                change_list.removeOne((*it)->data(FlatCol::FullName, Qt::UserRole).toString());
            }
        }
        if (change_list.isEmpty()) { // Reset comboFilterFlatpak if nothing is selected
            ui->pushUninstall->setEnabled(false);
            ui->pushInstall->setEnabled(false);
        }
    }
    if (lastItemClicked) {
        ui->treeFlatpak->scrollToItem(lastItemClicked);
    }
    ui->labelNumAppFP->setText(QString::number(total));
    ui->treeFlatpak->blockSignals(false);
    blockInterfaceFP(false);
}

void MainWindow::displayPackages()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    displayPackagesIsRunning = true;

    QTreeWidget *newtree {nullptr};

    QMap<QString, PackageInfo> *list {nullptr};

    if (currentTree == ui->treeMXtest) {
        if (!dirtyTest) {
            return;
        }
        list = &mx_list;
        newtree = ui->treeMXtest;
        dirtyTest = false;
    } else if (currentTree == ui->treeBackports) {
        if (!dirtyBackports) {
            return;
        }
        list = &backports_list;
        newtree = ui->treeBackports;
        dirtyBackports = false;
    } else {
        if (!dirtyEnabledRepos) {
            return;
        }
        list = &enabled_list;
        newtree = ui->treeEnabled;
        dirtyEnabledRepos = false;
    }
    newtree->blockSignals(true);
    newtree->clear();

    // Create a list of apps, create a hash with app_name, app_info
    for (auto it = list->constBegin(); it != list->constEnd(); ++it) {
        auto *widget_item = new QTreeWidgetItem(newtree);
        widget_item->setCheckState(TreeCol::Check, Qt::Unchecked);
        widget_item->setText(TreeCol::Name, it.key());
        widget_item->setText(TreeCol::Version, it.value().version);
        widget_item->setText(TreeCol::Description, it.value().description);
        widget_item->setData(0, Qt::UserRole, true); // All items are displayed till filtered
    }

    // Process the entire list of apps and count upgradable and installable
    int upgr_count = 0;
    int inst_count = 0;

    auto hashInstalled = listInstalledVersions();
    // Update tree
    for (QTreeWidgetItemIterator it(newtree); (*it) != nullptr; ++it) {
        const QString app_name = (*it)->text(TreeCol::Name);
        if (ui->checkHideLibs->isChecked() && isFilteredName(app_name)) {
            (*it)->setHidden(true);
        }
        const QString app_ver = (*it)->text(TreeCol::Version);
        const VersionNumber installed = hashInstalled.value(app_name);
        // Candidate from the selected repo, might be different than the one from Enabled
        const VersionNumber repo_candidate(app_ver);

        (*it)->setIcon(TreeCol::Check, QIcon()); // Reset update icon
        if (installed.toString().isEmpty()) {
            for (int i = 0; i < newtree->columnCount(); ++i) {
                if (enabled_list.contains(app_name)) {
                    (*it)->setToolTip(i, tr("Version ") + enabled_list.value(app_name).version
                                             + tr(" in the enabled repos"));
                } else {
                    (*it)->setToolTip(i, tr("Not available in the enabled repos"));
                }
            }
            (*it)->setData(TreeCol::Status, Qt::UserRole, Status::NotInstalled);
        } else {
            ++inst_count;
            if (installed >= repo_candidate) {
                (*it)->setIcon(TreeCol::Check, qicon_installed);
                for (int i = 0; i < newtree->columnCount(); ++i) {
                    (*it)->setToolTip(i, tr("Latest version ") + installed.toString() + tr(" already installed"));
                }
                (*it)->setData(TreeCol::Status, Qt::UserRole, Status::Installed);
            } else {
                (*it)->setIcon(TreeCol::Check, qicon_upgradable);
                for (int i = 0; i < newtree->columnCount(); ++i) {
                    (*it)->setToolTip(i, tr("Version ") + installed.toString() + tr(" installed"));
                }
                ++upgr_count;
                (*it)->setData(TreeCol::Status, Qt::UserRole, Status::Upgradable);
            }
        }
    }
    for (uchar i = 0; i < newtree->columnCount(); ++i) {
        newtree->resizeColumnToContents(i);
    }
    if (newtree == ui->treeEnabled) {
        ui->pushRemoveOrphan->setVisible(
            Cmd().run(R"lit(test -n "$(apt-get --dry-run autoremove |grep -Po '^Remv \K[^ ]+' )")lit"));
    }
    updateInterface();
    newtree->blockSignals(false);
    displayPackagesIsRunning = false;
}

void MainWindow::displayFlatpaks(bool force_update)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    displayFlatpaksIsRunning = true;
    lastItemClicked = nullptr;
    if (flatpaks.isEmpty() || force_update) {
        if (ui->tabWidget->currentIndex() == Tab::Flatpak) {
            setCursor(QCursor(Qt::BusyCursor));
        }
        listFlatpakRemotes();
        ui->treeFlatpak->blockSignals(true);
        ui->treeFlatpak->clear();
        change_list.clear();

        if (ui->tabWidget->currentIndex() == Tab::Flatpak) {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
            }
        }
        blockInterfaceFP(true);
        flatpaks = listFlatpaks(ui->comboRemote->currentText());
        flatpaks_apps.clear();
        flatpaks_runtimes.clear();

        // List installed packages
        installed_apps_fp = listInstalledFlatpaks("--app");

        // Add runtimes (needed for older flatpak versions)
        installed_runtimes_fp = listInstalledFlatpaks("--runtime");
        uint total_count = 0;
        QTreeWidgetItem *widget_item {nullptr};
        QString version;
        QString size;
        for (QString item : qAsConst(flatpaks)) {
            if (fp_ver < VersionNumber("1.2.4")) {
                size = item.section('\t', 1, 1);
                item = item.section('\t', 0, 0); // strip size
                version = item.section('/', -1);
            } else { // Buster and higher versions
                size = item.section('\t', -1);
                version = item.section('\t', 0, 0);
                item = item.section('\t', 1, 1).section('/', 1);
            }
            if (version.isEmpty()) {
                version = item.section('/', -1);
            }
            QString long_name = item.section('/', 0, 0);
            QString short_name = long_name.section('.', -1);
            if (short_name == QLatin1String("Locale") || short_name == QLatin1String("Sources")
                || short_name == QLatin1String("Debug")) { // Skip Locale, Sources, Debug
                continue;
            }
            ++total_count;
            widget_item = new QTreeWidgetItem(ui->treeFlatpak);
            widget_item->setCheckState(FlatCol::Check, Qt::Unchecked);
            widget_item->setText(FlatCol::Name, short_name);
            widget_item->setText(FlatCol::LongName, long_name);
            widget_item->setText(FlatCol::Version, version);
            widget_item->setText(FlatCol::Size, size);
            widget_item->setData(FlatCol::FullName, Qt::UserRole, item); // Full string
            QStringList installed_all {installed_apps_fp + installed_runtimes_fp};
            if (installed_all.contains(item)) {
                widget_item->setIcon(FlatCol::Check, QIcon::fromTheme("package-installed-updated",
                                                                      QIcon(":/icons/package-installed-updated.png")));
                widget_item->setData(FlatCol::Status, Qt::UserRole, Status::Installed);
            } else {
                widget_item->setData(FlatCol::Status, Qt::UserRole, Status::NotInstalled);
            }
            widget_item->setData(0, Qt::UserRole, true); // all items are displayed till filtered
        }
        // Add sizes for the installed packages for older flatpak that doesn't list size for all the packages
        listSizeInstalledFP();
        ui->labelNumAppFP->setText(QString::number(total_count));

        uint total = 0;
        if (installed_apps_fp != QStringList(QLatin1String(""))) {
            total = installed_apps_fp.count();
        }
        ui->labelNumInstFP->setText(QString::number(total));
        ui->treeFlatpak->sortByColumn(FlatCol::Name, Qt::AscendingOrder);
        removeDuplicatesFP();
        for (uchar i = 0; i < ui->treeFlatpak->columnCount(); ++i) {
            ui->treeFlatpak->resizeColumnToContents(i);
        }
    }
    ui->treeFlatpak->blockSignals(false);
    if (ui->tabWidget->currentIndex() == Tab::Flatpak && !ui->comboFilterFlatpak->currentText().isEmpty()) {
        filterChanged(ui->comboFilterFlatpak->currentText());
    }
    if (ui->tabWidget->currentIndex() == Tab::Flatpak) {
        ui->searchBoxFlatpak->setFocus();
    }
    displayFlatpaksIsRunning = false;
    firstRunFP = false;
    blockInterfaceFP(false);
}

void MainWindow::displayWarning(const QString &repo)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    bool *displayed = nullptr;
    QString msg;
    QString key;

    if (repo == "test") {
        displayed = &warning_test;
        key = "NoWarningTest";
        msg = tr("You are about to use the MX Test repository, whose packages are provided for "
                 "testing purposes only. It is possible that they might break your system, so it "
                 "is suggested that you back up your system and install or update only one package "
                 "at a time. Please provide feedback in the Forum so the package can be evaluated "
                 "before moving up to Main.");

    } else if (repo == "backports") {
        displayed = &warning_backports;
        key = "NoWarningBackports";
        msg = tr("You are about to use Debian Backports, which contains packages taken from the next "
                 "Debian release (called 'testing'), adjusted and recompiled for usage on Debian stable. "
                 "They cannot be tested as extensively as in the stable releases of Debian and MX Linux, "
                 "and are provided on an as-is basis, with risk of incompatibilities with other components "
                 "in Debian stable. Use with care!");
    } else if (repo == "flatpaks") {
        displayed = &warning_flatpaks;
        key = "NoWarningFlatpaks";
        msg = tr("MX Linux includes this repository of flatpaks for the users' convenience only, and "
                 "is not responsible for the functionality of the individual flatpaks themselves. "
                 "For more, consult flatpaks in the Wiki.");
    }
    if ((displayed == nullptr) || *displayed || settings.value(key, false).toBool()) {
        return;
    }

    QMessageBox msgBox(QMessageBox::Warning, tr("Warning"), msg);
    msgBox.addButton(QMessageBox::Close);
    auto *cb = new QCheckBox();
    msgBox.setCheckBox(cb);
    cb->setText(tr("Do not show this message again"));
    connect(cb, &QCheckBox::clicked, this, [this, key, cb] { settings.setValue(key, cb->isChecked()); });
    msgBox.exec();
    *displayed = true;
}

void MainWindow::ifDownloadFailed() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    progress->hide();
    ui->tabWidget->setCurrentWidget(ui->tabPopular);
}

void MainWindow::listFlatpakRemotes() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QString currentRemote = ui->comboRemote->currentText();
    ui->comboRemote->blockSignals(true);
    ui->comboRemote->clear();
    QStringList list = Cmd().getOut("flatpak remote-list " + FPuser + "| cut -f1").remove(' ').split('\n');
    ui->comboRemote->addItems(list);
    ui->comboRemote->setCurrentText(currentRemote.isEmpty() ? "flathub" : currentRemote);
    ui->comboRemote->blockSignals(false);
}

bool MainWindow::confirmActions(const QString &names, const QString &action)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    qDebug() << "names" << names << "and" << change_list;
    QString msg;

    QString detailed_names;
    QStringList detailed_installed_names;
    QString detailed_to_install;
    QString detailed_removed_names;
    QString recommends;
    QString recommends_aptitude;
    QString aptitude_info;

    const QString frontend {
        "DEBIAN_FRONTEND=$(dpkg -l debconf-kde-helper 2>/dev/null | grep -sq ^i && echo kde || echo gnome) LANG=C "};
    const QString aptget {"apt-get -s -V -o=Dpkg::Use-Pty=0 "};
    const QString aptitude {"aptitude -sy -V -o=Dpkg::Use-Pty=0 "};
    if (currentTree == ui->treeFlatpak && names != "flatpak") {
        detailed_installed_names = change_list;
    } else if (currentTree == ui->treeBackports) {
        recommends = (ui->checkBoxInstallRecommendsMXBP->isChecked()) ? "--install-recommends " : "";
        recommends_aptitude
            = (ui->checkBoxInstallRecommendsMXBP->isChecked()) ? "--with-recommends " : "--without-recommends ";
        detailed_names = cmd.getOutAsRoot(
            frontend + aptget + action + ' ' + recommends + "-t " + ver_name + "-backports --reinstall " + names
            + R"lit(|grep 'Inst\|Remv' | awk '{V=""; P="";}; $3 ~ /^\[/ { V=$3 }; $3 ~ /^\(/ { P=$3 ")"}; $4 ~ /^\(/ {P=" => " $4 ")"};  {print $2 ";" V  P ";" $1}')lit");
        aptitude_info = cmd.getOutAsRoot(frontend + aptitude + action + ' ' + recommends_aptitude + "-t " + ver_name
                                         + "-backports " + names + " |tail -2 |head -1");
    } else if (currentTree == ui->treeMXtest) {
        recommends = (ui->checkBoxInstallRecommendsMX->isChecked()) ? "--install-recommends " : "";
        recommends_aptitude
            = (ui->checkBoxInstallRecommendsMX->isChecked()) ? "--with-recommends " : "--without-recommends ";
        detailed_names = cmd.getOutAsRoot(
            frontend + aptget + action + " -t mx " + recommends + "--reinstall " + names
            + R"lit(|grep 'Inst\|Remv' | awk '{V=""; P="";}; $3 ~ /^\[/ { V=$3 }; $3 ~ /^\(/ { P=$3 ")"}; $4 ~ /^\(/ {P=" => " $4 ")"};  {print $2 ";" V  P ";" $1}')lit");
        aptitude_info = cmd.getOutAsRoot(frontend + aptitude + action + " -t mx " + recommends_aptitude + names
                                         + " |tail -2 |head -1");
    } else {
        recommends = (ui->checkBoxInstallRecommends->isChecked()) ? "--install-recommends " : "";
        recommends_aptitude
            = (ui->checkBoxInstallRecommends->isChecked()) ? "--with-recommends " : "--without-recommends ";
        detailed_names = cmd.getOutAsRoot(
            frontend + aptget + action + ' ' + recommends + "--reinstall " + names
            + R"lit(|grep 'Inst\|Remv'| awk '{V=""; P="";}; $3 ~ /^\[/ { V=$3 }; $3 ~ /^\(/ { P=$3 ")"}; $4 ~ /^\(/ {P=" => " $4 ")"};  {print $2 ";" V  P ";" $1}')lit");
        aptitude_info
            = cmd.getOutAsRoot(frontend + aptitude + action + ' ' + recommends_aptitude + names + " |tail -2 |head -1");
    }

    if (currentTree != ui->treeFlatpak) {
        detailed_installed_names = detailed_names.split('\n');
    }

    detailed_installed_names.sort();
    qDebug() << "detailed installed names sorted " << detailed_installed_names;
    QStringListIterator iterator(detailed_installed_names);

    if (currentTree != ui->treeFlatpak) {
        while (iterator.hasNext()) {
            QString value = iterator.next();
            if (value.contains(QLatin1String("Remv"))) {
                value = value.section(';', 0, 0) + ' ' + value.section(';', 1, 1);
                detailed_removed_names = detailed_removed_names + value + '\n';
            }
            if (value.contains(QLatin1String("Inst"))) {
                value = value.section(';', 0, 0) + ' ' + value.section(';', 1, 1);
                detailed_to_install = detailed_to_install + value + '\n';
            }
        }
        if (!detailed_removed_names.isEmpty()) {
            detailed_removed_names.prepend(tr("Remove") + '\n');
        }
        if (!detailed_to_install.isEmpty()) {
            detailed_to_install.prepend(tr("Install") + '\n');
        }
    } else {
        if (action == QLatin1String("remove")) {
            detailed_removed_names = change_list.join('\n');
            detailed_to_install.clear();
        }
        if (action == QLatin1String("install")) {
            detailed_to_install = change_list.join('\n');
            detailed_removed_names.clear();
        }
    }

    msg = "<b>" + tr("The following packages were selected. Click Show Details for list of changes.") + "</b>";

    QMessageBox msgBox;
    msgBox.setText(msg);
    msgBox.setInformativeText('\n' + names + "\n\n" + aptitude_info);

    if (action == QLatin1String("install")) {
        msgBox.setDetailedText(detailed_to_install + '\n' + detailed_removed_names);
    } else {
        msgBox.setDetailedText(detailed_removed_names + '\n' + detailed_to_install);
    }

    // find Detailed Info box and set heigth, set box height between 100 - 400 depending on length of content
    const auto min = 100;
    const auto max = 400;
    auto *const detailedInfo = msgBox.findChild<QTextEdit *>();
    const auto recommended = qMax(msgBox.detailedText().length() / 2, min); // half of length is just guesswork
    const auto height = qMin(recommended, max);
    detailedInfo->setFixedHeight(height);

    msgBox.addButton(QMessageBox::Ok);
    msgBox.addButton(QMessageBox::Cancel);

    const auto width = 600;
    auto *horizontalSpacer = new QSpacerItem(width, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    auto *layout = qobject_cast<QGridLayout *>(msgBox.layout());
    layout->addItem(horizontalSpacer, 0, 1);
    return msgBox.exec() == QMessageBox::Ok;
}

bool MainWindow::install(const QString &names)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    if (!isOnline()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Installing packages..."));

    // Simulate install of selections and present for confirmation
    // if user selects cancel, break routine but return success to avoid error message
    if (!confirmActions(names, "install")) {
        return true;
    }
    enableOutput();
    QString frontend {
        "DEBIAN_FRONTEND=$(dpkg -l debconf-kde-helper 2>/dev/null | grep -sq ^i && echo kde || echo gnome) "};
    QString aptget {"apt-get -o=Dpkg::Use-Pty=0 install -y "};

    if (lock_file->isLockedGUI()) {
        return false;
    }
    QString recommends;
    bool success = false;
    if (currentTree == ui->treeBackports) {
        recommends = (ui->checkBoxInstallRecommendsMXBP->isChecked()) ? "--install-recommends " : "";
        success = cmd.runAsRoot(frontend + aptget + recommends + "-t " + ver_name + "-backports --reinstall " + names);
    } else if (currentTree == ui->treeMXtest) {
        recommends = (ui->checkBoxInstallRecommendsMX->isChecked()) ? "--install-recommends " : "";
        success = cmd.runAsRoot(frontend + aptget + recommends + " -t mx " + names);
    } else {
        recommends = (ui->checkBoxInstallRecommends->isChecked()) ? "--install-recommends " : "";
        success = cmd.runAsRoot(frontend + aptget + recommends + "--reinstall " + names);
    }
    return success;
}

// Install a list of application and run postprocess for each of them.
bool MainWindow::installBatch(const QStringList &name_list)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    bool result = true;
    QString postinstall;
    QString install_names;

    for (const QString &name : name_list) {
        for (const auto &item : qAsConst(popular_apps)) {
            if (item.name == name) {
                postinstall += item.postInstall + '\n';
                install_names += item.installNames + ' ';
            }
        }
    }

    if (!install_names.isEmpty()) {
        if (!install(install_names)) {
            result = false;
        }
    }

    if (postinstall != '\n') {
        qDebug() << "Post-install";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Post-processing..."));
        if (lock_file->isLockedGUI()) {
            return false;
        }
        enableOutput();
        if (!cmd.runAsRoot(postinstall)) {
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
    QString install_names;

    // Get all the app info
    for (const auto &item : qAsConst(popular_apps)) {
        if (item.name == name) {
            preinstall = item.preInstall;
            postinstall = item.postInstall;
            install_names = item.installNames;
        }
    }
    enableOutput();
    // Preinstall
    if (!preinstall.isEmpty()) {
        qDebug() << "Pre-install";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Pre-processing for ") + name);
        if (lock_file->isLockedGUI()) {
            return false;
        }
        if (!cmd.runAsRoot(preinstall)) {
            if (QFile::exists(temp_list)) {
                QString elevate {QFile::exists("/usr/bin/pkexec") ? "/usr/bin/pkexec" : "/usr/bin/gksu"};
                Cmd().run(elevate + " /usr/lib/mx-packageinstaller/mxpi-lib cleanup_temp", true);
                updateApt();
            }
            return false;
        }
    }
    // Install
    if (!install_names.isEmpty()) {
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Installing ") + name);
        result = install(install_names);
    }
    enableOutput();
    // Postinstall
    if (!postinstall.isEmpty()) {
        qDebug() << "Post-install";
        ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Post-processing for ") + name);
        if (lock_file->isLockedGUI()) {
            return false;
        }
        cmd.runAsRoot(postinstall);
    }
    if (QFile::exists(temp_list)) {
        QString elevate {QFile::exists("/usr/bin/pkexec") ? "/usr/bin/pkexec" : "/usr/bin/gksu"};
        Cmd().run(elevate + " /usr/lib/mx-packageinstaller/mxpi-lib cleanup_temp", true);
        updateApt();
    }
    return result;
}

bool MainWindow::installPopularApps()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QStringList batch_names;
    bool result = true;

    if (!isOnline()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    if (!updated_once) {
        updateApt();
    }

    // Make a list of apps to be installed together
    for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
        if ((*it)->checkState(PopCol::Check) == Qt::Checked) {
            QString name = (*it)->text(2);
            for (const auto &item : qAsConst(popular_apps)) {
                if (item.name == name) {
                    const QString &preinstall = item.preInstall;
                    if (preinstall.isEmpty()) { // Add to batch processing if there is no preinstall command
                        batch_names << name;
                        (*it)->setCheckState(PopCol::Check, Qt::Unchecked);
                    }
                }
            }
        }
    }
    if (!installBatch(batch_names)) {
        result = false;
    }

    // Install the rest of the apps
    for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
        if ((*it)->checkState(PopCol::Check) == Qt::Checked) {
            if (!installPopularApp((*it)->text(PopCol::Name))) {
                result = false;
            }
        }
    }
    setCursor(QCursor(Qt::ArrowCursor));
    return result;
}

bool MainWindow::installSelected()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), true);
    QString names = change_list.join(' ');

    // Change sources as needed
    if (currentTree == ui->treeMXtest) {
        // Add testrepo unless already enabled
        if (!test_initially_enabled) {
            QString suite = ver_name;
            if (ver_name == "jessie") { // use 'mx15' for Stretch based MX, user
                                        // version name for newer versions
                suite = "mx15";
            }
            if (arch == "amd64") {
                cmd.runAsRoot("apt-get update --print-uris | tac | "
                              "grep -m1 -oE 'https?://.*/mx/repo/dists/"
                              + suite + "/main' | sed 's:^:deb :; s:/repo/dists/:/testrepo :; s:/main: test:' > "
                              + temp_list);
            } else {
                cmd.runAsRoot("apt-get update --print-uris | tac | "
                              "grep -m1 -oE 'https?://.*/mx/repo/dists/"
                              + suite
                              + "/main' | sed 's:^:deb [arch='$(dpkg --print-architecture)'] :; "
                                "s:/repo/dists/:/testrepo :; s:/main: test:' > "
                              + temp_list);
            }
        }
        updateApt();
    } else if (currentTree == ui->treeBackports) {
        cmd.runAsRoot("echo deb http://ftp.debian.org/debian " + ver_name + "-backports main contrib non-free > "
                      + temp_list);
        updateApt();
    }
    bool result = install(names);
    if (currentTree == ui->treeBackports || currentTree == ui->treeMXtest) {
        if (QFile::exists(temp_list)) {
            QString elevate {QFile::exists("/usr/bin/pkexec") ? "/usr/bin/pkexec" : "/usr/bin/gksu"};
            Cmd().run(elevate + " /usr/lib/mx-packageinstaller/mxpi-lib cleanup_temp", true);
            updateApt();
        }
    }
    change_list.clear();
    installed_packages = listInstalled();
    return result;
}

bool MainWindow::isFilteredName(const QString &name)
{
    static const QRegularExpression filterRegex(R"((^lib(?!reoffice|rewolf))|(-dev$)|(-dbg$)|(-dbgsym$)|(-libs$))",
                                                QRegularExpression::CaseInsensitiveOption);

    return filterRegex.match(name).hasMatch();
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

    auto error = QNetworkReply::NoError;
    for (const QString address : {"https://mxrepo.com", "https://google.com"}) {
        error = QNetworkReply::NoError; // reset for each tried address
        QNetworkProxyQuery query {QUrl(address)};
        QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        if (!proxies.isEmpty()) {
            manager.setProxy(proxies.first());
        }
        request.setUrl(QUrl(address));
        reply = manager.head(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(reply, &QNetworkReply::errorOccurred, [&error](QNetworkReply::NetworkError err) { error = err; });
        connect(reply, &QNetworkReply::errorOccurred, &loop, &QEventLoop::quit);
        auto timeout = settings.value("timeout", 7000).toUInt();
        QTimer::singleShot(timeout, &loop, [&loop, &error] {
            error = QNetworkReply::TimeoutError;
            loop.quit();
        }); // manager.setTransferTimeout(time) // only in Qt >= 5.15
        loop.exec();
        reply->disconnect();
        if (error == QNetworkReply::NoError) {
            return true;
        }
    }
    qDebug() << "No network detected:" << reply->url() << error;
    return false;
}

bool MainWindow::downloadFile(const QString &url, QFile &file)
{
    qDebug() << "... downloading: " << url;
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Could not open file:" << file.fileName();
        return false;
    }

    QNetworkRequest request;
    request.setUrl(QUrl(url));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", QApplication::applicationName().toUtf8() + '/'
                                           + QApplication::applicationVersion().toUtf8() + " (linux-gnu)");

    reply = manager.get(request);
    QEventLoop loop;

    bool success = true;
    connect(reply, &QNetworkReply::readyRead, this,
            [this, &file, &success] { success = (file.write(reply->readAll()) != 0); });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->disconnect();

    if (!success) {
        QMessageBox::warning(this, tr("Error"),
                             tr("There was an error writing file: %1. Please check if you have "
                                "enough free space on your drive")
                                 .arg(file.fileName()));
        exit(EXIT_FAILURE);
    }

    file.close();
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "There was an error downloading the file:" << url;
        return false;
    }
    return true;
}

bool MainWindow::downloadAndUnzip(const QString &url, QFile &file)
{
    if (!downloadFile(url, file)) {
        file.remove();
        QFile::remove(QFileInfo(file.fileName()).path() + '/'
                      + QFileInfo(file.fileName()).baseName()); // rm unzipped file
        return false;
    } else {
        QString unzip = (QFileInfo(file).suffix() == "gz") ? "gunzip -f " : "unxz -f ";

        if (!cmd.run(unzip + file.fileName())) {
            qDebug() << "Could not unzip file:" << file.fileName();
            return false;
        }
    }
    return true;
}

bool MainWindow::downloadAndUnzip(const QString &url, const QString &repo_name, const QString &branch,
                                  const QString &format, QFile &file)
{
    return downloadAndUnzip(url + repo_name + branch + "/binary-" + arch + "/Packages." + format, file);
}

bool MainWindow::buildPackageLists(bool force_download)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (force_download) {
        setDirty();
    }
    clearUi();
    if (!downloadPackageList(force_download)) {
        ifDownloadFailed();
        return false;
    }
    if (!readPackageList(force_download)) {
        ifDownloadFailed();
        return false;
    }
    displayPackages();
    return true;
}

// Download the Packages.gz from sources
bool MainWindow::downloadPackageList(bool force_download)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (!isOnline()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Internet is not available, won't be able to download the list of packages"));
        return false;
    }
    if (!tmp_dir.isValid()) {
        qDebug() << "Can't create temp folder";
        return false;
    }
    QDir::setCurrent(tmp_dir.path());
    progress->setLabelText(tr("Downloading package info..."));
    pushCancel->setEnabled(true);

    if (enabled_list.isEmpty() || force_download) {
        if (force_download) {
            if (!updateApt()) {
                return false;
            }
        }
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
        AptCache cache;
        enabled_list = cache.getCandidates();
        if (enabled_list.isEmpty()) {
            updateApt();
            AptCache cache2;
            enabled_list = cache2.getCandidates();
        }
    }

    if (currentTree == ui->treeMXtest) {
        if (!QFile::exists(tmp_dir.path() + "/mxPackages") || force_download) {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
            }

            QFile file(tmp_dir.path() + "/mxPackages.gz");
            QString url {"http://mxrepo.com/mx/testrepo/dists/"};
            if (!cmd.run("apt-get update --print-uris | tac | grep -m1 -oP 'https?://.*/mx/testrepo/dists/(?="
                         + ver_name + "/test/)'")) {
                cmd.run("apt-get update --print-uris | tac | grep -m1 -oE 'https?://.*/mx/repo/dists/" + ver_name
                        + "/main/' | sed -e 's:/mx/repo/dists/" + ver_name
                        + "/main/:/mx/testrepo/dists/:' | grep -oE 'https?://.*/mx/testrepo/dists/'");
            }
            url = cmd.readAllOutput();
            QString branch {"/test"};
            QString format {"gz"};
            if (!downloadAndUnzip(url, ver_name, branch, format, file)) {
                return false;
            }
        }
    } else if (currentTree == ui->treeBackports) {
        if (!QFile::exists(tmp_dir.path() + "/mainPackages") || !QFile::exists(tmp_dir.path() + "/contribPackages")
            || !QFile::exists(tmp_dir.path() + "/nonfreePackages") || force_download) {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
            }

            QFile file(tmp_dir.path() + "/mainPackages.xz");
            QString url = "http://deb.debian.org/debian/dists/";
            QString branch {"-backports/main"};
            QString format {"xz"};
            if (!downloadAndUnzip(url, ver_name, branch, format, file)) {
                return false;
            }

            file.setFileName(tmp_dir.path() + "/contribPackages.xz");
            branch = "-backports/contrib";
            if (!downloadAndUnzip(url, ver_name, branch, format, file)) {
                return false;
            }

            file.setFileName(tmp_dir.path() + "/nonfreePackages.xz");
            branch = "-backports/non-free";
            if (!downloadAndUnzip(url, ver_name, branch, format, file)) {
                return false;
            }

            pushCancel->setDisabled(true);
            QFile outputFile("allPackages");
            if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                qWarning() << "Could not open: " << outputFile.fileName();
            }

            QTextStream outStream(&outputFile);
            QFile inputFile1("mainPackages");
            QFile inputFile2("contribPackages");
            QFile inputFile3("nonfreePackages");

            if (!inputFile1.open(QIODevice::ReadOnly | QIODevice::Text)
                || !inputFile2.open(QIODevice::ReadOnly | QIODevice::Text)
                || !inputFile3.open(QIODevice::ReadOnly | QIODevice::Text)) {
                qWarning() << "Could not read file";
            }

            outStream << inputFile1.readAll();
            outStream << inputFile2.readAll();
            outStream << inputFile3.readAll();
            outputFile.close();
        }
    }
    return true;
}

void MainWindow::enableTabs(bool enable) const
{
    for (uchar tab = 0; tab < ui->tabWidget->count() - 1; ++tab) { // Enable all except last (Console)
        ui->tabWidget->setTabEnabled(tab, enable);
    }
    if (arch == "i386") {
        ui->tabWidget->setTabEnabled(Tab::Flatpak, false);
    }
}

void MainWindow::hideColumns() const
{
    ui->tabWidget->setCurrentIndex(Tab::Popular);
    ui->treeEnabled->hideColumn(TreeCol::Status); // Status of the package: installed, upgradable, etc
    ui->treeMXtest->hideColumn(TreeCol::Status);
    ui->treeBackports->hideColumn(TreeCol::Status);
    ui->treeFlatpak->hideColumn(FlatCol::Status);
    ui->treeFlatpak->hideColumn(FlatCol::Duplicate);
    ui->treeFlatpak->hideColumn(FlatCol::FullName);
}

void MainWindow::hideLibs() const
{
    if (currentTree != ui->treeFlatpak && ui->checkHideLibs->isChecked()) {
        for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
            if (isFilteredName((*it)->text(TreeCol::Name))) {
                (*it)->setHidden(true);
            }
        }
    }
}

// Process downloaded *Packages.gz files
bool MainWindow::readPackageList(bool force_download)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    pushCancel->setDisabled(true);

    // Early return if lists are already populated and not forced to download
    if (!force_download
        && ((currentTree == ui->treeEnabled && !enabled_list.isEmpty())
            || (currentTree == ui->treeMXtest && !mx_list.isEmpty())
            || (currentTree == ui->treeBackports && !backports_list.isEmpty()))) {
        return true;
    }

    // Determine the file name based on the current tree
    QString fileName;
    if (currentTree == ui->treeMXtest) {
        fileName = tmp_dir.path() + "/mxPackages";
    } else if (currentTree == ui->treeBackports) {
        fileName = tmp_dir.path() + "/allPackages";
    } else if (currentTree == ui->treeEnabled) {
        // treeEnabled is updated at downloadPackageList
        return true;
    }

    // Read the content of the file
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly)) {
        qDebug() << "Could not open file: " << file.fileName();
        return false;
    }
    QString file_content = file.readAll();
    file.close();

    // Select the appropriate list to populate
    QMap<QString, PackageInfo> *packageMap = (currentTree == ui->treeMXtest) ? &mx_list : &backports_list;
    packageMap->clear();

    // Parse the file content and populate the map
    QStringList lines = file_content.split('\n');
    QString package, version, description;
    for (const QString &line : lines) {
        if (line.startsWith("Package: ")) {
            package = line.section(' ', 1);
        } else if (line.startsWith("Version: ")) {
            version = line.section(' ', 1);
        } else if (line.startsWith("Description: ")) {
            description = line.section(' ', 1);
            packageMap->insert(package, {version, description});
            package.clear();
            version.clear();
            description.clear();
        }
    }
    return true;
}

void MainWindow::cancelDownload()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    cmd.terminate();
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
    ui->pushCancel->setEnabled(true);
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);

    if (currentTree == ui->treeEnabled) {
        ui->labelNumApps->clear();
        ui->labelNumInst->clear();
        ui->labelNumUpgr->clear();
        ui->treeEnabled->clear();
        ui->pushUpgradeAll->setHidden(true);
    } else if (currentTree == ui->treeMXtest) {
        ui->labelNumApps_2->clear();
        ui->labelNumInstMX->clear();
        ui->labelNumUpgrMX->clear();
        ui->treeMXtest->clear();
    } else if (currentTree == ui->treeBackports) {
        ui->labelNumApps_3->clear();
        ui->labelNumInstBP->clear();
        ui->labelNumUpgrBP->clear();
        ui->treeBackports->clear();
    }
    ui->comboFilterBP->setCurrentIndex(0);
    ui->comboFilterFlatpak->setCurrentIndex(0);
    ui->comboFilterMX->setCurrentIndex(0);
    ui->comboFilterEnabled->setCurrentIndex(0);
    blockSignals(false);
}

void MainWindow::cleanup()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (cmd.state() != QProcess::NotRunning) {
        qDebug() << "Command" << cmd.program() << cmd.arguments() << "terminated" << cmd.terminateAndKill();
    }
    QString elevate {QFile::exists("/usr/bin/pkexec") ? "/usr/bin/pkexec" : "/usr/bin/gksu"};
    if (QFile::exists(temp_list)) {
        Cmd().run(elevate + " /usr/lib/mx-packageinstaller/mxpi-lib cleanup_temp", true);
        Cmd().runAsRoot("apt-get update&");
    }
    Cmd().run(elevate + " /usr/lib/mx-packageinstaller/mxpi-lib copy_log", true);
    settings.setValue("geometry", saveGeometry());
}

QString MainWindow::getVersion(const QString &name) const
{
    return Cmd().getOut("dpkg-query -f '${Version}' -W " + name);
}

// Return true if all the packages listed are installed
bool MainWindow::checkInstalled(const QString &names) const
{
    if (names.isEmpty()) {
        return false;
    }

    const auto names_list = names.split('\n');
    return std::all_of(names_list.cbegin(), names_list.cend(),
                       [&](const QString &name) { return installed_packages.contains(name.trimmed()); });
}

// Return true if all the packages in the list are installed
bool MainWindow::checkInstalled(const QStringList &name_list) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (name_list.isEmpty()) {
        return false;
    }

    for (const QString &name : name_list) {
        if (!installed_packages.contains(name)) {
            return false;
        }
    }
    return true;
}

// Return true if all the items in the list are upgradable
bool MainWindow::checkUpgradable(const QStringList &name_list) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (name_list.isEmpty()) {
        return false;
    }
    for (const QString &name : name_list) {
        auto item_list = currentTree->findItems(name, Qt::MatchExactly, TreeCol::Name);
        if (item_list.isEmpty() || item_list.at(0)->data(TreeCol::Status, Qt::UserRole) != Status::Upgradable) {
            return false;
        }
    }
    return true;
}

QStringList MainWindow::listInstalled() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    Cmd shell;
    QString str = shell.getOut("dpkg --get-selections | grep -v deinstall | cut -f1", true);
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        QMessageBox::critical(
            nullptr, tr("Error"),
            tr("dpkg command returned an error, please run 'dpkg --list' in terminal and check the output."));
        exit(EXIT_FAILURE);
    }
    str.remove(":i386");
    str.remove(":amd64");
    str.remove(":arm64");
    str.remove(":armhf");
    return str.split('\n');
}

QStringList MainWindow::listFlatpaks(const QString &remote, const QString &type) const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    static bool updated = false;

    // Need to specify arch for older version (flatpak takes different format than dpkg)
    QString arch_fp = getArchOption();
    if (arch_fp.isEmpty()) {
        return {};
    }

    bool success = false;
    QStringList list;
    Cmd shell;
    if (fp_ver < VersionNumber("1.0.1")) {
        // List packages, strip first part remote/ or app/ no size for old flatpak
        success = shell.run("flatpak -d remote-ls " + FPuser + remote + ' ' + arch_fp + type
                            + R"( 2>/dev/null| cut -f1 | tr -s ' ' | cut -f1 -d' '|sed 's/^[^\/]*\///g' ")");
        list = shell.readAllOutput().split('\n');
    } else if (fp_ver < VersionNumber("1.2.4")) { // lower than Buster version
        // List size too
        success = shell.run("flatpak -d remote-ls " + FPuser + remote + ' ' + arch_fp + type
                            + R"( 2>/dev/null| cut -f1,3 |tr -s ' ' | sed 's/^[^\/]*\///g' ")");
        list = shell.readAllOutput().split('\n');
    } else { // Buster version and above
        if (!updated) {
            success = shell.run("flatpak update --appstream");
            updated = true;
        }
        // List version too, unfortunatelly the resulting string structure is different depending on type option
        if (type == QLatin1String("--app") || type.isEmpty()) {
            success = shell.run("flatpak remote-ls " + FPuser + remote + ' ' + arch_fp
                                + "--app --columns=ver,ref,installed-size 2>/dev/null");
            list = shell.readAllOutput().split('\n');
            if (list == QStringList("")) {
                list = QStringList();
            }
        }
        if (type == QLatin1String("--runtime") || type.isEmpty()) {
            success = shell.run("flatpak remote-ls " + FPuser + remote + ' ' + arch_fp
                                + "--runtime --columns=branch,ref,installed-size 2>/dev/null");
            list += shell.readAllOutput().split('\n');
            if (list == QStringList("")) {
                list = QStringList();
            }
        }
    }
    if (!success || list == QStringList("")) {
        qDebug() << QString("Could not list packages from %1 remote, or remote doesn't contain packages").arg(remote);
        return {};
    }
    return list;
}

// List installed flatpaks by type: apps, runtimes, or all (if no type is provided)
QStringList MainWindow::listInstalledFlatpaks(const QString &type)
{
    QStringList list;
    if (fp_ver < VersionNumber("1.2.4")) {
        list << cmd.getOut("flatpak -d list " + FPuser + "2>/dev/null " + type + "|cut -f1|cut -f1 -d' '")
                    .remove(' ')
                    .split('\n');
    } else {
        list << cmd.getOut("flatpak list " + FPuser + "2>/dev/null " + type + " --columns=ref").remove(' ').split('\n');
    }
    if (list == QStringList("")) {
        return {};
    }
    return list;
}

void MainWindow::setCurrentTree()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    const QList list({ui->treePopularApps, ui->treeEnabled, ui->treeMXtest, ui->treeBackports, ui->treeFlatpak});

    auto it = std::find_if(list.cbegin(), list.cend(), [](const auto &item) { return item->isVisible(); });
    if (it != list.end()) {
        currentTree = *it;
        updateInterface();
        return;
    }
}

void MainWindow::setDirty()
{
    dirtyBackports = dirtyEnabledRepos = dirtyTest = true;
}

void MainWindow::setIcons()
{

    const QString icon_upgradable {"package-installed-outdated"};
    const QString icon_installed {"package-installed-updated"};

    const QIcon backup_icon_upgradable(":/icons/package-installed-outdated.png");
    const QIcon backup_icon_installed(":/icons/package-installed-updated.png");

    const QIcon theme_icon_upgradable = QIcon::fromTheme(icon_upgradable, backup_icon_upgradable);
    const QIcon theme_icon_installed = QIcon::fromTheme(icon_installed, backup_icon_installed);

    const bool force_backup_icon = (theme_icon_upgradable.name() == theme_icon_installed.name());

    qicon_installed = force_backup_icon ? backup_icon_installed : theme_icon_installed;
    qicon_upgradable = force_backup_icon ? backup_icon_upgradable : theme_icon_upgradable;

    ui->iconUpgradable->setIcon(qicon_upgradable);
    ui->iconUpgradable_2->setIcon(ui->iconUpgradable->icon());
    ui->iconUpgradable_3->setIcon(ui->iconUpgradable->icon());
    ui->iconInstalledPackages->setIcon(qicon_installed);
    ui->iconInstalledPackages_2->setIcon(ui->iconInstalledPackages->icon());
    ui->iconInstalledPackages_3->setIcon(ui->iconInstalledPackages->icon());
    ui->iconInstalledPackages_4->setIcon(ui->iconInstalledPackages->icon());
    ui->iconInstalledPackages_5->setIcon(ui->iconInstalledPackages->icon());
}

QHash<QString, VersionNumber> MainWindow::listInstalledVersions()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    QHash<QString, VersionNumber> result;
    Cmd shell;
    const QStringList &list = shell.getOut("dpkg -l | grep '^ii'", true).split('\n');
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        QMessageBox::critical(
            this, tr("Error"),
            tr("dpkg command returned an error, please run 'dpkg --list' in terminal and check the output."));
        exit(EXIT_FAILURE);
    }
    for (const QString &line : list) {
        QStringList item = line.split(QRegularExpression("\\s{2,}"));
        QString name = item.at(1);
        name.remove(":i386").remove(":amd64");
        QString ver_str = item.at(2);
        ver_str.remove(" amd64");
        result.insert(name, VersionNumber(ver_str));
    }
    return result;
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
    progress->hide();
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

void MainWindow::displayInfoTestOrBackport(const QTreeWidget *tree, const QTreeWidgetItem *item)
{
    QString file_name = (tree == ui->treeMXtest) ? tmp_dir.path() + "/mxPackages" : tmp_dir.path() + "/allPackages";

    QFile file(file_name);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        qDebug() << "Could not open: " << file.fileName();
        return;
    }
    QString msg;
    QString item_name = item->text(TreeCol::Name);
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line == "Package: " + item_name) {
            msg += line + '\n';
            line.clear();
            while (!in.atEnd()) {
                line = in.readLine();
                if (line.startsWith(QLatin1String("Package: "))) {
                    break;
                }
                msg += line + '\n';
            }
        }
    }
    auto msg_list = msg.split('\n');
    auto max_no_chars = 2000;        // Around 15-17 lines
    if (msg.size() > max_no_chars) { // Split msg into details if too large
        uchar max_no_lines = 20;     // Cut message after these many lines
        msg = msg_list.mid(0, max_no_lines).join('\n');
    }
    QMessageBox info(QMessageBox::NoIcon, tr("Package info"), msg, QMessageBox::Close);

    // Make it wider
    auto *horizontalSpacer = new QSpacerItem(width(), 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    auto *layout = qobject_cast<QGridLayout *>(info.layout());
    layout->addItem(horizontalSpacer, 0, 1);
    info.exec();
}

void MainWindow::displayPackageInfo(const QTreeWidget *tree, QPoint pos)
{
    auto *t_widget = qobject_cast<QTreeWidget *>(focusWidget());
    auto *action = new QAction(QIcon::fromTheme("dialog-information"), tr("More &info..."), this);
    if (tree == ui->treePopularApps) {
        if (t_widget->currentItem()->parent() == nullptr) { // Skip categories
            action->deleteLater();
            return;
        }
        connect(action, &QAction::triggered, this,
                [this, t_widget] { displayPopularInfo(t_widget->currentItem(), 3); });
    }
    QMenu menu(this);
    menu.addAction(action);
    if (tree == ui->treeEnabled) {
        connect(action, &QAction::triggered, this, [this, t_widget] { displayPackageInfo(t_widget->currentItem()); });
    } else {
        connect(action, &QAction::triggered, this,
                [this, tree, t_widget] { displayInfoTestOrBackport(tree, t_widget->currentItem()); });
    }
    menu.exec(t_widget->mapToGlobal(pos));
}

void MainWindow::displayPopularInfo(const QTreeWidgetItem *item, int column)
{
    if (column != PopCol::Info || item->parent() == nullptr) {
        return;
    }

    QString desc = item->text(PopCol::Description);
    QString install_names = item->text(PopCol::InstallNames);
    QString title = item->text(PopCol::Name);
    QString msg = "<b>" + title + "</b><p>" + desc + "<p>";
    if (!install_names.isEmpty()) {
        msg += tr("Packages to be installed: ") + install_names;
    }

    QUrl url = item->data(PopCol::Screenshot, Qt::UserRole).toString(); // screenshot url

    if (!url.isValid() || url.isEmpty() || url.url() == QLatin1String("none")) {
        qDebug() << "no screenshot for: " << title;
    } else {
        QNetworkProxyQuery query {QUrl(url)};
        QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        if (!proxies.isEmpty()) {
            manager.setProxy(proxies.first());
        }
        reply = manager.get(QNetworkRequest(url));

        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(5s, &loop, &QEventLoop::quit);
        ui->treePopularApps->blockSignals(true);
        loop.exec();
        ui->treePopularApps->blockSignals(false);

        if (reply->error() != 0) {
            qDebug() << "Download of " << url.url() << " failed: " << qPrintable(reply->errorString());
        } else {
            QImage image;
            QByteArray data;
            QBuffer buffer(&data);
            QImageReader imageReader(reply);
            image = imageReader.read();
            if (imageReader.error() != 0) {
                qDebug() << "loading screenshot: " << imageReader.errorString();
            } else {
                image = image.scaled(QSize(200, 300), Qt::KeepAspectRatioByExpanding);
                image.save(&buffer, "PNG");
                msg += QString("<p><img src='data:image/png;base64, %0'>").arg(QString(data.toBase64()));
            }
        }
    }
    QMessageBox info(QMessageBox::NoIcon, tr("Package info"), msg, QMessageBox::Close);
    info.exec();
}

void MainWindow::displayPackageInfo(const QTreeWidgetItem *item)
{
    QString msg = cmd.getOut("aptitude show " + item->text(TreeCol::Name));
    // Remove first 5 lines from aptitude output "Reading package..."
    QString details = cmd.getOut("DEBIAN_FRONTEND=$(dpkg -l debconf-kde-helper 2>/dev/null "
                                 "| grep -sq ^i && echo kde || echo gnome) aptitude -sy -V -o=Dpkg::Use-Pty=0 install "
                                 + item->text(TreeCol::Name) + " |tail -5");

    auto detail_list = details.split('\n');
    auto msg_list = msg.split('\n');
    auto max_no_chars = 2000;        // Around 15-17 lines
    if (msg.size() > max_no_chars) { // Split msg into details if too large
        uchar max_no_lines = 17;     // Cut message after these many lines
        msg = msg_list.mid(0, max_no_lines).join('\n');
        detail_list = msg_list.mid(max_no_lines, msg_list.length()) + QStringList {} + detail_list;
        details = detail_list.join('\n');
    }
    msg += "\n\n" + detail_list.at(detail_list.size() - 2); // Add info about space needed/freed

    QMessageBox info(QMessageBox::NoIcon, tr("Package info"), msg.trimmed(), QMessageBox::Close);
    info.setDetailedText(details.trimmed());

    // Make it wider
    auto *horizontalSpacer = new QSpacerItem(width(), 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    auto *layout = qobject_cast<QGridLayout *>(info.layout());
    layout->addItem(horizontalSpacer, 0, 1);
    info.exec();
}

void MainWindow::findPopular() const
{
    QString word = ui->searchPopular->text();
    if (word.length() == 1) {
        return;
    }

    if (word.isEmpty()) {
        for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
            (*it)->setExpanded(false);
        }
        ui->treePopularApps->reset();
        for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
            if ((*it)->parent() == nullptr) {
                (*it)->setFirstColumnSpanned(true);
            }
        }
        for (uchar i = 1; i < ui->treePopularApps->columnCount(); ++i) {
            ui->treePopularApps->resizeColumnToContents(i);
        }
        return;
    }
    auto found_items = ui->treePopularApps->findItems(word, Qt::MatchContains | Qt::MatchRecursive, PopCol::Name);
    found_items << ui->treePopularApps->findItems(word, Qt::MatchContains | Qt::MatchRecursive,
                                                  PopCol::Icon); // Category
    found_items << ui->treePopularApps->findItems(word, Qt::MatchContains | Qt::MatchRecursive, PopCol::Description);

    // Hide/unhide items
    for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
        if ((*it)->parent()) {
            if (found_items.contains(*it)) {
                (*it)->setHidden(false);
            } else {
                (*it)->parent()->setHidden(true);
                (*it)->setHidden(true);
            }
        }
    }

    // Process found items
    for (const auto &item : qAsConst(found_items)) {
        if (item->parent()) { // If child, expand parent
            item->parent()->setExpanded(true);
            item->parent()->setHidden(false);
        } else { // if parent, expand children
            item->setFirstColumnSpanned(true);
            item->setExpanded(true);
            item->setHidden(false);
            for (int i = 0; i < item->childCount(); ++i) {
                item->child(i)->setHidden(false);
            }
        }
    }
    for (uchar i = 1; i < ui->treePopularApps->columnCount(); ++i) {
        ui->treePopularApps->resizeColumnToContents(i);
    }
}

void MainWindow::findPackageOther()
{
    // Retrieve the search word from the appropriate search box based on the current tree
    QString word = currentTree == ui->treeEnabled     ? ui->searchBoxEnabled->text()
                   : currentTree == ui->treeMXtest    ? ui->searchBoxMX->text()
                   : currentTree == ui->treeBackports ? ui->searchBoxBP->text()
                                                      : ui->searchBoxFlatpak->text(); // Default to flatpak

    // Return early if the search word is a single character
    if (word.length() == 1) {
        return;
    }
    QList<QTreeWidgetItem *> found_items;
    if (currentTree != ui->treeFlatpak) {
        found_items = currentTree->findItems(word, Qt::MatchContains, TreeCol::Name);
        found_items << currentTree->findItems(word, Qt::MatchContains, TreeCol::Description);
    } else {
        found_items = currentTree->findItems(word, Qt::MatchContains, FlatCol::LongName);
    }
    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        bool shouldHide = (*it)->data(0, Qt::UserRole) == false || !found_items.contains(*it);
        (*it)->setHidden(shouldHide);
    }
    if (currentTree != ui->treeFlatpak) {
        hideLibs();
    }
}

void MainWindow::showOutput()
{
    ui->outputBox->clear();
    ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), true);
    ui->tabWidget->setCurrentWidget(ui->tabOutput);
    enableTabs(false);
}

void MainWindow::on_pushInstall_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();
    if (currentTree == ui->treeFlatpak) {
        // Confirmation dialog
        if (!confirmActions(change_list.join(' '), "install")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        if (cmd.run("socat SYSTEM:'flatpak install -y " + FPuser + ui->comboRemote->currentText() + ' '
                    + change_list.join(' ') + "',stderr STDIO")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
        } else {
            setCursor(QCursor(Qt::ArrowCursor));
            QMessageBox::critical(this, tr("Error"),
                                  tr("Problem detected while installing, please inspect the console output."));
        }
    } else {
        bool success = currentTree == ui->treePopularApps ? installPopularApps() : installSelected();
        setDirty();
        buildPackageLists();
        refreshPopularApps();
        if (success) {
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
        } else {
            QMessageBox::critical(this, tr("Error"),
                                  tr("Problem detected while installing, please inspect the console output."));
        }
    }
    enableTabs(true);
}

void MainWindow::on_pushAbout_clicked()
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

void MainWindow::on_pushHelp_clicked()
{
    QString lang = locale.bcp47Name();
    QString url {"/usr/share/doc/mx-packageinstaller/mx-package-installer.html"};

    if (lang.startsWith("fr")) {
        url = "https://mxlinux.org/wiki/help-files/help-mx-installateur-de-paquets";
    }
    displayDoc(url, tr("%1 Help").arg(windowTitle()));
}

// Resize columns when expanding
void MainWindow::on_treePopularApps_expanded()
{
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
    ui->treePopularApps->resizeColumnToContents(PopCol::Description);
}

void MainWindow::on_treePopularApps_itemExpanded(QTreeWidgetItem *item)
{
    item->setIcon(PopCol::Icon, QIcon::fromTheme("folder-open"));
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
    ui->treePopularApps->resizeColumnToContents(PopCol::Description);
}

void MainWindow::on_treePopularApps_itemCollapsed(QTreeWidgetItem *item)
{
    item->setIcon(PopCol::Icon, QIcon::fromTheme("folder"));
    ui->treePopularApps->resizeColumnToContents(PopCol::Name);
    ui->treePopularApps->resizeColumnToContents(PopCol::Description);
}

void MainWindow::on_pushUninstall_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";

    showOutput();

    QString names;
    QString preuninstall;
    QString postuninstall;
    if (currentTree == ui->treePopularApps) {
        for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
            if ((*it)->checkState(PopCol::Check) == Qt::Checked) {
                names += (*it)->data(PopCol::UninstallNames, Qt::UserRole).toString().replace('\n', ' ') + ' ';
                postuninstall += (*it)->data(PopCol::PostUninstall, Qt::UserRole).toString() + '\n';
                preuninstall += (*it)->data(PopCol::PreUninstall, Qt::UserRole).toString() + '\n';
            }
        }
    } else if (currentTree == ui->treeFlatpak) {
        bool success = true;

        // New version of flatpak takes a "-y" confirmation
        QString conf = "-y ";
        if (fp_ver < VersionNumber("1.0.1")) {
            conf = QString();
        }
        // Confirmation dialog
        if (!confirmActions(change_list.join(' '), "remove")) {
            displayFlatpaks(true);
            indexFilterFP.clear();
            listFlatpakRemotes();
            ui->comboRemote->setCurrentIndex(0);
            on_comboRemote_activated();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }

        setCursor(QCursor(Qt::BusyCursor));
        for (const QString &app : qAsConst(change_list)) {
            enableOutput();
            if (!cmd.run("socat SYSTEM:'flatpak uninstall " + FPuser + conf + ' ' + app
                         + "',stderr STDIO")) { // success if all processed successfuly,
                                                // failure if one failed
                success = false;
            }
        }
        if (success) { // Success if all processed successfuly, failure if one failed
            displayFlatpaks(true);
            indexFilterFP.clear();
            listFlatpakRemotes();
            ui->comboRemote->setCurrentIndex(0);
            on_comboRemote_activated();
            ui->comboFilterFlatpak->setCurrentIndex(0);
            QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
        } else {
            QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling, please check output"));
        }
        enableTabs(true);
        return;
    } else {
        names = change_list.join(' ');
    }

    bool success = uninstall(names, preuninstall, postuninstall);
    setDirty();
    buildPackageLists();
    refreshPopularApps();
    if (success) {
        QMessageBox::information(this, tr("Success"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    } else {
        QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling the program"));
    }
    enableTabs(true);
}

void MainWindow::on_tabWidget_currentChanged(int index)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Console Output"));
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);
    currentTree->blockSignals(true);

    // Reset checkboxes when tab changes
    if (currentTree != ui->treePopularApps) {
        currentTree->clearSelection();
        for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
            (*it)->setCheckState(0, Qt::Unchecked);
        }
    }

    // Save the search text
    QString search_str;
    int filter_idx = 0;
    if (currentTree == ui->treePopularApps) {
        search_str = ui->searchPopular->text();
    } else if (currentTree == ui->treeEnabled) {
        search_str = ui->searchBoxEnabled->text();
        filter_idx = ui->comboFilterEnabled->currentIndex();
    } else if (currentTree == ui->treeMXtest) {
        search_str = ui->searchBoxMX->text();
        filter_idx = ui->comboFilterMX->currentIndex();
    } else if (currentTree == ui->treeBackports) {
        search_str = ui->searchBoxBP->text();
        filter_idx = ui->comboFilterBP->currentIndex();
    } else if (currentTree == ui->treeFlatpak) {
        search_str = ui->searchBoxFlatpak->text();
    }
    switch (index) {
    case Tab::Popular:
        ui->searchPopular->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        if (!ui->searchPopular->text().isEmpty()) {
            findPopular();
        }
        currentTree->blockSignals(false);
        break;
    case Tab::EnabledRepos:
        ui->searchBoxEnabled->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        change_list.clear();
        if (!displayPackagesIsRunning) {
            if (currentTree->topLevelItemCount() == 0 || dirtyEnabledRepos) {
                if (!buildPackageLists()) {
                    QMessageBox::critical(
                        this, tr("Error"),
                        tr("Could not download the list of packages. Please check your APT sources."));
                    currentTree->blockSignals(false);
                    return;
                }
            }
        } else {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
            }
        }
        ui->comboFilterEnabled->setCurrentIndex(filter_idx);
        if (!ui->searchBoxEnabled->text().isEmpty()) {
            findPackageOther();
        }
        if (!displayPackagesIsRunning) {
            currentTree->blockSignals(false);
        }
        break;
    case Tab::Test:
        ui->searchBoxMX->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        displayWarning("test");
        change_list.clear();
        if (currentTree->topLevelItemCount() == 0 || dirtyTest) {
            if (!buildPackageLists()) {
                QMessageBox::critical(this, tr("Error"),
                                      tr("Could not download the list of packages. Please check your APT sources."));
                currentTree->blockSignals(false);
                return;
            }
        }
        ui->comboFilterMX->setCurrentIndex(filter_idx);
        if (!ui->searchBoxMX->text().isEmpty()) {
            findPackageOther();
        }
        currentTree->blockSignals(false);
        break;
    case Tab::Backports:
        ui->searchBoxBP->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        displayWarning("backports");
        change_list.clear();
        if (currentTree->topLevelItemCount() == 0 || dirtyBackports) {
            if (!buildPackageLists()) {
                QMessageBox::critical(this, tr("Error"),
                                      tr("Could not download the list of packages. Please check your APT sources."));
                currentTree->blockSignals(false);
                return;
            }
        }
        ui->comboFilterBP->setCurrentIndex(filter_idx);
        if (!ui->searchBoxBP->text().isEmpty()) {
            findPackageOther();
        }
        currentTree->blockSignals(false);
        break;
    case Tab::Flatpak:
        lastItemClicked = nullptr;
        ui->searchBoxFlatpak->setText(search_str);
        enableTabs(true);
        setCurrentTree();
        displayWarning("flatpaks");
        ui->searchBoxFlatpak->setFocus();
        if (!firstRunFP && checkInstalled("flatpak")) {
            ui->searchBoxBP->setText(search_str);
            if (!ui->searchBoxBP->text().isEmpty()) {
                findPackageOther();
            }
            if (!displayFlatpaksIsRunning) {
                filterChanged(ui->comboFilterFlatpak->currentText());
            }
            currentTree->blockSignals(false);
            return;
        }
        firstRunFP = false;
        blockInterfaceFP(true);
        if (!checkInstalled("flatpak")) {
            int ans = QMessageBox::question(this, tr("Flatpak not installed"),
                                            tr("Flatpak is not currently installed.\nOK to go ahead and install it?"));
            if (ans == QMessageBox::No) {
                ui->tabWidget->setCurrentIndex(Tab::Popular);
                break;
            }
            ui->tabWidget->setTabEnabled(ui->tabWidget->indexOf(ui->tabOutput), true);
            ui->tabWidget->setCurrentWidget(ui->tabOutput);
            setCursor(QCursor(Qt::BusyCursor));
            showOutput();
            displayFlatpaksIsRunning = true;
            install("flatpak");
            installed_packages = listInstalled();
            setDirty();
            buildPackageLists();
            if (!checkInstalled("flatpak")) {
                QMessageBox::critical(this, tr("Flatpak not installed"), tr("Flatpak was not installed"));
                ui->tabWidget->setCurrentIndex(Tab::Popular);
                setCursor(QCursor(Qt::ArrowCursor));
                enableTabs(true);
                currentTree->blockSignals(false);
                return;
            }
            fp_ver = getVersion("flatpak");
            Cmd().runAsRoot("flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo");
            Cmd().runAsRoot("flatpak remote-add --if-not-exists --subset=verified flathub-verified "
                            "https://flathub.org/repo/flathub.flatpakrepo");
            enableOutput();
            listFlatpakRemotes();
            if (displayFlatpaksIsRunning) {
                progress->show();
                if (!timer.isActive()) {
                    timer.start(100ms);
                }
            }
            setCursor(QCursor(Qt::ArrowCursor));
            ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->tabOutput), tr("Console Output"));
            ui->tabWidget->blockSignals(true);
            displayFlatpaks(true);
            ui->tabWidget->blockSignals(false);
            QMessageBox::warning(this, tr("Needs re-login"),
                                 tr("You might need to logout/login to see installed items in the menu"));
            ui->tabWidget->setCurrentWidget(ui->tabFlatpak);
            enableTabs(true);
            return;
        }
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        setCursor(QCursor(Qt::ArrowCursor));
        if (ui->comboRemote->currentText().isEmpty()) {
            listFlatpakRemotes();
        }
        if (displayFlatpaksIsRunning) {
            progress->show();
            if (!timer.isActive()) {
                timer.start(100ms);
                qApp->processEvents();
            }
        }
        ui->searchBoxBP->setText(search_str);
        if (!ui->searchBoxBP->text().isEmpty()) {
            findPackageOther();
        }
        break;
    case Tab::Output:
        ui->searchPopular->clear();
        ui->searchBoxEnabled->clear();
        ui->searchBoxMX->clear();
        ui->searchBoxBP->clear();
        ui->pushInstall->setDisabled(true);
        ui->pushUninstall->setDisabled(true);
        break;
    }
    ui->pushUpgradeAll->setVisible((currentTree == ui->treeEnabled) && (ui->labelNumUpgr->text().toInt() > 0));
}

void MainWindow::filterChanged(const QString &arg1)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    currentTree->blockSignals(true);

    QList<QTreeWidgetItem *> foundItems;
    if (currentTree == ui->treeFlatpak) {
        if (arg1 == tr("Installed runtimes")) {
            displayFilteredFP(installed_runtimes_fp);
        } else if (arg1 == tr("Installed apps")) {
            displayFilteredFP(installed_apps_fp);
        } else if (arg1 == tr("All apps")) {
            if (flatpaks_apps.isEmpty()) {
                flatpaks_apps = listFlatpaks(ui->comboRemote->currentText(), "--app");
            }
            displayFilteredFP(flatpaks_apps, true);
        } else if (arg1 == tr("All runtimes")) {
            if (flatpaks_runtimes.isEmpty()) {
                flatpaks_runtimes = listFlatpaks(ui->comboRemote->currentText(), "--runtime");
            }
            displayFilteredFP(flatpaks_runtimes, true);
        } else if (arg1 == tr("All available")) {
            int total = 0;
            for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
                ++total;
                (*it)->setData(0, Qt::UserRole, true);
                (*it)->setHidden(false);
            }
            ui->labelNumAppFP->setText(QString::number(total));
        } else if (arg1 == tr("All installed")) {
            displayFilteredFP(installed_apps_fp + installed_runtimes_fp);
        } else if (arg1 == tr("Not installed")) {
            for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
                auto userData = (*it)->data(FlatCol::Status, Qt::UserRole);
                if (userData == Status::NotInstalled) {
                    foundItems.append(*it);
                }
            }
            QStringList newList;
            for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
                if (foundItems.contains(*it)) {
                    newList << (*it)->data(FlatCol::FullName, Qt::UserRole).toString();
                }
            }
            displayFilteredFP(newList);
        }
        findPackageOther();
        setSearchFocus();
        currentTree->blockSignals(false);
        return;
    }
    if (arg1 == tr("All packages")) {
        for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
            (*it)->setData(0, Qt::UserRole, true);
            (*it)->setHidden(false);
        }
        findPackageOther();
        setSearchFocus();
        currentTree->blockSignals(false);
        return;
    }

    const QMap<QString, int> statusMap {{tr("Upgradable"), Status::Upgradable},
                                        {tr("Installed"), Status::Installed},
                                        {tr("Not installed"), Status::NotInstalled}};

    auto find = [&foundItems](const QTreeWidgetItemIterator &it, int status) {
        auto userData = (*it)->data(TreeCol::Status, Qt::UserRole);
        if (userData == status) {
            foundItems.append(*it);
        }
    };

    auto itStatus = statusMap.find(arg1);
    if (itStatus != statusMap.end()) {
        for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
            if (itStatus.value() == Status::Installed
                && (*it)->data(TreeCol::Status, Qt::UserRole) != Status::Installed) {
                find(it, Status::Upgradable);
            } else {
                find(it, itStatus.value());
            }
        }
    }

    change_list.clear();
    ui->pushUninstall->setEnabled(false);
    ui->pushInstall->setEnabled(false);

    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        (*it)->setCheckState(TreeCol::Check, Qt::Unchecked); // Uncheck all items
        if (foundItems.contains(*it)) {
            (*it)->setHidden(false);
            (*it)->setData(0, Qt::UserRole, true); // Displayed
        } else {
            (*it)->setHidden(true);
            (*it)->setData(0, Qt::UserRole, false);
        }
    }
    findPackageOther();
    setSearchFocus();
    currentTree->blockSignals(false);
}

void MainWindow::on_treeEnabled_itemChanged(QTreeWidgetItem *item)
{
    if (item->checkState(TreeCol::Check) == Qt::Checked) {
        ui->treeEnabled->setCurrentItem(item);
    }
    buildChangeList(item);
}

void MainWindow::on_treeMXtest_itemChanged(QTreeWidgetItem *item)
{
    if (item->checkState(TreeCol::Check) == Qt::Checked) {
        ui->treeMXtest->setCurrentItem(item);
    }
    buildChangeList(item);
}

void MainWindow::on_treeBackports_itemChanged(QTreeWidgetItem *item)
{
    if (item->checkState(TreeCol::Check) == Qt::Checked) {
        ui->treeBackports->setCurrentItem(item);
    }
    buildChangeList(item);
}

void MainWindow::on_treeFlatpak_itemChanged(QTreeWidgetItem *item)
{
    if (item->checkState(FlatCol::Check) == Qt::Checked) {
        ui->treeFlatpak->setCurrentItem(item);
    }
    buildChangeList(item);
}

// Build the change_list when selecting on item in the tree
void MainWindow::buildChangeList(QTreeWidgetItem *item)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    /* if all apps are uninstalled (or some installed) -> enable Install, disable Uinstall
     * if all apps are installed or upgradable -> enable Uninstall, enable Install
     * if all apps are upgradable -> change Install label to Upgrade;
     */

    lastItemClicked = item;
    QString newapp;
    if (currentTree == ui->treeFlatpak) {
        if (change_list.isEmpty()
            && indexFilterFP.isEmpty()) { // remember the Flatpak combo location first time this is called
            indexFilterFP = ui->comboFilterFlatpak->currentText();
        }
        newapp = (item->data(FlatCol::FullName, Qt::UserRole).toString());
    } else {
        newapp = (item->text(TreeCol::Name));
    }

    if (item->checkState(0) == Qt::Checked) {
        ui->pushInstall->setEnabled(true);
        change_list.append(newapp);
    } else {
        change_list.removeOne(newapp);
    }

    if (currentTree != ui->treeFlatpak) {
        ui->pushUninstall->setEnabled(checkInstalled(change_list));
        ui->pushInstall->setText(checkUpgradable(change_list) ? tr("Upgrade") : tr("Install"));
    } else { // For Flatpaks allow selection only of installed or not installed items so one clicks
             // on an installed item only installed items should be displayed and the other way round
        ui->pushInstall->setText(tr("Install"));
        if (item->data(FlatCol::Status, Qt::UserRole) == Status::Installed) {
            if (item->checkState(FlatCol::Check) == Qt::Checked
                && ui->comboFilterFlatpak->currentText() != tr("All installed")) {
                ui->comboFilterFlatpak->setCurrentText(tr("All installed"));
            }
            ui->pushUninstall->setEnabled(true);
            ui->pushInstall->setEnabled(false);
        } else {
            if (item->checkState(FlatCol::Check) == Qt::Checked
                && ui->comboFilterFlatpak->currentText() != tr("Not installed")) {
                ui->comboFilterFlatpak->setCurrentText(tr("Not installed"));
            }
            ui->pushUninstall->setEnabled(false);
            ui->pushInstall->setEnabled(true);
        }
        if (change_list.isEmpty()) { // Reset comboFilterFlatpak if nothing is selected
            ui->comboFilterFlatpak->setCurrentText(indexFilterFP);
            indexFilterFP.clear();
        }
        ui->treeFlatpak->setFocus();
    }

    if (change_list.isEmpty()) {
        ui->pushInstall->setEnabled(false);
        ui->pushUninstall->setEnabled(false);
    }
}

// Force repo upgrade
void MainWindow::on_pushForceUpdateEnabled_clicked()
{
    ui->searchBoxEnabled->clear();
    ui->comboFilterEnabled->setCurrentIndex(0);
    buildPackageLists(true);
}

void MainWindow::on_pushForceUpdateMX_clicked()
{
    ui->searchBoxMX->clear();
    ui->comboFilterMX->setCurrentIndex(0);
    buildPackageLists(true);
}

void MainWindow::on_pushForceUpdateBP_clicked()
{
    ui->searchBoxBP->clear();
    ui->comboFilterBP->setCurrentIndex(0);
    buildPackageLists(true);
}

// Hide/unhide lib/-dev packages
void MainWindow::on_checkHideLibs_toggled(bool checked)
{
    ui->checkHideLibsMX->setChecked(checked);
    ui->checkHideLibsBP->setChecked(checked);

    for (QTreeWidgetItemIterator it(ui->treeEnabled); (*it) != nullptr; ++it) {
        (*it)->setHidden(isFilteredName((*it)->text(TreeCol::Name)) && checked);
    }
    filterChanged(ui->comboFilterEnabled->currentText());
}

void MainWindow::on_pushUpgradeAll_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();

    QList<QTreeWidgetItem *> foundItems;
    for (QTreeWidgetItemIterator it(currentTree); (*it) != nullptr; ++it) {
        auto userData = (*it)->data(TreeCol::Status, Qt::UserRole);
        if (userData == Status::Upgradable) {
            foundItems.append(*it);
        }
    }
    QString names;
    for (QTreeWidgetItemIterator it(ui->treeEnabled); (*it) != nullptr; ++it) {
        if (foundItems.contains(*it)) {
            names += (*it)->text(TreeCol::Name) + ' ';
        }
    }
    bool success = install(names);
    setDirty();
    buildPackageLists();
    if (success) {
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    } else {
        QMessageBox::critical(this, tr("Error"),
                              tr("Problem detected while installing, please inspect the console output."));
    }
    enableTabs(true);
}

// Pressing Enter or buttonEnter will do the same thing
void MainWindow::on_pushEnter_clicked()
{
    if (currentTree == ui->treeFlatpak
        && ui->lineEdit->text().isEmpty()) { // Add "Y" as default response for flatpaks to work like apt-get
        cmd.write("y");
    }
    on_lineEdit_returnPressed();
}

void MainWindow::on_lineEdit_returnPressed()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    cmd.write(ui->lineEdit->text().toUtf8() + '\n');
    ui->outputBox->appendPlainText(ui->lineEdit->text() + '\n');
    ui->lineEdit->clear();
    ui->lineEdit->setFocus();
}

void MainWindow::on_pushCancel_clicked()
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

void MainWindow::on_checkHideLibsMX_clicked(bool checked)
{
    ui->checkHideLibs->setChecked(checked);
    ui->checkHideLibsBP->setChecked(checked);
}

void MainWindow::on_checkHideLibsBP_clicked(bool checked)
{
    ui->checkHideLibs->setChecked(checked);
    ui->checkHideLibsMX->setChecked(checked);
}

// On change flatpak remote
void MainWindow::on_comboRemote_activated(int /*index*/)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    lastItemClicked = nullptr;
    displayFlatpaks(true);
}

void MainWindow::on_pushUpgradeFP_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    enableOutput();
    if (cmd.run("socat SYSTEM:'flatpak update " + FPuser + "',pty STDIO")) {
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

void MainWindow::on_pushRemotes_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    auto *dialog = new ManageRemotes(this);
    dialog->exec();
    if (dialog->isChanged()) {
        listFlatpakRemotes();
        displayFlatpaks(true);
    }
    if (!dialog->getInstallRef().isEmpty()) {
        showOutput();
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        if (cmd.run("socat SYSTEM:'flatpak install -y " + dialog->getUser() + "--from "
                    + dialog->getInstallRef().replace(':', "\\:") + "',stderr STDIO\"")) {
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

void MainWindow::on_comboUser_activated(int index)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (index == 0) {
        FPuser = "--system ";
    } else {
        FPuser = "--user ";
        static bool updated = false;
        if (!updated) {
            setCursor(QCursor(Qt::BusyCursor));
            enableOutput();
            cmd.run("flatpak --user remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo");
            cmd.run("flatpak --user remote-add --if-not-exists --subset=verified flathub-verified "
                    "https://flathub.org/repo/flathub.flatpakrepo");
            if (fp_ver >= VersionNumber("1.2.4")) {
                cmd.run("flatpak update --appstream");
            }
            setCursor(QCursor(Qt::ArrowCursor));
            updated = true;
        }
    }
    lastItemClicked = nullptr;
    listFlatpakRemotes();
    displayFlatpaks(true);
}

void MainWindow::on_treePopularApps_customContextMenuRequested(QPoint pos)
{
    auto *t_widget = qobject_cast<QTreeWidget *>(focusWidget());
    if (t_widget->currentItem()->parent() == nullptr) { // skip categories
        return;
    }
    auto *action = new QAction(QIcon::fromTheme("dialog-information"), tr("More &info..."), this);
    QMenu menu(this);
    menu.addAction(action);
    connect(action, &QAction::triggered, this, [this, t_widget] { displayPopularInfo(t_widget->currentItem(), 3); });
    menu.exec(ui->treePopularApps->mapToGlobal(pos));
}

// Process keystrokes
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        on_pushCancel_clicked();
    }
}

void MainWindow::on_treePopularApps_itemChanged(QTreeWidgetItem *item)
{
    if (item->checkState(PopCol::Check) == Qt::Checked) {
        ui->treePopularApps->setCurrentItem(item);
    }
    bool checked = false;
    bool installed = true;

    for (QTreeWidgetItemIterator it(ui->treePopularApps); (*it) != nullptr; ++it) {
        if ((*it)->checkState(PopCol::Check) == Qt::Checked) {
            checked = true;
            if ((*it)->icon(PopCol::Check).isNull()) {
                installed = false;
            }
        }
    }
    ui->pushInstall->setEnabled(checked);
    ui->pushUninstall->setEnabled(checked && installed);
    if (checked && installed) {
        ui->pushInstall->setText(tr("Reinstall"));
    } else {
        ui->pushInstall->setText(tr("Install"));
    }
}

void MainWindow::on_pushRemoveUnused_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    enableOutput();
    // New version of flatpak takes a "-y" confirmation
    QString conf = "-y ";
    if (fp_ver < VersionNumber("1.0.1")) {
        conf = QString();
    }
    if (cmd.run("socat SYSTEM:'flatpak uninstall --unused " + conf + "',pty STDIO")) {
        displayFlatpaks(true);
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

void MainWindow::on_pushRemoveOrphan_clicked()
{
    QString names = cmd.getOutAsRoot(R"(apt-get --dry-run autoremove |grep -Po '^Remv \K[^ ]+' |tr '\n' ' ')");
    QMessageBox::warning(this, tr("Warning"),
                         tr("Potentially dangerous operation.\nPlease make sure you check "
                            "carefully the list of packages to be removed."));
    showOutput();
    bool success = uninstall(names);
    setDirty();
    buildPackageLists();
    refreshPopularApps();
    if (success) {
        QMessageBox::information(this, tr("Success"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(currentTree->parentWidget());
    } else {
        QMessageBox::critical(this, tr("Error"), tr("We encountered a problem uninstalling the program"));
    }
    enableTabs(true);
    ui->tabWidget->setCurrentIndex(Tab::EnabledRepos);
}
