/**********************************************************************
 * Copyright (C) 2017-2026 MX Authors
 *
 * Authors: Adrian
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
#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QVector>

#include "../packagestatus.h"

namespace TreeCol
{
enum { Check, Name, RepoVersion, InstalledVersion, Description, Status };
}

struct PackageData {
    QString name;
    QString repoVersion;
    QString installedVersion;
    QString description;
    Qt::CheckState checkState = Qt::Unchecked;
    int status = Status::NotInstalled;
    bool fromRepo = false;
};

class PackageModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit PackageModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                       int role = Qt::DisplayRole) const override;

    void setPackageData(const QVector<PackageData> &packages);
    void clear();

    [[nodiscard]] QStringList checkedPackages() const;
    [[nodiscard]] QStringList checkedPackageNames() const;
    void setAllChecked(bool checked);
    void uncheckAll();
    void setCheckedForVisible(const QVector<int> &visibleRows, bool checked);
    void checkByStatus(int status, bool checked);

    [[nodiscard]] const PackageData *packageAt(int row) const;
    [[nodiscard]] int findPackageRow(const QString &name) const;

    void setAutoremovable(const QStringList &names);
    // Whether the Autoremovable statuses reflect a setAutoremovable() call made since
    // the statuses were last reset (setPackageData()/updateInstalledVersions()/clear()).
    [[nodiscard]] bool autoremovableKnown() const { return m_autoremovableKnown; }
    void updateInstalledVersions(const QHash<QString, QString> &versions);

    void setIcons(const QIcon &installed, const QIcon &upgradable);

    [[nodiscard]] int countByStatus(int status) const;

signals:
    void checkStateChanged(const QString &packageName, Qt::CheckState state);

private:
    QVector<PackageData> m_packages;
    QHash<QString, int> m_nameToRow;
    QIcon m_iconInstalled;
    QIcon m_iconUpgradable;
    int m_countInstalled = 0;
    int m_countUpgradable = 0;
    bool m_autoremovableKnown = false;
};
