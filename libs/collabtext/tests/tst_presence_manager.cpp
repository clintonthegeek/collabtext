#include <QTest>
#include <QTemporaryDir>
#include "collabtext/PresenceManager.h"

#include <chrono>
#include <fstream>
#include <iterator>

using namespace CollabText::Identity;
namespace Crdt = CollabText::Crdt;
namespace fs = std::filesystem;

namespace {
Presence make_presence(const std::string& rid = "r",
                       const std::string& iid = "i") {
    Presence p;
    p.replica_id = rid;
    p.identity_id = iid;
    p.device_name = "d";
    p.active = true;
    p.last_heartbeat = "2026-04-30T15:00:00Z";
    p.session_started = "2026-04-30T14:00:00Z";
    return p;
}
EphemeralState make_ephemeral(uint64_t seq) {
    EphemeralState es;
    es.seq = seq;
    es.timestamp = "2026-04-30T15:00:00Z";
    es.activity = "typing";
    return es;
}
}

class TestPresenceManager : public QObject {
    Q_OBJECT

private slots:
    void writes_combined_state_json() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        PresenceManager pm(shared, "r", "i");
        pm.start();
        pm.update_presence(make_presence());
        pm.update_ephemeral(make_ephemeral(1));
        pm.flush_state();
        QVERIFY(fs::exists(shared / "replicas" / "r" / "state.json"));
        QVERIFY(!fs::exists(shared / "replicas" / "r" / "presence.json"));
        QVERIFY(!fs::exists(shared / "replicas" / "r" / "ephemeral.json"));
    }

    void reads_remote_presences_and_ephemerals_from_state_json() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        PresenceManager me(shared, "r", "i");
        PresenceManager peer(shared, "peer", "j");
        me.start(); peer.start();

        peer.update_presence(make_presence("peer", "j"));
        peer.update_ephemeral(make_ephemeral(7));
        peer.flush_state();

        auto pres = me.read_remote_presences();
        QCOMPARE(pres.size(), size_t(1));
        QCOMPARE(pres[0].first, std::string("peer"));
        QCOMPARE(pres[0].second.identity_id, std::string("j"));
        auto ephs = me.read_remote_ephemerals();
        QCOMPARE(ephs.size(), size_t(1));
        QCOMPARE(ephs[0].second.seq, uint64_t(7));
    }

    void throttles_writes_to_floor_in_burst() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        PresenceManager pm(shared, "r", "i");
        pm.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        pm.update_presence(make_presence());
        pm.update_ephemeral(make_ephemeral(1));
        pm.tick(t0);                                            // first write
        pm.update_ephemeral(make_ephemeral(2));
        pm.tick(t0 + std::chrono::milliseconds(50));             // suppressed
        pm.update_ephemeral(make_ephemeral(3));
        pm.tick(t0 + std::chrono::milliseconds(300));            // allowed
        QCOMPARE(pm.write_count_for_test(), uint64_t(2));
    }

    void writes_keepalive_after_idle_ceiling() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        PresenceManager pm(shared, "r", "i");
        pm.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        pm.update_presence(make_presence());
        pm.update_ephemeral(make_ephemeral(1));
        pm.tick(t0);
        // No further updates; tick at the ceiling.
        pm.tick(t0 + std::chrono::seconds(26));
        QCOMPARE(pm.write_count_for_test(), uint64_t(2));
    }

    void depart_forces_flush_with_active_false() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        PresenceManager pm(shared, "r", "i");
        pm.start();
        pm.update_presence(make_presence());
        pm.update_ephemeral(make_ephemeral(1));
        pm.tick(std::chrono::steady_clock::time_point{});
        pm.depart();
        std::ifstream f(shared / "replicas" / "r" / "state.json", std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        QVERIFY(s.find("\"active\":false") != std::string::npos);
    }

    void start_preserves_existing_state_across_restart() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        {
            PresenceManager pm(shared, "r", "i");
            pm.start();
            pm.update_presence(make_presence());
            auto es = make_ephemeral(42);
            es.activity = "originalActivity";
            pm.update_ephemeral(es);
            pm.flush_state();
        }
        {
            PresenceManager pm2(shared, "r", "i");
            pm2.start();
            // No update — flush should preserve seq=42 from disk.
            pm2.flush_state();
            std::ifstream f(shared / "replicas" / "r" / "state.json", std::ios::binary);
            std::string s((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
            QVERIFY(s.find("\"seq\":42") != std::string::npos);
            QVERIFY(s.find("originalActivity") != std::string::npos);
        }
    }

    void is_live_within_30_seconds() {
        Presence p = make_presence();
        // active=true, but heartbeat is from 2026-04-30 — far in the past
        // relative to system clock at runtime → not live.
        QVERIFY(!PresenceManager::is_live(p));
        p.active = false;
        QVERIFY(PresenceManager::is_departed(p));
    }
};

QTEST_APPLESS_MAIN(TestPresenceManager)
#include "tst_presence_manager.moc"
