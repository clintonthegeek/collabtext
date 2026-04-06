#include <QTest>
#include <QTemporaryDir>
#include "collabtext/PresenceManager.h"

#include <fstream>

using namespace CollabText::Identity;
namespace Crdt = CollabText::Crdt;
namespace fs = std::filesystem;

class TestPresenceManager : public QObject {
    Q_OBJECT

private slots:
    void write_presence_creates_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "laptop-3");

        PresenceManager pm(shared, "laptop-3", "clinton-a7f3b2");

        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "ThinkPad";
        p.active = true;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        p.session_started = "2026-04-06T12:00:00Z";

        pm.write_presence(p);
        QVERIFY(fs::exists(shared / "replicas" / "laptop-3" / "presence.json"));
    }

    void write_ephemeral_creates_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "laptop-3");

        PresenceManager pm(shared, "laptop-3", "clinton-a7f3b2");

        EphemeralState es;
        es.seq = 1;
        es.timestamp = "2026-04-06T14:30:00Z";
        es.activity = "idle";

        pm.write_ephemeral(es);
        QVERIFY(fs::exists(shared / "replicas" / "laptop-3" / "ephemeral.json"));
    }

    void read_remote_presences_skips_own() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "laptop-3");
        fs::create_directories(shared / "replicas" / "desktop-1");

        PresenceManager pmA(shared, "laptop-3", "clinton-a7f3b2");
        PresenceManager pmB(shared, "desktop-1", "alice-04e1c9");

        Presence pA;
        pA.replica_id = "laptop-3";
        pA.identity_id = "clinton-a7f3b2";
        pA.device_name = "ThinkPad";
        pA.active = true;
        pA.last_heartbeat = "2026-04-06T14:30:00Z";
        pA.session_started = "2026-04-06T12:00:00Z";
        pmA.write_presence(pA);

        Presence pB;
        pB.replica_id = "desktop-1";
        pB.identity_id = "alice-04e1c9";
        pB.device_name = "Desktop";
        pB.active = true;
        pB.last_heartbeat = "2026-04-06T14:30:00Z";
        pB.session_started = "2026-04-06T12:00:00Z";
        pmB.write_presence(pB);

        auto remotes = pmA.read_remote_presences();
        QCOMPARE(remotes.size(), size_t(1));
        QCOMPARE(remotes[0].first, std::string("desktop-1"));
        QCOMPARE(remotes[0].second.identity_id, std::string("alice-04e1c9"));
    }

    void read_remote_ephemerals_skips_own() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "a");
        fs::create_directories(shared / "replicas" / "b");

        PresenceManager pmA(shared, "a", "id-a");
        PresenceManager pmB(shared, "b", "id-b");

        EphemeralState es;
        es.seq = 1;
        es.timestamp = "2026-04-06T14:30:00Z";
        es.activity = "typing";
        es.cursors.push_back({
            Crdt::Anchor(1, 10, Crdt::Bias::Right),
            Crdt::Anchor(1, 10, Crdt::Bias::Right)
        });

        pmA.write_ephemeral(es);
        pmB.write_ephemeral(es);

        auto remotes = pmA.read_remote_ephemerals();
        QCOMPARE(remotes.size(), size_t(1));
        QCOMPARE(remotes[0].first, std::string("b"));
    }

    void is_live_checks_heartbeat_and_active() {
        Presence p;
        p.active = true;

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&time_t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        p.last_heartbeat = buf;
        QVERIFY(PresenceManager::is_live(p));
        QVERIFY(!PresenceManager::is_stale(p));
        QVERIFY(!PresenceManager::is_departed(p));
    }

    void is_stale_with_old_heartbeat() {
        Presence p;
        p.active = true;
        p.last_heartbeat = "2020-01-01T00:00:00Z";
        QVERIFY(!PresenceManager::is_live(p));
        QVERIFY(PresenceManager::is_stale(p));
    }

    void is_departed_when_inactive() {
        Presence p;
        p.active = false;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        QVERIFY(PresenceManager::is_departed(p));
        QVERIFY(!PresenceManager::is_live(p));
    }

    void depart_sets_active_false() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "laptop-3");

        PresenceManager pm(shared, "laptop-3", "clinton-a7f3b2");

        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "ThinkPad";
        p.active = true;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        p.session_started = "2026-04-06T12:00:00Z";
        pm.write_presence(p);

        pm.depart();

        auto remotes = PresenceManager(shared, "other", "other").read_remote_presences();
        QCOMPARE(remotes.size(), size_t(1));
        QCOMPARE(remotes[0].second.active, false);
    }

    void malformed_presence_file_skipped() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        fs::create_directories(shared / "replicas" / "bad");

        std::ofstream f(shared / "replicas" / "bad" / "presence.json");
        f << "not json at all";
        f.close();

        PresenceManager pm(shared, "good", "id");
        auto remotes = pm.read_remote_presences();
        QCOMPARE(remotes.size(), size_t(0));
    }
};

QTEST_MAIN(TestPresenceManager)
#include "tst_presence_manager.moc"
