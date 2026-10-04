#include "remotes.h"

#include <QDebug>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include "cmd.h"

ManageRemotes::ManageRemotes(QWidget *parent, const QString &user)
    : QDialog(parent),
      user(user)
{
    setWindowTitle(tr("Manage Flatpak Remotes"));
    changed = false;

    auto *layout = new QGridLayout();
    setLayout(layout);

    comboUser = new QComboBox(this);
    comboUser->addItem(tr("For all users"));
    comboUser->addItem(tr("For current user"));
    if (user == QLatin1String("--system ")) {
        comboUser->setCurrentIndex(0);
    } else {
        comboUser->setCurrentIndex(1);
    }

    comboRemote = new QComboBox(this);

    editAddRemote = new QLineEdit(this);
    editAddRemote->setMinimumWidth(400);
    editAddRemote->setPlaceholderText(tr("enter Flatpak remote URL"));

    editInstallFlatpakref = new QLineEdit(this);
    editInstallFlatpakref->setPlaceholderText(tr("enter Flatpakref location to install app"));

    auto *label = new QLabel(tr("Add or remove flatpak remotes (repos), or install apps using flatpakref URL or path"));
    layout->addWidget(label, 0, 0, 1, 5);
    label->setAlignment(Qt::AlignCenter);

    layout->addWidget(comboUser, 1, 0, 1, 4);
    layout->addWidget(comboRemote, 2, 0, 1, 4);
    layout->addWidget(editAddRemote, 3, 0, 1, 4);
    layout->addWidget(editInstallFlatpakref, 4, 0, 1, 4);

    auto *remove = new QPushButton(tr("Remove remote"));
    remove->setIcon(QIcon::fromTheme("remove"));
    remove->setAutoDefault(false);
    layout->addWidget(remove, 2, 4, 1, 1);

    auto *add = new QPushButton(tr("Add remote"));
    add->setIcon(QIcon::fromTheme("add"));
    add->setAutoDefault(false);
    layout->addWidget(add, 3, 4, 1, 1);

    auto *install = new QPushButton(tr("Install app"));
    install->setIcon(QIcon::fromTheme("run-install"));
    install->setAutoDefault(false);
    layout->addWidget(install, 4, 4, 1, 1);

    auto *cancel = new QPushButton(tr("Close"));
    cancel->setIcon(QIcon::fromTheme("window-close"));
    cancel->setAutoDefault(false);
    layout->addWidget(cancel, 5, 4, 1, 1);

    connect(cancel, &QPushButton::clicked, this, &ManageRemotes::close);
    connect(remove, &QPushButton::clicked, this, &ManageRemotes::removeItem);
    connect(add, &QPushButton::clicked, this, &ManageRemotes::addItem);
    connect(install, &QPushButton::clicked, this, &ManageRemotes::setInstall);
    connect(comboUser, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ManageRemotes::userSelected);

    listFlatpakRemotes();
}

void ManageRemotes::removeItem()
{
    const QString remote = comboRemote->currentText().section(" -- ", 0, 0);
    if (remote.isEmpty()) {
        return;
    }
    if (remote == QLatin1String("flathub")) {
        QMessageBox::information(this, tr("Not removable"),
                                 tr("Flathub is the main Flatpak remote and won't be removed"));
        return;
    }

    // The second remote-list column is the comma-separated Options field: the
    // installation first, then flags such as no-gpg-verify, oci or disabled.
    const QString installation = comboRemote->currentText().section(" -- ", 1, 1).section(',', 0, 0);
    QStringList args {"remote-delete", remote};
    if (installation == QLatin1String("system") || installation == QLatin1String("user")) {
        args << ("--" + installation);
    } else if (!installation.isEmpty()) {
        args << ("--installation=" + installation);
    }

    setCursor(QCursor(Qt::BusyCursor));
    const bool removed = Cmd().proc("flatpak", args);
    setCursor(QCursor(Qt::ArrowCursor));
    if (!removed) {
        QMessageBox::critical(this, tr("Error removing remote"),
                              tr("Could not remove remote - command returned an error. Please try again."));
        return;
    }

    changed = true;
    listFlatpakRemotes();
}

void ManageRemotes::addItem()
{
    setCursor(QCursor(Qt::BusyCursor));
    QString location = editAddRemote->text();
    QString name = editAddRemote->text().section('/', -1).section('.', 0, 0); // obtain the name before .flatpakremo

    QStringList addArgs {"remote-add", user.trimmed(), "--if-not-exists", name, location};
    if (!Cmd().proc("flatpak", addArgs)) {
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::critical(this, tr("Error adding remote"),
                              tr("Could not add remote - command returned an error. Please double-check the remote "
                                 "address and try again"));
    } else {
        changed = true;
        setCursor(QCursor(Qt::ArrowCursor));
        QMessageBox::information(this, tr("Success"), tr("Remote added successfully"));
        editAddRemote->clear();
        comboRemote->addItem(name);
    }
}

void ManageRemotes::setInstall()
{
    installRef = editInstallFlatpakref->text();
    close();
}

void ManageRemotes::userSelected(int index)
{
    if (index == 0) {
        user = QStringLiteral("--system ");
    } else {
        user = QStringLiteral("--user ");
        setCursor(QCursor(Qt::BusyCursor));
        Cmd().proc("flatpak", {"--user", "remote-add", "--if-not-exists", "flathub",
                               "https://flathub.org/repo/flathub.flatpakrepo"});
        setCursor(QCursor(Qt::ArrowCursor));
    }
    listFlatpakRemotes();
}

// List the flatpak remote and load them into combobox
void ManageRemotes::listFlatpakRemotes() const
{
    qDebug() << "+++" << __PRETTY_FUNCTION__ << "+++";
    comboRemote->clear();
    QStringList list = Cmd().getOut("flatpak", {"remote-list"}).replace('\t', " -- ").split('\n');
    comboRemote->addItems(list);
}
