#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QFile>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QStandardPaths>

#include <chrono>
#include <utility>

using namespace std::chrono_literals;

#ifdef PACKAGE_BACKEND_PACMAN
QString MainWindow::getParuPath()
{
    if (cachedParuPathFetched) {
        return cachedParuPath;
    }
    cachedParuPath = QStandardPaths::findExecutable("paru");
    if (cachedParuPath.isEmpty()) {
        const QStringList fallbackPaths = {"/usr/bin/paru", "/bin/paru", "/usr/local/bin/paru"};
        for (const QString &path : fallbackPaths) {
            if (QFile::exists(path)) {
                cachedParuPath = path;
                break;
            }
        }
    }
    cachedParuPathFetched = true;
    return cachedParuPath;
}

// Populates aurList: with an empty search term, the AUR-origin ("foreign") installed
// packages, showing an available-upgrade version where paru reports one; otherwise a
// live AUR search via paru. No async caching (unlike the arch branch this was ported
// from) -- this always re-queries, matching how every other list in this app already
// works synchronously.
bool MainWindow::buildAurList(const QString &searchTerm)
{
    aurList.clear();
    const QString term = searchTerm.trimmed();
    const QString paruPath = getParuPath();
    if (paruPath.isEmpty()) {
        return false;
    }

    Cmd shell;
    QScopedValueRollback<bool> guard(suppressCmdOutput, true);
    // "pacman -Qm" and "paru -Ss" exit 1 when nothing matches. With no output at all
    // (stderr is captured too, so a real error leaves a message), that is an empty
    // result, not a failed query.
    const auto queryFailed = [&shell](const QStringList &output) {
        if (shell.exitStatus() != QProcess::NormalExit) {
            return true;
        }
        return shell.exitCode() != 0 && !(shell.exitCode() == 1 && output.isEmpty());
    };
    if (term.isEmpty()) {
        const QStringList installed
            = shell.getOut({{"LANG", "C"}}, "pacman", {"-Qm", "--color", "never"}).split('\n', Qt::SkipEmptyParts);
        if (queryFailed(installed)) {
            return false;
        }
        if (installed.isEmpty()) {
            return true;
        }
        if (installedPackages.isEmpty()) {
            installedPackages = listInstalled();
        }

        QHash<QString, QString> aurUpdates;
        const QStringList updates
            = shell.getOut({{"LANG", "C"}}, paruPath, {"-Qua", "--color", "never"}).split('\n', Qt::SkipEmptyParts);
        if (shell.exitStatus() == QProcess::NormalExit && shell.exitCode() == 0) {
            for (const QString &line : updates) {
                const QString name = line.section(' ', 0, 0).trimmed();
                const QString newVersion = line.section("->", 1).trimmed();
                if (!name.isEmpty() && !newVersion.isEmpty()) {
                    aurUpdates.insert(name, newVersion);
                }
            }
        }

        for (const QString &line : installed) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                continue;
            }
            const QString repoVersion = aurUpdates.value(parts.at(0), parts.at(1));
            const QString description = installedPackages.value(parts.at(0)).description;
            aurList.insert(parts.at(0), {repoVersion, description});
        }
        return true;
    }

    if (!isOnline()) {
        return false;
    }
    // --aur: plain "-Ss" also searches the sync repos and lists their matches first.
    QStringList results = shell.getOut({{"LANG", "C"}}, paruPath, {"-Ssq", "--aur", "--color", "never", term})
                              .split('\n', Qt::SkipEmptyParts);
    if (queryFailed(results)) {
        return false;
    }
    if (results.isEmpty()) {
        return true;
    }
    constexpr int maxResults = 200;
    if (results.size() > maxResults) {
        results = results.mid(0, maxResults);
    }

    QStringList infoArgs {"-Si", "--aur", "--color", "never"};
    infoArgs += results;
    const QString infoOutput = shell.getOut({{"LANG", "C"}}, paruPath, infoArgs);
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0 || infoOutput.trimmed().isEmpty()) {
        for (const QString &name : std::as_const(results)) {
            aurList.insert(name, {QString(), QString()});
        }
        return true;
    }

    QString packageName;
    QString version;
    QString description;
    auto flushPackage = [&]() {
        if (!packageName.isEmpty()) {
            aurList.insert(packageName, {version, description});
        }
        packageName.clear();
        version.clear();
        description.clear();
    };
    for (const QString &line : infoOutput.split('\n')) {
        if (line.startsWith("Name")) {
            flushPackage();
            packageName = line.section(':', 1).trimmed();
        } else if (line.startsWith("Version")) {
            version = line.section(':', 1).trimmed();
        } else if (line.startsWith("Description")) {
            description = line.section(':', 1).trimmed();
        }
    }
    flushPackage();
    return true;
}

void MainWindow::showAurPackageInfo(const QString &packageName)
{
    const QString paruPath = getParuPath();
    if (paruPath.isEmpty()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("paru is not installed.\n\nInstall it with: pacman -S paru"));
        return;
    }
    Cmd shell;
    QString info = shell.getOut({{"LANG", "C"}}, paruPath, {"-Si", "--color", "never", packageName});
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0 || info.trimmed().isEmpty()) {
        info = shell.getOut({{"LANG", "C"}}, "pacman", {"-Qi", "--color", "never", packageName});
    }
    QMessageBox::information(this, packageName,
                             info.trimmed().isEmpty() ? tr("No information available.") : info.trimmed());
}

// AUR's data isn't "downloaded once, rarely dirty" like the other apt tabs -- an
// empty search means "installed AUR packages" and a non-empty one means "live AUR
// search", so this always rebuilds rather than reusing buildPackageLists()'s
// cache-until-dirty model.
void MainWindow::handleAurTab(const QString &searchStr)
{
    ui->searchBoxAUR->setText(searchStr);
    changeList.clear();
    displayWarning("aur");
    if (displayPackagesIsRunning) {
        progress->show();
        if (!timer.isActive()) {
            timer.start(100ms);
        }
        connect(this, &MainWindow::displayPackagesFinished, this, &MainWindow::updateInterface,
                Qt::SingleShotConnection);
        return;
    }
    if (!buildAurList(searchStr)) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Could not query AUR packages. Please check that paru is installed and you are "
                                 "online."));
        currentTree->blockSignals(false);
        return;
    }
    displayPackages();
    if (ui->comboFilterAUR->currentIndex() != savedComboIndex) {
        ui->comboFilterAUR->setCurrentIndex(savedComboIndex);
    }
    filterChanged(ui->comboFilterAUR->currentText());
    currentTree->blockSignals(false);
}

void MainWindow::pushForceUpdateAUR_clicked()
{
    beginOperation();
    ui->searchBoxAUR->clear();
    ui->comboFilterAUR->setCurrentIndex(0);
    handleAurTab(QString());
}
#endif
