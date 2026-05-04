#include <QTest>
#include <QTemporaryDir>
#include "crdt/SidecarManifest.h"

#include <filesystem>
#include <fstream>

using namespace CollabText::Crdt;
namespace fs = std::filesystem;

class TestSidecarManifest : public QObject {
    Q_OBJECT
private slots:
    void round_trip() {
        SidecarManifest m;
        m.schema_version    = 3;
        m.doc_id            = "01HXXX0000000000000000000A";
        m.enrolled_at       = "2026-04-30T14:22:01Z";
        m.original_filename = "notes.md";
        m.seed_sha256       = "deadbeef";

        std::string json = manifest_to_json(m);
        auto parsed = manifest_from_json(json);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->schema_version,    m.schema_version);
        QCOMPARE(parsed->doc_id,            m.doc_id);
        QCOMPARE(parsed->enrolled_at,       m.enrolled_at);
        QCOMPARE(parsed->original_filename, m.original_filename);
        QCOMPARE(parsed->seed_sha256,       m.seed_sha256);
    }

    void rejects_unknown_schema_version() {
        std::string json = R"({"schema_version":99,"doc_id":"x","enrolled_at":"t","original_filename":"f","seed_sha256":"h"})";
        QVERIFY(!manifest_from_json(json).has_value());
    }

    void rejects_malformed_json() {
        QVERIFY(!manifest_from_json("").has_value());
        QVERIFY(!manifest_from_json("{").has_value());
        QVERIFY(!manifest_from_json("not json").has_value());
    }

    void sha256_helper_matches_known_value() {
        QCOMPARE(sha256_hex("abc"), std::string(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }

    void sha256_helper_empty_input() {
        QCOMPARE(sha256_hex(""), std::string(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }

    void atomic_write_and_read_round_trip() {
        QTemporaryDir tmp;
        fs::path dir = tmp.path().toStdString();

        SidecarManifest m;
        m.schema_version = 3;
        m.doc_id         = "01HABC";
        m.enrolled_at    = "2026-04-30T14:22:01Z";
        m.original_filename = "notes.md";
        m.seed_sha256    = sha256_hex("hello");

        auto path = dir / "manifest.json";
        write_manifest(path, m);
        QVERIFY(fs::exists(path));

        auto loaded = read_manifest(path);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->doc_id, m.doc_id);
        QCOMPARE(loaded->schema_version, 3);
    }

    void rejects_schema_version_1() {
        QTemporaryDir tmp;
        fs::path p = fs::path(tmp.path().toStdString()) / "manifest.json";
        std::ofstream(p) << "{\"schema_version\":1,\"doc_id\":\"x\","
                            "\"enrolled_at\":\"2026-04-30T00:00:00Z\","
                            "\"original_filename\":\"f.txt\","
                            "\"seed_sha256\":\"00\"}";
        auto loaded = read_manifest(p);
        QVERIFY(!loaded.has_value());
    }

    void doc_id_compare_is_lexicographic() {
        QVERIFY(doc_id_less("01HABC", "01HABD"));
        QVERIFY(!doc_id_less("01HABD", "01HABC"));
        QVERIFY(!doc_id_less("01HABC", "01HABC"));
    }
};

QTEST_GUILESS_MAIN(TestSidecarManifest)
#include "tst_sidecar_manifest.moc"
