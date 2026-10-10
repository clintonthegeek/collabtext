#include <QCoreApplication>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>

#include "collabtext/DocumentSession.h"
#include "crdt/SidecarManifest.h"
#include "Document.h"

#include <filesystem>
#include <fstream>

using namespace CollabText;

class TestEditorRecovery : public QObject {
  Q_OBJECT
private slots:
  void manifest_arriving_before_seed_stays_incomplete_then_opens() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdString());
    const QString file_path = QString::fromStdString((root / "note.txt").string());
    const std::string seed = "arriving seed";
    {
      QFile file(file_path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(seed.data(), static_cast<qint64>(seed.size())),
               static_cast<qint64>(seed.size()));
    }
    const auto sidecar = std::filesystem::path(file_path.toStdString() + ".collab");
    std::filesystem::create_directories(sidecar);
    Crdt::SidecarManifest manifest;
    manifest.doc_id = "arrival-order-document";
    manifest.enrolled_at = "2026-10-10T00:00:00Z";
    manifest.original_filename = "note.txt";
    manifest.seed_sha256 = sha256_hex(seed);
    Crdt::write_manifest(sidecar / "manifest.json", manifest);

    Identity::Identity identity;
    identity.identity_id = "arrival-order-user";
    identity.display_name = "Arrival Order";
    identity.color = "#336699";
    CollabEdit::Document document(identity, "arrival-order-device", nullptr,
                                  root / "private" / "replica-state");
    const auto missing_seed = document.open(file_path);
    QVERIFY(missing_seed.contains(QStringLiteral("missing seed.txt")));
    QVERIFY(!document.isCollab());

    // The later seed arrival repairs the sidecar; retrying open must rebuild
    // from that seed and become editable only after the recovery input exists.
    std::ofstream seed_file(sidecar / "seed.txt", std::ios::binary);
    seed_file.write(seed.data(), static_cast<std::streamsize>(seed.size()));
    seed_file.close();
    QVERIFY(seed_file.good());
    QVERIFY(document.open(file_path).isEmpty());
    QVERIFY(document.isCollab());
    auto *edit = document.widget()->findChild<QPlainTextEdit *>();
    QVERIFY(edit);
    QCOMPARE(edit->toPlainText(), QString::fromStdString(seed));
    QVERIFY(document.closeDoc().isEmpty());
  }

  void editor_reopens_with_local_undo_redo_and_remote_edits() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdString());
    const QString file_path = QString::fromStdString((root / "note.txt").string());
    {
      QFile file(file_path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write("seed"), qint64(4));
    }

    Identity::Identity identity;
    identity.identity_id = "editor-test-user";
    identity.display_name = "Editor Test";
    identity.color = "#336699";
    const auto private_root = root / "private" / "replica-state";
    CollabEdit::Document document(identity, "editor-device", nullptr,
                                  private_root);
    QVERIFY(document.open(file_path).isEmpty());
    const auto enable_error = document.enableCollab();
    QVERIFY2(enable_error.isEmpty(), qPrintable(enable_error));
    QVERIFY(document.isCollab());

    auto *edit = document.widget()->findChild<QPlainTextEdit *>();
    QVERIFY(edit);
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed"));

    const auto sidecar = std::filesystem::path(file_path.toStdString() + ".collab");
    auto manifest = Crdt::read_manifest(sidecar / "manifest.json");
    QVERIFY(manifest.has_value());
    DocumentSessionConfig peer_config;
    peer_config.shared_store = sidecar;
    peer_config.private_state = root / "peer-private";
    peer_config.document = manifest->doc_id;
    peer_config.stream = "text";
    peer_config.initial_text = "seed";
    auto peer_opened = DocumentSession::open(peer_config);
    QVERIFY(peer_opened.ok());
    auto peer = std::move(peer_opened).value();
    QVERIFY(peer->ready());
    QVERIFY(peer->apply_local({{4, 4}}, {"!"}).ok());
    QVERIFY(peer->close().ok());
    QTRY_COMPARE_WITH_TIMEOUT(edit->toPlainText(), QStringLiteral("seed!"), 5000);

    QTextCursor cursor = edit->textCursor();
    cursor.movePosition(QTextCursor::End);
    edit->setTextCursor(cursor);
    QTest::keyClicks(edit, QStringLiteral(" one"));
    QTest::qWait(600);
    QTest::keyClicks(edit, QStringLiteral(" two"));
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed! one two"));
    QVERIFY(document.isModified());

    QTest::keyClick(edit, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed! one "));
    QVERIFY(document.save().isEmpty());
    QVERIFY(!document.isModified());

    QVERIFY(document.closeDoc().isEmpty());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(document.open(file_path).isEmpty());
    edit = document.widget()->findChild<QPlainTextEdit *>();
    QVERIFY(edit);
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed! one "));

    QTest::keyClick(edit, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed! one"));
    QTest::keyClick(edit, Qt::Key_Z,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed! one "));
    QTest::keyClick(edit, Qt::Key_Z,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(edit->toPlainText(), QStringLiteral("seed! one two"));

    // Force the materialized-view replacement to fail after the session has
    // durably committed another edit. QSaveFile must preserve the old file,
    // and the editor must keep its modified state until a later save succeeds.
    QTest::keyClicks(edit, QStringLiteral("?"));
    QVERIFY(document.isModified());
    const auto backup = root / "old-materialized.txt";
    std::error_code rename_error;
    std::filesystem::rename(file_path.toStdString(), backup, rename_error);
    QVERIFY(!rename_error);
    QVERIFY(std::filesystem::create_directory(file_path.toStdString()));
    const auto sentinel = std::filesystem::path(file_path.toStdString()) / "sentinel";
    std::ofstream(sentinel) << "preserve";
    QVERIFY(!document.save().isEmpty());
    QVERIFY(document.isModified());
    QVERIFY(std::filesystem::exists(sentinel));
    std::ifstream old_file(backup);
    std::string old_content((std::istreambuf_iterator<char>(old_file)), {});
    QCOMPARE(old_content, std::string("seed! one "));

    std::filesystem::remove_all(file_path.toStdString());
    std::filesystem::rename(backup, file_path.toStdString());
    QVERIFY(document.save().isEmpty());
    QVERIFY(!document.isModified());
    QVERIFY(document.closeDoc().isEmpty());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(document.open(file_path).isEmpty());
    edit = document.widget()->findChild<QPlainTextEdit *>();
    QVERIFY(edit);
    cursor = edit->textCursor();
    cursor.movePosition(QTextCursor::End);
    edit->setTextCursor(cursor);
    QTest::keyClicks(edit, QStringLiteral(" shutdown"));
    QVERIFY(document.isModified());
    const QString recovered_text = edit->toPlainText();
    QVERIFY(document.closeDoc().isEmpty()); // do not save the materialized view
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(document.open(file_path).isEmpty());
    edit = document.widget()->findChild<QPlainTextEdit *>();
    QVERIFY(edit);
    QCOMPARE(edit->toPlainText(), recovered_text);
    QVERIFY(document.isModified());
    QVERIFY(document.save().isEmpty());
    QVERIFY(!document.isModified());
    QVERIFY(document.closeDoc().isEmpty());
  }
};

QTEST_MAIN(TestEditorRecovery)
#include "tst_editor_recovery.moc"
