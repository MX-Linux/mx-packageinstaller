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
#include "packagemodel.h"

#include "../versionnumber.h"
#ifdef PACKAGE_BACKEND_PACMAN
#include "../packagebackend.h"
#endif

PackageModel::PackageModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int PackageModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_packages.size());
}

int PackageModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return TreeCol::Status + 1;
}

QVariant PackageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_packages.size()) {
        return {};
    }

    const PackageData &pkg = m_packages.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case TreeCol::Name:
            return pkg.name;
        case TreeCol::RepoVersion:
            return pkg.repoVersion;
        case TreeCol::InstalledVersion:
            return pkg.installedVersion;
        case TreeCol::Description:
            return pkg.description;
        default:
            return {};
        }

    case Qt::CheckStateRole:
        if (index.column() == TreeCol::Check) {
            return pkg.checkState;
        }
        return {};

    case Qt::DecorationRole:
        if (index.column() == TreeCol::Check) {
            if (pkg.status == Status::Upgradable) {
                return m_iconUpgradable;
            }
            if (pkg.status == Status::Installed || pkg.status == Status::Autoremovable) {
                return m_iconInstalled;
            }
        }
        return {};

    case Qt::UserRole:
        if (index.column() == TreeCol::Status) {
            return pkg.status;
        }
        return {};

    default:
        return {};
    }
}

bool PackageModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() >= m_packages.size()) {
        return false;
    }

    if (role == Qt::CheckStateRole && index.column() == TreeCol::Check) {
        PackageData &pkg = m_packages[index.row()];
        auto newState = static_cast<Qt::CheckState>(value.toInt());
        if (pkg.checkState != newState) {
            pkg.checkState = newState;
            emit dataChanged(index, index, {Qt::CheckStateRole});
            emit checkStateChanged(pkg.name, newState);
            return true;
        }
    }

    return false;
}

Qt::ItemFlags PackageModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

    if (index.column() == TreeCol::Check) {
        flags |= Qt::ItemIsUserCheckable;
    }

    return flags;
}

QVariant PackageModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case TreeCol::Check:
        return QString();
    case TreeCol::Name:
        return tr("Package");
    case TreeCol::RepoVersion:
        return tr("Repo Version");
    case TreeCol::InstalledVersion:
        return tr("Installed");
    case TreeCol::Description:
        return tr("Description");
    case TreeCol::Status:
        return QString();
    default:
        return {};
    }
}

void PackageModel::setPackageData(const QVector<PackageData> &packages)
{
    beginResetModel();
    m_packages = packages;
    m_nameToRow.clear();
    m_nameToRow.reserve(m_packages.size());
    m_countInstalled = 0;
    m_countUpgradable = 0;
    m_autoremovableKnown = false;
    for (int i = 0; i < m_packages.size(); ++i) {
        m_nameToRow.insert(m_packages.at(i).name, i);
        if (m_packages.at(i).status == Status::Installed) {
            ++m_countInstalled;
        } else if (m_packages.at(i).status == Status::Upgradable) {
            ++m_countUpgradable;
        }
    }
    endResetModel();
}

void PackageModel::clear()
{
    beginResetModel();
    m_packages.clear();
    m_nameToRow.clear();
    m_countInstalled = 0;
    m_countUpgradable = 0;
    m_autoremovableKnown = false;
    endResetModel();
}

QStringList PackageModel::checkedPackages() const
{
    QStringList result;
    for (const PackageData &pkg : m_packages) {
        if (pkg.checkState == Qt::Checked) {
            result.append(pkg.name);
        }
    }
    return result;
}

void PackageModel::setAllChecked(bool checked)
{
    // beginResetModel()/endResetModel() instead of one dataChanged() over every row:
    // same fix as updateInstalledVersions() (see its comment) -- a dynamicSortFilter
    // proxy re-filters/re-sorts incrementally on a whole-range dataChanged, which is
    // measurably slower than one clean resort on large lists.
    beginResetModel();
    Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].checkState != state) {
            m_packages[i].checkState = state;
        }
    }
    endResetModel();
}

void PackageModel::setCheckedForVisible(const QVector<int> &visibleRows, bool checked)
{
    Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    for (int row : visibleRows) {
        if (row >= 0 && row < m_packages.size()) {
            if (m_packages[row].checkState != state) {
                m_packages[row].checkState = state;
                emit dataChanged(index(row, TreeCol::Check), index(row, TreeCol::Check),
                                 {Qt::CheckStateRole});
            }
        }
    }
}

