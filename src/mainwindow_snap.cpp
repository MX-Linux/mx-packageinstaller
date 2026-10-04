#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTextCursor>
#include <QTimer>

#include <chrono>
#include <utility>

using MainWindowHelpers::appendFlatpakStatusMessage;
using MainWindowHelpers::runMxpiLibAsRoot;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Snap support
// ---------------------------------------------------------------------------

// Show a critical error dialog with a short, fixed message. Any long/raw output
// (command logs, backend errors) goes into the collapsible "Show Details" box so
// it never bloats the title or the message body.
void MainWindow::showError(const QString &message, const QString &details)
{
    QMessageBox box(QMessageBox::Critical, tr("Error"), message, QMessageBox::Ok, this);
    const QString trimmed = details.trimmed();
    if (!trimmed.isEmpty()) {
        box.setDetailedText(trimmed);
    }
    box.exec();
}

// snapd is a systemd-only service; the Snap tab is hidden on non-systemd systems.
bool MainWindow::isSystemdInit()
{
    QFile comm {"/proc/1/comm"};
    if (comm.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString init = QString::fromLatin1(comm.readLine()).trimmed();
        if (!init.isEmpty()) {
            return init == QLatin1String("systemd");
        }
    }
    // Fallback: systemd populates this directory only when it is the init system
    return QFileInfo::exists(QStringLiteral("/run/systemd/system"));
}

// snapd is usable once the package is installed and its socket unit is active.
bool MainWindow::isSnapdReady() const
{
    return checkInstalled(QStringLiteral("snapd")) && QFile::exists(QStringLiteral("/run/snapd.socket"));
}

QStringList MainWindow::listInstalledSnaps() const
{
    Cmd shell;
    const QString out = shell.getOut({{"LANG", "C"}}, "snap", {"list"}, Cmd::QuietMode::Yes, /*discardStderr=*/true);
    QStringList names;
    static const QRegularExpression ws {QStringLiteral("\\s+")};
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split(ws, Qt::SkipEmptyParts);
        if (parts.isEmpty() || parts.at(0) == QLatin1String("Name")) {
            continue;
        }
        names << parts.at(0);
    }
    return names;
}

// Parse the tabular output of `snap list` (installed=true) or `snap find` (installed=false).
QVector<SnapData> MainWindow::parseSnapList(const QString &output, bool installed) const
{
    QVector<SnapData> result;
    static const QRegularExpression ws {QStringLiteral("\\s+")};
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split(ws, Qt::SkipEmptyParts);
        if (parts.isEmpty() || parts.at(0) == QLatin1String("Name")) {
            continue; // header row
        }
        SnapData data;
        if (installed) {
            // Columns: Name Version Rev Tracking Publisher Notes
            if (parts.size() < 2) {
                continue;
            }
            data.name = parts.at(0);
            data.version = parts.value(1);
            data.publisher = parts.value(4);
            data.notes = parts.value(5);
        } else {
            // Columns: Name Version Publisher Notes Summary...
            if (parts.size() < 4) {
                continue;
            }
            data.name = parts.at(0);
            data.version = parts.value(1);
            data.publisher = parts.value(2);
            data.notes = parts.value(3);
            data.description = parts.size() > 4 ? parts.mid(4).join(' ') : QString();
        }
        // Strip the verification markers snap appends to trusted publishers (✓, *, **)
        data.publisher.remove(QChar(0x2713));
        while (data.publisher.endsWith(QLatin1Char('*'))) {
            data.publisher.chop(1);
        }
        data.isClassic = data.notes.contains(QLatin1String("classic"));
        result.append(data);
    }
    return result;
}

