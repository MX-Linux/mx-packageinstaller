#include <QtTest>
#include <QSignalSpy>
#include <QStandardPaths>
#include "../src/models/packagemodel.h"

class TestPackageModel : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Basic model tests
    void testEmptyModel();
    void testSetPackageData();
    void testRowColumnCount();

    // Data retrieval tests
    void testDataDisplayRole();
    void testDataCheckStateRole();
    void testDataUserRole();

    // Check state tests
    void testCheckStateChange();
    void testCheckStateSignal();
    void testSetAllChecked();
    void testCheckedPackages();
    void testSetCheckedForVisible();
    void testCheckByStatus();

    // Package lookup tests
    void testFindPackageRow();
    void testPackageAt();
    void testPackageAtInvalidRow();

    // Status tests
    void testCountByStatus();
    void testSetAutoremovable();
    void testAutoremovableKnownFollowsStatusResets();
    void testUpdateInstalledVersions();

    // Model reset tests
    void testClear();

    // Flags tests
    void testItemFlags();

    // Header tests
    void testHeaderData();

private:
    QVector<PackageData> createTestPackages();
};

void TestPackageModel::initTestCase()
{
    qDebug() << "Starting PackageModel tests";
}

void TestPackageModel::cleanupTestCase()
{
    qDebug() << "Finished PackageModel tests";
}

QVector<PackageData> TestPackageModel::createTestPackages()
{
    QVector<PackageData> packages;

    PackageData pkg1;
    pkg1.name = "vim";
    pkg1.repoVersion = "9.0.0-1";
    pkg1.installedVersion = "8.2.0-1";
    pkg1.description = "Vi IMproved - enhanced vi editor";
    pkg1.status = Status::Upgradable;
    packages.append(pkg1);

    PackageData pkg2;
    pkg2.name = "firefox";
    pkg2.repoVersion = "120.0-1";
    pkg2.installedVersion = "120.0-1";
    pkg2.description = "Mozilla Firefox web browser";
    pkg2.status = Status::Installed;
    packages.append(pkg2);

    PackageData pkg3;
    pkg3.name = "gimp";
    pkg3.repoVersion = "2.10.0-1";
    pkg3.installedVersion.clear();
    pkg3.description = "GNU Image Manipulation Program";
    pkg3.status = Status::NotInstalled;
    packages.append(pkg3);

    return packages;
}

void TestPackageModel::testEmptyModel()
{
    PackageModel model;
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.columnCount(), TreeCol::Status + 1);
    QVERIFY(model.checkedPackages().isEmpty());
}

void TestPackageModel::testSetPackageData()
{
    PackageModel model;
    QVector<PackageData> packages = createTestPackages();

    model.setPackageData(packages);

    QCOMPARE(model.rowCount(), 3);
}

void TestPackageModel::testRowColumnCount()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.columnCount(), TreeCol::Status + 1);

    // Parent should return 0 for table models
    QModelIndex parent = model.index(0, 0);
    QCOMPARE(model.rowCount(parent), 0);
    QCOMPARE(model.columnCount(parent), 0);
}

void TestPackageModel::testDataDisplayRole()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Test name column
    QModelIndex nameIndex = model.index(0, TreeCol::Name);
    QCOMPARE(model.data(nameIndex, Qt::DisplayRole).toString(), QString("vim"));

    // Test repo version column
    QModelIndex repoIndex = model.index(0, TreeCol::RepoVersion);
    QCOMPARE(model.data(repoIndex, Qt::DisplayRole).toString(), QString("9.0.0-1"));

    // Test installed version column
    QModelIndex installedIndex = model.index(0, TreeCol::InstalledVersion);
    QCOMPARE(model.data(installedIndex, Qt::DisplayRole).toString(), QString("8.2.0-1"));

    // Test description column
    QModelIndex descIndex = model.index(0, TreeCol::Description);
    QCOMPARE(model.data(descIndex, Qt::DisplayRole).toString(), QString("Vi IMproved - enhanced vi editor"));
}

void TestPackageModel::testDataCheckStateRole()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QModelIndex checkIndex = model.index(0, TreeCol::Check);
    QCOMPARE(model.data(checkIndex, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));

    // Non-check column should return empty
    QModelIndex nameIndex = model.index(0, TreeCol::Name);
    QVERIFY(!model.data(nameIndex, Qt::CheckStateRole).isValid());
}

void TestPackageModel::testDataUserRole()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QModelIndex statusIndex = model.index(0, TreeCol::Status);
    QCOMPARE(model.data(statusIndex, Qt::UserRole).toInt(), Status::Upgradable);

    QModelIndex statusIndex2 = model.index(1, TreeCol::Status);
    QCOMPARE(model.data(statusIndex2, Qt::UserRole).toInt(), Status::Installed);
}

