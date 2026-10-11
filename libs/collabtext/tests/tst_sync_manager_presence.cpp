#include <collabtext/SyncManager.h>

#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>

using namespace CollabText;
namespace fs = std::filesystem;

class SyncManagerPresenceTest final : public QObject {
  Q_OBJECT
private slots:
  void startProvidesPresenceWithoutCreatingPseudoOperationCursors() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const fs::path root = temp.path().toStdString();
    Identity::Identity identity;
    identity.identity_id = "presence-only-user";
    identity.display_name = "Presence Only";
    identity.color = "#336699";

    SyncManager manager(nullptr, identity, "presence-device", "Test Device");
    manager.start(QString::fromStdString(root.string()));
    QVERIFY(manager.isRunning());
    manager.stop();
    QVERIFY(!manager.isRunning());

    QVERIFY(!fs::exists(root / "replicas" / "presence-device" / "ops"));
    QVERIFY(!fs::exists(root / "replicas" / "presence-device" / "seq"));
    QVERIFY(!fs::exists(root / "local" / "presence-device" / "cursors"));
    QVERIFY(fs::exists(root / ".stignore"));
  }
};

QTEST_GUILESS_MAIN(SyncManagerPresenceTest)
#include "tst_sync_manager_presence.moc"