void MainWindow::handleSnapTab(const QString &searchStr)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    ui->searchBoxSnap->setText(searchStr);
    setCurrentTree();
    ui->searchBoxSnap->setFocus();
    // changeList is shared across tabs; start the Snap tab with a clean slate so
    // install/remove only ever act on snaps selected here.
    changeList.clear();

    const bool ready = isSnapdReady();
    ui->frameSnapSetup->setVisible(!ready);
    ui->comboFilterSnap->setEnabled(ready);
    ui->searchBoxSnap->setEnabled(ready);
    ui->pushRefreshSnap->setEnabled(ready);
    ui->pushUpgradeSnap->setEnabled(ready);
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);

    if (!ready) {
        if (snapModel) {
            snapModel->clear();
        }
        updateSnapCounts();
        currentTree->blockSignals(false);
        return;
    }

    snapStoreMode = (ui->comboFilterSnap->currentText() == tr("Search store"));

    // Arriving from another tab with a pending search term: default to searching the
    // store for it (searchSnapStore() flips the combo to "Search store" itself).
    if (!searchStr.isEmpty()) {
        firstRunSnap = false;
        ui->searchBoxSnap->setFocus();
        QMetaObject::invokeMethod(this, [this] { searchSnapStore(); }, Qt::QueuedConnection);
        currentTree->blockSignals(false);
        return;
    }

    if (firstRunSnap || (snapModel && snapModel->rowCount() == 0 && !snapStoreMode)) {
        firstRunSnap = false;
        setCursor(QCursor(Qt::BusyCursor));
        displaySnaps(true);
        setCursor(QCursor(Qt::ArrowCursor));
    }
    currentTree->blockSignals(false);
}

void MainWindow::displaySnaps(bool /*forceUpdate*/)
{
    if (!snapModel) {
        return;
    }
    if (!isSnapdReady()) {
        snapModel->clear();
        updateSnapCounts();
        return;
    }
    snapStoreMode = false;
    loadSnapData();
    populateSnapTree();
    updateSnapCounts();
}

void MainWindow::loadSnapData()
{
    Cmd shell;
    const QString out = shell.getOut({{"LANG", "C"}}, "snap", {"list"}, Cmd::QuietMode::Yes, /*discardStderr=*/true);
    const QVector<SnapData> data = parseSnapList(out, true);
    snapModel->setSnapData(data);
    snapModel->updateInstalledStatus(listInstalledSnaps());
    if (snapProxy) {
        snapProxy->setStatusFilter(0);
    }
}

void MainWindow::populateSnapTree()
{
    ui->treeSnap->sortByColumn(SnapCol::Name, Qt::AscendingOrder);
    if (snapModel) {
        for (int i = 0; i < snapModel->columnCount(); ++i) {
            if (!ui->treeSnap->isColumnHidden(i)) {
                ui->treeSnap->resizeColumnToContents(i);
            }
        }
    }
}

void MainWindow::updateSnapCounts()
{
    const int total = snapProxy ? snapProxy->rowCount() : 0;
    int installed = 0;
    if (snapModel) {
        for (int i = 0; i < snapModel->rowCount(); ++i) {
            const SnapData *snap = snapModel->snapAt(i);
            if (snap && snap->status == Status::Installed) {
                ++installed;
            }
        }
    }
    ui->labelNumAppSnap->setText(QString::number(total));
    ui->labelNumInstSnap->setText(QString::number(installed));
}

// Triggered by pressing Enter in the snap search box: query the snap store.
void MainWindow::searchSnapStore()
{
    if (currentTree != ui->treeSnap || !snapModel || !isSnapdReady()) {
        return;
    }
    const QString query = ui->searchBoxSnap->text().trimmed();
    if (query.isEmpty()) {
        ui->comboFilterSnap->setCurrentText(tr("Installed snaps"));
        return;
    }
    if (ui->comboFilterSnap->currentText() != tr("Search store")) {
        ui->comboFilterSnap->blockSignals(true);
        ui->comboFilterSnap->setCurrentText(tr("Search store"));
        ui->comboFilterSnap->blockSignals(false);
    }
    snapStoreMode = true;
    setCursor(QCursor(Qt::BusyCursor));
    Cmd shell;
    const QString out
        = shell.getOut({{"LANG", "C"}}, "snap", {"find", query}, Cmd::QuietMode::Yes, /*discardStderr=*/true);
    QVector<SnapData> data = parseSnapList(out, false);
    snapModel->setSnapData(data);
    snapModel->updateInstalledStatus(listInstalledSnaps());
    if (snapProxy) {
        snapProxy->setStatusFilter(0);
        snapProxy->setSearchText(QString()); // store results already match; don't filter them locally
    }
    populateSnapTree();
    updateSnapCounts();
    changeList.clear();
    ui->pushInstall->setEnabled(false);
    ui->pushUninstall->setEnabled(false);
    setCursor(QCursor(Qt::ArrowCursor));
    if (data.isEmpty()) {
        QMessageBox::information(this, tr("No results"), tr("No snaps found matching \"%1\".").arg(query));
    }
}