void TestPackageModel::testCheckStateChange()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QModelIndex checkIndex = model.index(0, TreeCol::Check);

    // Initially unchecked
    QCOMPARE(model.data(checkIndex, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));

    // Set to checked
    bool result = model.setData(checkIndex, Qt::Checked, Qt::CheckStateRole);
    QVERIFY(result);
    QCOMPARE(model.data(checkIndex, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));

    // Set back to unchecked
    result = model.setData(checkIndex, Qt::Unchecked, Qt::CheckStateRole);
    QVERIFY(result);
    QCOMPARE(model.data(checkIndex, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
}

void TestPackageModel::testCheckStateSignal()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QSignalSpy spy(&model, &PackageModel::checkStateChanged);
    QVERIFY(spy.isValid());

    QModelIndex checkIndex = model.index(0, TreeCol::Check);
    model.setData(checkIndex, Qt::Checked, Qt::CheckStateRole);

    QCOMPARE(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QString("vim"));
    QCOMPARE(args.at(1).value<Qt::CheckState>(), Qt::Checked);
}

void TestPackageModel::testSetAllChecked()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Check all
    model.setAllChecked(true);

    for (int i = 0; i < model.rowCount(); ++i) {
        QModelIndex idx = model.index(i, TreeCol::Check);
        QCOMPARE(model.data(idx, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
    }

    // Uncheck all
    model.setAllChecked(false);

    for (int i = 0; i < model.rowCount(); ++i) {
        QModelIndex idx = model.index(i, TreeCol::Check);
        QCOMPARE(model.data(idx, Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    }
}

void TestPackageModel::testCheckedPackages()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Initially empty
    QVERIFY(model.checkedPackages().isEmpty());

    // Check first and third packages
    model.setData(model.index(0, TreeCol::Check), Qt::Checked, Qt::CheckStateRole);
    model.setData(model.index(2, TreeCol::Check), Qt::Checked, Qt::CheckStateRole);

    QStringList checked = model.checkedPackages();
    QCOMPARE(checked.size(), 2);
    QVERIFY(checked.contains("vim"));
    QVERIFY(checked.contains("gimp"));
    QVERIFY(!checked.contains("firefox"));
}

void TestPackageModel::testSetCheckedForVisible()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Check only rows 0 and 2 (simulating visible rows in a filtered view)
    QVector<int> visibleRows = {0, 2};
    model.setCheckedForVisible(visibleRows, true);

    QCOMPARE(model.data(model.index(0, TreeCol::Check), Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
    QCOMPARE(model.data(model.index(1, TreeCol::Check), Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Unchecked));
    QCOMPARE(model.data(model.index(2, TreeCol::Check), Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
}

void TestPackageModel::testCheckByStatus()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Check only upgradable packages (row 0 is upgradable)
    model.checkByStatus(Status::Upgradable, true);

    QCOMPARE(model.data(model.index(0, TreeCol::Check), Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
    QCOMPARE(model.data(model.index(1, TreeCol::Check), Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Unchecked));
    QCOMPARE(model.data(model.index(2, TreeCol::Check), Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Unchecked));
}

void TestPackageModel::testFindPackageRow()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QCOMPARE(model.findPackageRow("vim"), 0);
    QCOMPARE(model.findPackageRow("firefox"), 1);
    QCOMPARE(model.findPackageRow("gimp"), 2);
    QCOMPARE(model.findPackageRow("nonexistent"), -1);
}

void TestPackageModel::testPackageAt()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    const PackageData *pkg = model.packageAt(0);
    QVERIFY(pkg != nullptr);
    QCOMPARE(pkg->name, QString("vim"));
    QCOMPARE(pkg->repoVersion, QString("9.0.0-1"));

    const PackageData *pkg2 = model.packageAt(2);
    QVERIFY(pkg2 != nullptr);
    QCOMPARE(pkg2->name, QString("gimp"));
}

void TestPackageModel::testPackageAtInvalidRow()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QVERIFY(model.packageAt(-1) == nullptr);
    QVERIFY(model.packageAt(100) == nullptr);
}

void TestPackageModel::testCountByStatus()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QCOMPARE(model.countByStatus(Status::Upgradable), 1);
    QCOMPARE(model.countByStatus(Status::Installed), 1);
    QCOMPARE(model.countByStatus(Status::NotInstalled), 1);
    QCOMPARE(model.countByStatus(Status::Autoremovable), 0);
}

void TestPackageModel::testSetAutoremovable()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Mark firefox as autoremovable
    model.setAutoremovable({"firefox"});

    QCOMPARE(model.countByStatus(Status::Autoremovable), 1);

    const PackageData *pkg = model.packageAt(1);
    QVERIFY(pkg != nullptr);
    QCOMPARE(pkg->status, Status::Autoremovable);
}

// The Autoremovable filter computes the statuses on demand when the model reports
// them unknown, so every path that recomputes statuses must reset that state, and
// an empty result must still count as computed.
void TestPackageModel::testAutoremovableKnownFollowsStatusResets()
{
    PackageModel model;
    model.setPackageData(createTestPackages());
    QVERIFY(!model.autoremovableKnown());

    model.setAutoremovable({});
    QVERIFY(model.autoremovableKnown());

    model.updateInstalledVersions({});
    QVERIFY(!model.autoremovableKnown());

    model.setAutoremovable({"firefox"});
    model.setPackageData(createTestPackages());
    QVERIFY(!model.autoremovableKnown());

    model.setAutoremovable({"firefox"});
    model.clear();
    QVERIFY(!model.autoremovableKnown());
}

void TestPackageModel::testUpdateInstalledVersions()
{
#ifdef PACKAGE_BACKEND_PACMAN
    // On this backend, updateInstalledVersions() -> PackageBackend::compareVersions()
    // shells out to the real `vercmp` (see packagebackend_pacman.cpp); skip rather
    // than fail if this environment doesn't have it (it ships with pacman itself,
    // so a real Arch install always does).
    if (QStandardPaths::findExecutable(QStringLiteral("vercmp")).isEmpty()) {
        QSKIP("vercmp not found on PATH");
    }
#endif

    PackageModel model;
    QVector<PackageData> packages = createTestPackages();

    PackageData pkg4;
    pkg4.name = "mx-packageinstaller";
    pkg4.repoVersion = "26.01";
    pkg4.installedVersion = "26.01.2";
    pkg4.description = "MX Package Installer";
    pkg4.status = Status::Installed;
    packages.append(pkg4);

    model.setPackageData(packages);

    // Update versions - vim is older, gimp is installed, mx-packageinstaller is newer than repo
    QHash<QString, QString> versions;
    versions["vim"] = "8.2.0-1";  // Older than repo -> upgradable
    versions["gimp"] = "2.10.0-1";  // Now installed
    versions["mx-packageinstaller"] = "26.01.2";  // Newer than repo -> not upgradable
    // firefox is removed from the hash, simulating uninstallation

    model.updateInstalledVersions(versions);

    const PackageData *vim = model.packageAt(0);
    QCOMPARE(vim->status, Status::Upgradable);  // Repo is newer than installed

    const PackageData *firefox = model.packageAt(1);
    QCOMPARE(firefox->status, Status::NotInstalled);  // Was installed, now not

    const PackageData *gimp = model.packageAt(2);
    QCOMPARE(gimp->status, Status::Installed);  // Was not installed, now installed

    const PackageData *mxpi = model.packageAt(3);
    QCOMPARE(mxpi->status, Status::Installed);  // Installed version is newer than repo
}

void TestPackageModel::testClear()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    QCOMPARE(model.rowCount(), 3);

    model.clear();

    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.checkedPackages().isEmpty());
    QCOMPARE(model.findPackageRow("vim"), -1);
}

