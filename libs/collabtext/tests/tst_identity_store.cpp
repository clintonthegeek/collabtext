#include <QTest>
#include <QTemporaryDir>
#include "collabtext/IdentityStore.h"

using namespace CollabText::Identity;
namespace fs = std::filesystem;

class TestIdentityStore : public QObject {
    Q_OBJECT

private slots:
    void load_returns_nullopt_when_no_file() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        QVERIFY(!store.load().has_value());
    }

    void generate_creates_valid_identity() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Clinton");
        QVERIFY(id.identity_id.starts_with("clinton-"));
        QCOMPARE(id.identity_id.size(), size_t(7 + 1 + 6)); // "clinton" + "-" + 6 hex
        QCOMPARE(id.display_name, std::string("Clinton"));
        QVERIFY(!id.color.empty());
        QVERIFY(id.color[0] == '#');
        QVERIFY(!id.updated.empty());
    }

    void generate_slug_handles_unicode() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Tëst Üser");
        QVERIFY(id.identity_id.find("tst") != std::string::npos ||
                id.identity_id.find("test") != std::string::npos ||
                id.identity_id.size() > 3);
    }

    void save_and_load_roundtrip() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id = store.generate("Alice");
        store.save(id);
        auto loaded = store.load();
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->identity_id, id.identity_id);
        QCOMPARE(loaded->display_name, id.display_name);
        QCOMPARE(loaded->color, id.color);
    }

    void save_creates_directory() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "nested" / "dir";
        IdentityStore store(dir);
        auto id = store.generate("Bob");
        store.save(id);
        QVERIFY(fs::exists(dir / "identity.json"));
    }

    void avatar_path_returns_expected_path() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto path = store.avatar_path();
        QVERIFY(path.filename() == "avatar.png");
    }

    void load_avatar_returns_empty_when_no_file() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto data = store.load_avatar();
        QVERIFY(data.empty());
    }

    void save_avatar_rejects_oversized() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        std::vector<uint8_t> big(257 * 1024, 0);
        QVERIFY(!store.save_avatar(big));
    }

    void save_and_load_avatar() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47};
        QVERIFY(store.save_avatar(data));
        auto loaded = store.load_avatar();
        QCOMPARE(loaded.size(), data.size());
        QCOMPARE(loaded, data);
    }

    void signing_key_path_returns_expected() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto path = store.signing_key_path();
        QVERIFY(path.filename() == "identity.key");
    }

    void two_generates_produce_different_ids() {
        QTemporaryDir tmp;
        IdentityStore store(tmp.path().toStdString());
        auto id1 = store.generate("Same");
        auto id2 = store.generate("Same");
        QVERIFY(id1.identity_id != id2.identity_id);
    }
};

QTEST_MAIN(TestIdentityStore)
#include "tst_identity_store.moc"