void MainWindow::onSnapCheckStateChanged(const QString &name, Qt::CheckState state, int status)
{
    buildSnapChangeList(name, state, status);
}

void MainWindow::buildSnapChangeList(const QString &name, Qt::CheckState state, int /*status*/)
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (state == Qt::Checked) {
        changeList.append(name);
    } else {
        changeList.removeOne(name);
    }

    // Enable each action based on the whole selection, not just the last toggle:
    // Install if any selected snap is not installed, Remove if any is installed.
    // The install/remove handlers split the selection by status accordingly.
    bool anyInstalled = false;
    bool anyNotInstalled = false;
    for (const QString &selectedName : std::as_const(changeList)) {
        const int row = snapModel ? snapModel->findSnapRow(selectedName) : -1;
        const SnapData *snap = (row >= 0) ? snapModel->snapAt(row) : nullptr;
        if (snap && snap->status == Status::Installed) {
            anyInstalled = true;
        } else {
            anyNotInstalled = true;
        }
    }

    ui->pushInstall->setText(tr("Install"));
    ui->pushInstall->setEnabled(!changeList.isEmpty() && anyNotInstalled);
    ui->pushUninstall->setEnabled(!changeList.isEmpty() && anyInstalled);
    ui->treeSnap->setFocus();
}

void MainWindow::pushRefreshSnap_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (!isSnapdReady()) {
        handleSnapTab(QString());
        return;
    }
    ui->searchBoxSnap->clear();
    ui->comboFilterSnap->blockSignals(true);
    ui->comboFilterSnap->setCurrentText(tr("Installed snaps"));
    ui->comboFilterSnap->blockSignals(false);
    setCursor(QCursor(Qt::BusyCursor));
    displaySnaps(true);
    setCursor(QCursor(Qt::ArrowCursor));
}

void MainWindow::pushSetupSnapd_clicked()
{
    setupSnapd();
}