void TestPackageModel::testItemFlags()
{
    PackageModel model;
    model.setPackageData(createTestPackages());

    // Check column should be checkable
    QModelIndex checkIndex = model.index(0, TreeCol::Check);
    Qt::ItemFlags checkFlags = model.flags(checkIndex);
    QVERIFY(checkFlags & Qt::ItemIsEnabled);
    QVERIFY(checkFlags & Qt::ItemIsSelectable);
    QVERIFY(checkFlags & Qt::ItemIsUserCheckable);

    // Name column should not be checkable
    QModelIndex nameIndex = model.index(0, TreeCol::Name);
    Qt::ItemFlags nameFlags = model.flags(nameIndex);
    QVERIFY(nameFlags & Qt::ItemIsEnabled);
    QVERIFY(nameFlags & Qt::ItemIsSelectable);
    QVERIFY(!(nameFlags & Qt::ItemIsUserCheckable));

    // Invalid index should have no flags
    QModelIndex invalidIndex;
    QCOMPARE(model.flags(invalidIndex), Qt::NoItemFlags);
}

void TestPackageModel::testHeaderData()
{
    PackageModel model;

    // Check header labels
    QCOMPARE(model.headerData(TreeCol::Name, Qt::Horizontal, Qt::DisplayRole).toString(),
             QString("Package"));
    QCOMPARE(model.headerData(TreeCol::RepoVersion, Qt::Horizontal, Qt::DisplayRole).toString(),
             QString("Repo Version"));
    QCOMPARE(model.headerData(TreeCol::InstalledVersion, Qt::Horizontal, Qt::DisplayRole).toString(),
             QString("Installed"));
    QCOMPARE(model.headerData(TreeCol::Description, Qt::Horizontal, Qt::DisplayRole).toString(),
             QString("Description"));

    // Check column should have empty header
    QVERIFY(model.headerData(TreeCol::Check, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());

    // Vertical orientation should return empty
    QVERIFY(!model.headerData(0, Qt::Vertical, Qt::DisplayRole).isValid());
}

QTEST_MAIN(TestPackageModel)
#include "test_packagemodel.moc"