const PackageData *PackageModel::packageAt(int row) const
{
    if (row >= 0 && row < m_packages.size()) {
        return &m_packages.at(row);
    }
    return nullptr;
}

int PackageModel::findPackageRow(const QString &name) const
{
    return m_nameToRow.value(name, -1);
}

void PackageModel::setAutoremovable(const QStringList &names)
{
    m_autoremovableKnown = true;
    QSet<QString> nameSet(names.begin(), names.end());
    for (int i = 0; i < m_packages.size(); ++i) {
        if (nameSet.contains(m_packages[i].name)) {
            if (m_packages[i].status == Status::Installed) {
                --m_countInstalled;
            } else if (m_packages[i].status == Status::Upgradable) {
                --m_countUpgradable;
            }
            m_packages[i].status = Status::Autoremovable;
            emit dataChanged(index(i, TreeCol::Check), index(i, TreeCol::Status));
        }
    }
}

void PackageModel::updateInstalledVersions(const QHash<QString, QString> &versions)
{
    // A single dataChanged() over the whole range makes a dynamicSortFilter-enabled
    // proxy incrementally re-filter/re-sort every row as it comes in -- fine for a
    // handful of rows, but a measured ~700ms+ on a full pacman repo listing (tens
    // of thousands of rows). setPackageData() (called right before this, always)
    // already does a full reset, so there's no extra view state lost by doing the
    // same here -- and a plain reset lets the proxy do one clean resort instead.
    beginResetModel();
    m_countInstalled = 0;
    m_countUpgradable = 0;
    m_autoremovableKnown = false;
    for (int i = 0; i < m_packages.size(); ++i) {
        PackageData &pkg = m_packages[i];
        auto it = versions.find(pkg.name);
        if (it != versions.end()) {
            pkg.installedVersion = it.value();
            if (!pkg.repoVersion.isEmpty()) {
#ifdef PACKAGE_BACKEND_PACMAN
                // pacman's version-ordering rules (tilde, unmarked suffixes, ...)
                // genuinely disagree with dpkg's in places -- see
                // PackageBackend::compareVersions()'s doc comment -- so this
                // must not go through VersionNumber.
                pkg.status = (PackageBackend::compareVersions(pkg.repoVersion, pkg.installedVersion) > 0)
                                 ? Status::Upgradable
                                 : Status::Installed;
#else
                const VersionNumber repoVersion(pkg.repoVersion);
                const VersionNumber installedVersion(pkg.installedVersion);
                pkg.status = (repoVersion > installedVersion) ? Status::Upgradable : Status::Installed;
#endif
            } else {
                pkg.status = Status::Installed;
            }
        } else {
            pkg.installedVersion.clear();
            pkg.status = Status::NotInstalled;
        }
        if (pkg.status == Status::Installed) {
            ++m_countInstalled;
        } else if (pkg.status == Status::Upgradable) {
            ++m_countUpgradable;
        }
    }

    endResetModel();
}

void PackageModel::setIcons(const QIcon &installed, const QIcon &upgradable)
{
    m_iconInstalled = installed;
    m_iconUpgradable = upgradable;
}

int PackageModel::countByStatus(int status) const
{
    switch (status) {
    case Status::Installed:
        return m_countInstalled;
    case Status::Upgradable:
        return m_countUpgradable;
    default:
        // For other statuses, still iterate (less common)
        int count = 0;
        for (const PackageData &pkg : m_packages) {
            if (pkg.status == status) {
                ++count;
            }
        }
        return count;
    }
}

QStringList PackageModel::checkedPackageNames() const
{
    return checkedPackages();
}

void PackageModel::uncheckAll()
{
    setAllChecked(false);
}

void PackageModel::checkByStatus(int status, bool checked)
{
    Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].status == status || (status == Status::Installed && m_packages[i].status == Status::Upgradable)) {
            m_packages[i].checkState = state;
        }
    }
    if (!m_packages.isEmpty()) {
        emit dataChanged(index(0, TreeCol::Check), index(m_packages.size() - 1, TreeCol::Check),
                         {Qt::CheckStateRole});
    }
}