// Install snapd and bring the service up, elevating through the MXPI helper.
void MainWindow::setupSnapd()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    beginOperation();
    ui->tabWidget->setTabEnabled(Tab::Output, true);
    ui->tabWidget->setCurrentWidget(ui->tabOutput);
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    enableOutput();

    if (!checkInstalled(QStringLiteral("snapd"))) {
        // Refresh the package lists first so the install pulls the current snapd
        // candidate; a stale cache can make the install fail or fetch nothing.
        if (!updateApt()) {
            setCursor(QCursor(Qt::ArrowCursor));
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            enableTabs(true);
            return;
        }
        QTreeView *savedTree = currentTree;
#ifdef PACKAGE_BACKEND_PACMAN
        // snapd is not in the official Arch repos -- it's AUR-only, so route this
        // through install()'s paru/AUR branch by pretending the AUR tree is current.
        currentTree = ui->treeAUR;
#else
        // Reuse the standard APT install path (uses default repo flags).
        currentTree = ui->treeEnabled;
#endif
        const bool ok = install(QStringLiteral("snapd"));
        const QString installOutput = cmd.readAllOutput();
        currentTree = savedTree;
        if (operationWasCanceled()) {
            setCursor(QCursor(Qt::ArrowCursor));
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            enableTabs(true);
            return;
        }
        // Refresh the in-memory installed-package list BEFORE verifying, otherwise
        // checkInstalled() still consults the pre-install snapshot and reports a
        // false "not installed" even though dpkg has just configured snapd.
        installedPackages = listInstalled();
        setDirty();
        if (!ok || !checkInstalled(QStringLiteral("snapd"))) {
            setCursor(QCursor(Qt::ArrowCursor));
            showError(tr("snapd was not installed. Click \"Show Details\" for more information."), installOutput);
            ui->tabWidget->setCurrentWidget(ui->tabSnap);
            enableTabs(true);
            return;
        }
    }

    // Enable the snapd service (runs as root through the MXPI helper). Capture its
    // output, but don't depend on it: on Debian, installing the snapd package already
    // enables snapd.socket, and an outdated helper would produce nothing here.
    runMxpiLibAsRoot(cmd, QStringLiteral("snapd_setup"), Cmd::QuietMode::No);
    QString setupOutput = cmd.readAllOutput();
    setCursor(QCursor(Qt::ArrowCursor));
    // Dismissing this prompt cancels the setup. Continuing would ask again for the core
    // snap, and the still-set dismissed flag would end that step silently even after
    // the user authenticated and core was installed.
    if (Cmd::elevationDismissed()) {
        ui->tabWidget->setCurrentWidget(ui->tabSnap);
        enableTabs(true);
        return;
    }

    firstRunSnap = false;
    displaySnaps(true);
    bool ready = isSnapdReady();
    bool coreInstalled = listInstalledSnaps().contains(QStringLiteral("core"));

    // Install the base "core" snap directly from here (not via the helper) so the real
    // outcome is always captured for the user, regardless of which helper is deployed.
    if (ready && !coreInstalled) {
        setCursor(QCursor(Qt::BusyCursor));
        enableOutput();
        appendFlatpakStatusMessage(ui->outputBox, tr("Installing the base \"core\" snap..."));
        // Remember where this attempt's output starts so a transient first-try failure
        // can be discarded before retrying, keeping it from reaching the user.
        const int outputAnchor = ui->outputBox->document()->characterCount() - 1;

        // Just after snapd is enabled the daemon is often not ready yet: the first
        // install can fail with "too early for operation, device not yet seeded" or a
        // transient connection error even though a later attempt succeeds. Wait for
        // seeding, then retry a few times with a short backoff, surfacing only the last
        // attempt's output so the user never sees the transient error.
        constexpr int maxAttempts = 3;
        QString coreOutput;
        for (int attempt = 1; attempt <= maxAttempts && !coreInstalled; ++attempt) {
            // Bounded, read-only wait for seeding (needs no elevation). A stale helper
            // skips its own wait, hence doing it here too.
            Cmd waitCmd;
            waitCmd.proc(QStringLiteral("timeout"), {"120", "snap", "wait", "system", "seed.loaded"}, nullptr,
                        nullptr, Cmd::QuietMode::Yes);
            if (attempt > 1) {
                // Give snapd a moment to finish coming up before trying again,
                // without blocking the event loop.
                QEventLoop backoffLoop;
                QTimer::singleShot(3s, &backoffLoop, &QEventLoop::quit);
                backoffLoop.exec();
            }
            // Route through the MXPI helper (auth_admin_keep) so the password cached by
            // the snapd apt install is reused instead of prompting again.
            cmd.procAsRoot(QStringLiteral("snap"), {QStringLiteral("install"), QStringLiteral("core")});
            if (Cmd::elevationDismissed()) {
                setCursor(QCursor(Qt::ArrowCursor));
                ui->tabWidget->setCurrentWidget(ui->tabSnap);
                enableTabs(true);
                return;
            }
            coreOutput = cmd.readAllOutput().trimmed();
            coreInstalled = listInstalledSnaps().contains(QStringLiteral("core"));
            if (coreInstalled) {
                break;
            }
            // Failed and a retry remains: drop this attempt's output so the next try
            // starts clean and the transient error is not surfaced.
            if (attempt < maxAttempts) {
                QTextCursor cursor(ui->outputBox->document());
                cursor.setPosition(outputAnchor);
                cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
                outputRenderer.reset();
            }
        }
        if (!coreOutput.isEmpty()) {
            setupOutput = coreOutput;
        }
        setCursor(QCursor(Qt::ArrowCursor));
        displaySnaps(true);
    }

    ready = isSnapdReady();
    ui->frameSnapSetup->setVisible(!ready);
    ui->comboFilterSnap->setEnabled(ready);
    ui->searchBoxSnap->setEnabled(ready);
    ui->pushRefreshSnap->setEnabled(ready);
    ui->pushUpgradeSnap->setEnabled(ready);
    ui->tabWidget->setCurrentWidget(ui->tabSnap);

    // Make sure the details box always has content so the "Show Details" button appears.
    const QString details = setupOutput.trimmed().isEmpty()
                                ? tr("No output was captured. Run 'sudo snap install core' in a terminal to see "
                                     "the underlying error.")
                                : setupOutput;

    if (!ready) {
        showError(tr("snapd was installed but its service could not be started. You may need to reboot or log out "
                     "and back in, then reopen the Snap tab. Click \"Show Details\" for more information."),
                  details);
    } else if (!coreInstalled) {
        showError(tr("Snap support was enabled, but the base \"core\" snap could not be installed, so most snaps will "
                     "not work yet. Click \"Show Details\" for the underlying error."),
                  details);
    } else {
        QMessageBox::warning(this, tr("Needs re-login"),
                             tr("Log out and back in to see installed items in the menu and use snap commands from "
                                "/snap/bin. These changes do not apply to your current session."));
    }
    enableTabs(true);
}

