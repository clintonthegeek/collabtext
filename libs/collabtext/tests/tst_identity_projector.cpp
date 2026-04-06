#include <QTest>
#include <QTemporaryDir>
#include "collabtext/IdentityProjector.h"

using namespace CollabText::Identity;
namespace fs = std::filesystem;

class TestIdentityProjector : public QObject {
    Q_OBJECT

private slots:
    void project_creates_profile_json() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "alice-04e1c9";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";

        proj.project(id);
        fs::path profile = shared / "identities" / "alice-04e1c9" / "profile.json";
        QVERIFY(fs::exists(profile));
    }

    void project_then_read_roundtrip() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "bob-ff1234";
        id.display_name = "Bob";
        id.color = "#ef4444";
        id.status = "Testing";
        id.updated = "2026-04-06T13:00:00Z";

        proj.project(id);
        auto loaded = proj.read("bob-ff1234");
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->display_name, std::string("Bob"));
        QCOMPARE(loaded->color, std::string("#ef4444"));
        QCOMPARE(loaded->status, std::string("Testing"));
    }

    void read_all_returns_multiple_identities() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity alice;
        alice.identity_id = "alice-111111";
        alice.display_name = "Alice";
        alice.color = "#22c55e";
        alice.updated = "2026-04-06T12:00:00Z";

        Identity bob;
        bob.identity_id = "bob-222222";
        bob.display_name = "Bob";
        bob.color = "#ef4444";
        bob.updated = "2026-04-06T12:00:00Z";

        proj.project(alice);
        proj.project(bob);
        auto all = proj.read_all();
        QCOMPARE(all.size(), size_t(2));
    }

    void read_nonexistent_returns_nullopt() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);
        QVERIFY(!proj.read("nobody-000000").has_value());
    }

    void project_skips_when_not_newer() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "alice-04e1c9";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";
        proj.project(id);

        id.display_name = "Alice Updated";
        proj.project(id); // same timestamp — should NOT rewrite

        auto loaded = proj.read("alice-04e1c9");
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->display_name, std::string("Alice"));
    }

    void project_updates_when_newer() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        Identity id;
        id.identity_id = "alice-04e1c9";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";
        proj.project(id);

        id.display_name = "Alice Updated";
        id.updated = "2026-04-06T13:00:00Z";
        proj.project(id);

        auto loaded = proj.read("alice-04e1c9");
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->display_name, std::string("Alice Updated"));
    }

    void project_avatar_writes_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        IdentityProjector proj(shared);

        std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47};
        proj.project_avatar("alice-04e1c9", data);

        fs::path avatar = shared / "identities" / "alice-04e1c9" / "avatar.png";
        QVERIFY(fs::exists(avatar));
    }
};

QTEST_MAIN(TestIdentityProjector)
#include "tst_identity_projector.moc"
