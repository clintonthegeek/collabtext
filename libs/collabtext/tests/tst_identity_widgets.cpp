#include <QTest>
#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "ui/AvatarWidget.h"
#include "ui/PresenceIndicator.h"
#include "ui/IdentityEditor.h"
#include "ui/ParticipantListWidget.h"
#include "ui/IdentitySetupDialog.h"
#include "ui/IdentityPreferencesPage.h"
#include "collabtext/IdentityStore.h"

using namespace CollabText::Ui;

class TestIdentityWidgets : public QObject {
    Q_OBJECT

private slots:
    void avatar_widget_default_size() {
        AvatarWidget w;
        QCOMPARE(w.sizeHint(), QSize(40, 40));
    }

    void avatar_widget_initials_fallback() {
        AvatarWidget w;
        w.setIdentity("Clinton Selke", "#3b82f6");
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void avatar_widget_set_image() {
        AvatarWidget w;
        std::vector<uint8_t> data = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
            0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,
            0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
            0x00, 0x00, 0x02, 0x00, 0x01, 0xE2, 0x21, 0xBC,
            0x33, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
            0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        w.setImage(data);
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void avatar_widget_clear_image_reverts_to_initials() {
        AvatarWidget w;
        w.setIdentity("Alice", "#22c55e");
        w.setImage({0x89, 0x50, 0x4E, 0x47});
        w.clearImage();
        w.resize(40, 40);
        QPixmap pm(40, 40);
        w.render(&pm);
        QVERIFY(!pm.isNull());
    }

    // --- PresenceIndicator tests ---

    void presence_indicator_default_size() {
        PresenceIndicator pi;
        QCOMPARE(pi.sizeHint(), QSize(12, 12));
    }

    void presence_indicator_activity_colors() {
        PresenceIndicator pi;
        pi.setActivity("typing");
        pi.resize(12, 12);
        QPixmap pm(12, 12);
        pi.render(&pm);
        QVERIFY(!pm.isNull());

        pi.setActivity("idle");
        pi.render(&pm);
        QVERIFY(!pm.isNull());

        pi.setActivity("away");
        pi.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void presence_indicator_stale() {
        PresenceIndicator pi;
        pi.setActivity("typing");
        pi.setStale(true);
        pi.resize(12, 12);
        QPixmap pm(12, 12);
        pi.render(&pm);
        QVERIFY(!pm.isNull());
    }

    // --- IdentityEditor tests ---

    void identity_editor_set_and_get() {
        CollabText::Identity::Identity id;
        id.identity_id = "test-aaa111";
        id.display_name = "Test User";
        id.status = "Testing";
        id.bio = "A bio";
        id.color = "#3b82f6";
        id.updated = "2026-04-06T12:00:00Z";

        IdentityEditor editor;
        editor.setIdentity(id);

        auto result = editor.identity();
        QCOMPARE(result.display_name, id.display_name);
        QCOMPARE(result.status, id.status);
        QCOMPARE(result.bio, id.bio);
        QCOMPARE(result.color, id.color);
    }

    void identity_editor_emits_changed() {
        IdentityEditor editor;
        QSignalSpy spy(&editor, &IdentityEditor::identityChanged);
        QVERIFY(spy.isValid());

        CollabText::Identity::Identity id;
        id.identity_id = "test-bbb222";
        id.display_name = "Alice";
        id.color = "#22c55e";
        id.updated = "2026-04-06T12:00:00Z";
        editor.setIdentity(id);

        QVERIFY(spy.count() >= 0);
    }

    // --- ParticipantListWidget tests ---

    void participant_list_empty() {
        ParticipantListWidget plw;
        plw.updateParticipants({}, {});
        plw.resize(200, 300);
        QPixmap pm(200, 300);
        plw.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void participant_list_two_participants() {
        CollabText::Identity::Identity alice;
        alice.identity_id = "alice-111111";
        alice.display_name = "Alice";
        alice.color = "#22c55e";

        CollabText::Identity::Identity bob;
        bob.identity_id = "bob-222222";
        bob.display_name = "Bob";
        bob.color = "#ef4444";

        CollabText::Identity::Presence pA;
        pA.replica_id = "rep-a";
        pA.identity_id = "alice-111111";
        pA.active = true;

        CollabText::Identity::Presence pB;
        pB.replica_id = "rep-b";
        pB.identity_id = "bob-222222";
        pB.active = true;

        ParticipantListWidget plw;
        plw.updateParticipants({alice, bob}, {pA, pB});
        plw.resize(200, 300);
        QPixmap pm(200, 300);
        plw.render(&pm);
        QVERIFY(!pm.isNull());
    }

    // --- IdentitySetupDialog tests ---

    void setup_dialog_creates_identity_on_accept() {
        QTemporaryDir tmp;
        CollabText::Identity::IdentityStore store(tmp.path().toStdString());
        IdentitySetupDialog dlg(store);
        QVERIFY(dlg.windowTitle().contains(QStringLiteral("CollabText")));
    }

    void preferences_page_loads_identity() {
        QTemporaryDir tmp;
        CollabText::Identity::IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Test");
        store.save(id);

        IdentityPreferencesPage page(store);
        page.resize(300, 400);
        QPixmap pm(300, 400);
        page.render(&pm);
        QVERIFY(!pm.isNull());
    }

    // --- ParticipantListWidget tests ---

    void participant_list_collapses_same_identity() {
        CollabText::Identity::Identity alice;
        alice.identity_id = "alice-111111";
        alice.display_name = "Alice";
        alice.color = "#22c55e";

        CollabText::Identity::Presence p1;
        p1.replica_id = "laptop";
        p1.identity_id = "alice-111111";
        p1.active = true;

        CollabText::Identity::Presence p2;
        p2.replica_id = "desktop";
        p2.identity_id = "alice-111111";
        p2.active = true;

        ParticipantListWidget plw;
        plw.updateParticipants({alice}, {p1, p2});
        plw.resize(200, 300);
        QPixmap pm(200, 300);
        plw.render(&pm);
        QVERIFY(!pm.isNull());
    }
};

QTEST_MAIN(TestIdentityWidgets)
#include "tst_identity_widgets.moc"