void MainWindow::pushUpgradeSnap_clicked()
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    if (!isSnapdReady()) {
        return;
    }
    Cmd::resetElevationDismissed();
    showOutput();
    setCursor(QCursor(Qt::BusyCursor));
    enableOutput();
    if (cmd.procAsRoot(QStringLiteral("snap"), {QStringLiteral("refresh")})) {
        appendFlatpakStatusMessage(ui->outputBox, tr("Update complete."));
        displaySnaps(true);
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::information(this, tr("Done"), tr("Processing finished successfully."));
        ui->tabWidget->setCurrentWidget(ui->tabSnap);
    } else if (!Cmd::elevationDismissed()) {
        const QString errorDetails = cmd.readAllOutput();
        setCursor(QCursor(Qt::ArrowCursor));
        showError(tr("Problem detected while updating snaps. Click \"Show Details\" for more information."),
                  errorDetails);
    } else {
        setCursor(QCursor(Qt::ArrowCursor));
        ui->tabWidget->setCurrentWidget(ui->tabSnap);
    }
    enableTabs(true);
}

void MainWindow::displaySnapInfo(const QModelIndex &index)
{
    if (!index.isValid() || !snapProxy || !snapModel) {
        return;
    }
    const SnapData *snap = snapModel->snapAt(snapProxy->mapToSource(index).row());
    if (!snap || snap->name.isEmpty()) {
        return;
    }

    QString text;
    bool richText = false;
    {
        // The network/Cmd calls below spin nested event loops; disable the window
        // meanwhile so they can't be reentered (see displayPackageInfo()).
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::BusyCursor);
        auto restore = qScopeGuard([this] {
            QApplication::restoreOverrideCursor();
            setEnabled(true);
        });

        // The store API refuses requests without a device series header.
        const QUrl url(QStringLiteral("https://api.snapcraft.io/v2/snaps/info/%1"
                                      "?fields=title,summary,description,media,publisher,license,website")
                           .arg(QString::fromUtf8(QUrl::toPercentEncoding(snap->name))));
        const QJsonObject json
            = QJsonDocument::fromJson(fetchUrl(url, {{"Snap-Device-Series", "16"}})).object().value(QStringLiteral("snap")).toObject();
        if (!json.isEmpty()) {
            AppInfo app;
            app.title = json.value(QStringLiteral("title")).toString();
            if (app.title.isEmpty()) {
                app.title = snap->name;
            }
            app.summary = json.value(QStringLiteral("summary")).toString();
            // The store description is plain text with blank lines between paragraphs.
            const QStringList paragraphs = json.value(QStringLiteral("description"))
                                               .toString()
                                               .split(QRegularExpression(QStringLiteral("\n\\s*\n")), Qt::SkipEmptyParts);
            for (const QString &para : paragraphs) {
                app.descriptionHtml += QStringLiteral("<p>")
                                       + para.trimmed().toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
            }
            app.developer = json.value(QStringLiteral("publisher")).toObject().value(QStringLiteral("display-name")).toString();
            app.license = json.value(QStringLiteral("license")).toString();
            app.homepage = json.value(QStringLiteral("website")).toString();
            for (const auto &value : json.value(QStringLiteral("media")).toArray()) {
                const QJsonObject media = value.toObject();
                if (media.value(QStringLiteral("type")).toString() == QLatin1String("screenshot")) {
                    app.screenshotHtml = imageHtml(QUrl(media.value(QStringLiteral("url")).toString()));
                    break;
                }
            }
            text = appInfoHtml(app);
            richText = true;
        } else {
            // Offline or not in the store: fall back to snapd's own info.
            Cmd shell;
            text = shell.getOut("snap", {"info", snap->name}, Cmd::QuietMode::Yes).trimmed();
        }
    }
    showInfoBox(text.isEmpty() ? tr("No information available.") : text, richText);
}
