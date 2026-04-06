#include <QTest>
#include "collabtext/Identity.h"

using namespace CollabText::Identity;
namespace Crdt = CollabText::Crdt;

class TestIdentityJson : public QObject {
    Q_OBJECT

private slots:
    void identity_roundtrip() {
        Identity id;
        id.identity_id = "clinton-a7f3b2";
        id.display_name = "Clinton";
        id.status = "Drafting the sync spec";
        id.bio = "Systems programmer.";
        id.color = "#3b82f6";
        id.public_key = "";
        id.updated = "2026-04-06T12:00:00Z";

        auto json = to_json(id);
        auto parsed = identity_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->identity_id, id.identity_id);
        QCOMPARE(parsed->display_name, id.display_name);
        QCOMPARE(parsed->status, id.status);
        QCOMPARE(parsed->bio, id.bio);
        QCOMPARE(parsed->color, id.color);
        QCOMPARE(parsed->public_key, id.public_key);
        QCOMPARE(parsed->updated, id.updated);
    }

    void identity_with_special_chars() {
        Identity id;
        id.identity_id = "user-abc123";
        id.display_name = "Tëst \"User\" \\one";
        id.status = "line\nbreak";
        id.bio = "";
        id.color = "#ff0000";
        id.public_key = "";
        id.updated = "2026-04-06T12:00:00Z";

        auto json = to_json(id);
        auto parsed = identity_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->display_name, id.display_name);
        QCOMPARE(parsed->status, id.status);
    }

    void identity_malformed_json_returns_nullopt() {
        QVERIFY(!identity_from_json("not json").has_value());
        QVERIFY(!identity_from_json("").has_value());
        QVERIFY(!identity_from_json("{}").has_value());
    }

    void presence_roundtrip() {
        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "Clinton's ThinkPad";
        p.active = true;
        p.last_heartbeat = "2026-04-06T14:30:00.337Z";
        p.session_started = "2026-04-06T12:00:00Z";
        p.version_summary.observe(Crdt::Lamport(1, 421));
        p.version_summary.observe(Crdt::Lamport(2, 300));

        auto json = to_json(p);
        auto parsed = presence_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->replica_id, p.replica_id);
        QCOMPARE(parsed->identity_id, p.identity_id);
        QCOMPARE(parsed->device_name, p.device_name);
        QCOMPARE(parsed->active, true);
        QCOMPARE(parsed->last_heartbeat, p.last_heartbeat);
        QCOMPARE(parsed->session_started, p.session_started);
        QCOMPARE(parsed->version_summary.get(1), uint32_t(421));
        QCOMPARE(parsed->version_summary.get(2), uint32_t(300));
    }

    void presence_departed() {
        Presence p;
        p.replica_id = "laptop-3";
        p.identity_id = "clinton-a7f3b2";
        p.device_name = "ThinkPad";
        p.active = false;
        p.last_heartbeat = "2026-04-06T14:30:00Z";
        p.session_started = "2026-04-06T12:00:00Z";

        auto json = to_json(p);
        auto parsed = presence_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->active, false);
    }

    void ephemeral_roundtrip() {
        EphemeralState es;
        es.seq = 14207;
        es.timestamp = "2026-04-06T14:32:01.337Z";
        es.cursors.push_back({
            Crdt::Anchor(1, 400, Crdt::Bias::Right),
            Crdt::Anchor(1, 400, Crdt::Bias::Right)
        });
        es.selections.push_back({
            Crdt::Anchor(2, 50, Crdt::Bias::Left),
            Crdt::Anchor(2, 77, Crdt::Bias::Right)
        });
        es.viewport_top = Crdt::Anchor(1, 380, Crdt::Bias::Left);
        es.viewport_bottom = Crdt::Anchor(1, 412, Crdt::Bias::Left);
        es.activity = "typing";

        auto json = to_json(es);
        auto parsed = ephemeral_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->seq, es.seq);
        QCOMPARE(parsed->timestamp, es.timestamp);
        QCOMPARE(parsed->cursors.size(), size_t(1));
        QCOMPARE(parsed->cursors[0].anchor.replica_id, uint16_t(1));
        QCOMPARE(parsed->cursors[0].anchor.char_value, uint32_t(400));
        QCOMPARE(parsed->cursors[0].anchor.bias, Crdt::Bias::Right);
        QCOMPARE(parsed->selections.size(), size_t(1));
        QVERIFY(parsed->viewport_top.has_value());
        QCOMPARE(parsed->viewport_top->char_value, uint32_t(380));
        QVERIFY(parsed->viewport_bottom.has_value());
        QCOMPARE(parsed->activity, std::string("typing"));
    }

    void ephemeral_no_viewport() {
        EphemeralState es;
        es.seq = 1;
        es.timestamp = "2026-04-06T14:32:01Z";
        es.activity = "idle";

        auto json = to_json(es);
        auto parsed = ephemeral_from_json(json);
        QVERIFY(parsed.has_value());
        QVERIFY(!parsed->viewport_top.has_value());
        QVERIFY(!parsed->viewport_bottom.has_value());
        QCOMPARE(parsed->cursors.size(), size_t(0));
    }

    void ephemeral_malformed_returns_nullopt() {
        QVERIFY(!ephemeral_from_json("garbage").has_value());
        QVERIFY(!ephemeral_from_json("").has_value());
    }
};

QTEST_MAIN(TestIdentityJson)
#include "tst_identity_json.moc"
