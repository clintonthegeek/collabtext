#include <collabtext/CollabDocument.h>
#include <collabtext/Serialization.h>

#include "TestActor.h"
#include "ui/CollabPlainTextEdit.h"

#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>

#include <filesystem>

using namespace CollabText;

class DocumentAdapterTest final : public QObject {
  Q_OBJECT
private slots:
  void directDocumentEditsEmitOneLocalOperationAndStayUnicodeAligned() {
    CollabDocument document(Test::test_counter(121));
    QSignalSpy updates(&document, &CollabDocument::updateReady);
    QVERIFY(updates.isValid());

    QTextCursor cursor(document.qtDocument());
    cursor.insertText(QString::fromUtf8("a😀z"));
    QCOMPARE(document.engine()->text(),
             document.qtDocument()->toPlainText().toUtf8().toStdString());
    QCOMPARE(updates.size(), 1);

    cursor.setPosition(1);
    cursor.setPosition(3, QTextCursor::KeepAnchor);
    cursor.insertText(QString::fromUtf8("雪"));
    QCOMPARE(document.engine()->text(),
             document.qtDocument()->toPlainText().toUtf8().toStdString());
    QCOMPARE(QString::fromUtf8(document.engine()->text().c_str()),
             QString::fromUtf8("a雪z"));
    QCOMPARE(updates.size(), 2);
  }

  void remoteOperationsUpdateQtWithoutBeingReemitted() {
    CollabDocument document(Test::test_counter(122));
    QSignalSpy updates(&document, &CollabDocument::updateReady);
    CrdtEngine author(Test::test_counter(123));
    std::string encoded;
    author.setOnLocalOp([&](const Crdt::Operation &operation) {
      auto bytes = encode_operation(operation);
      QVERIFY(bytes.ok());
      encoded = bytes.value();
    });
    author.insert(0, "remote 🌊");
    QVERIFY(!encoded.empty());

    const QByteArray update(encoded.data(), static_cast<qsizetype>(encoded.size()));
    auto applied = document.applyUpdate(update);
    QVERIFY(applied);
    QCOMPARE(applied.value().kind(), ApplyOutcomeKind::applied);
    QCOMPARE(document.engine()->text(), std::string("remote 🌊"));
    QCOMPARE(document.qtDocument()->toPlainText(), QString::fromUtf8("remote 🌊"));
    QCOMPARE(updates.size(), 0);

    auto duplicate = document.applyUpdate(update);
    QVERIFY(duplicate);
    QCOMPARE(duplicate.value().kind(), ApplyOutcomeKind::duplicate);
    QCOMPARE(updates.size(), 0);
    QVERIFY(!document.applyUpdate(QByteArray("bad", 3)));
  }

  void remoteMinimalPatchPreservesDocumentCursorPosition() {
    CollabDocument document(Test::test_counter(127));
    QByteArray initial;
    connect(&document, &CollabDocument::updateReady, this,
            [&](const QByteArray &update) { initial = update; });
    document.insertText(0, QStringLiteral("abcd"));
    QTextCursor stable(document.qtDocument());
    stable.setPosition(3);

    CrdtEngine author(Test::test_counter(128));
    auto initial_operation = Crdt::decode_operation(std::string_view(
        initial.constData(), static_cast<size_t>(initial.size())));
    QVERIFY(initial_operation.ok());
    QCOMPARE(author.apply_remote(initial_operation.value()).kind(),
             ApplyOutcomeKind::applied);
    QCOMPARE(author.text(), std::string("abcd"));
    std::string encoded;
    author.setOnLocalOp([&](const Crdt::Operation &operation) {
      auto bytes = encode_operation(operation);
      QVERIFY(bytes.ok());
      encoded = bytes.value();
    });
    author.insert(0, "X");
    auto applied = document.applyUpdate(
        QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size())));
    QVERIFY(applied);
    QCOMPARE(document.qtDocument()->toPlainText(), QStringLiteral("Xabcd"));
    QCOMPARE(stable.position(), 4);
  }

  void keyboardNonBmpAndMultiCursorMutationsReachTheEngine() {
    CollabDocument document(Test::test_counter(124));
    Ui::CollabPlainTextEdit editor;
    editor.setDocument(document.qtDocument());
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    editor.setFocus();
    QCoreApplication::processEvents();
    QTest::keyClicks(&editor, QStringLiteral("IME"));
    QTextCursor nonBmp(document.qtDocument());
    nonBmp.setPosition(document.qtDocument()->toPlainText().size());
    nonBmp.insertText(QString::fromUtf8("🧭"));
    QCOMPARE(document.engine()->text(),
             document.qtDocument()->toPlainText().toUtf8().toStdString());
    QCOMPARE(document.qtDocument()->toPlainText(), QString::fromUtf8("IME🧭"));

    document.insertText(document.engine()->length(), QStringLiteral(" end"));
    auto *controller = editor.multiCursorController();
    controller->setPrimaryCursor(QTextCursor(document.qtDocument()));
    QTextCursor primary(document.qtDocument());
    primary.setPosition(1);
    controller->setPrimaryCursor(primary);
    controller->addCursorAt(document.qtDocument()->toPlainText().size() - 1);
    controller->insertText(QStringLiteral("X"));
    QCOMPARE(document.engine()->text(),
             document.qtDocument()->toPlainText().toUtf8().toStdString());
  }

  void undoRedoAndProgrammaticAdapterCallsUseTheSameEnginePath() {
    CollabDocument document(Test::test_counter(125));
    QSignalSpy updates(&document, &CollabDocument::updateReady);
    document.insertText(0, QStringLiteral("seed"));
    QCOMPARE(document.engine()->text(), std::string("seed"));
    QCOMPARE(updates.size(), 1);
    QVERIFY(document.undo());
    QCOMPARE(document.engine()->text(), std::string{});
    QCOMPARE(document.qtDocument()->toPlainText(), QString{});
    QCOMPARE(updates.size(), 2);
    QVERIFY(document.redo());
    QCOMPARE(document.engine()->text(), std::string("seed"));
    QCOMPARE(document.qtDocument()->toPlainText(), QStringLiteral("seed"));
    QCOMPARE(updates.size(), 3);

    document.removeText(1, 2);
    QCOMPARE(document.engine()->text(), std::string("sd"));
    QCOMPARE(document.qtDocument()->toPlainText(), QStringLiteral("sd"));
    QCOMPARE(updates.size(), 4);
  }

  void engineRejectsOutOfRangeAndSurrogateSplittingReplacements() {
    CrdtEngine engine(Test::test_counter(129));
    engine.insert(0, "a😀b");
    const std::string before = engine.text();

    QVERIFY(!engine.try_replace(-1, 0, "x"));
    QVERIFY(!engine.try_replace(0, 5, "x"));
    QVERIFY(!engine.try_replace(2, 0, "x"));
    QVERIFY(!engine.try_replace(1, 1, "x"));
    QVERIFY(!engine.try_replace(2, 1, "x"));
    QCOMPARE(engine.text(), before);

    auto valid = engine.try_replace(1, 2, "🌊");
    QVERIFY(valid);
    QCOMPARE(engine.text(), std::string("a🌊b"));
  }

  void durableSessionBackendTracksQtAndAppliesSessionDeltasWithoutFeedback() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path root(temporary.path().toStdString());
    DocumentSessionConfig config{root / "shared", root / "private",
                                 "adapter-session", "text", 1};
    config.initial_text = "a😀z";
    auto opened = DocumentSession::open(config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    QVERIFY(session->ready());

    CollabDocument adapter(*session);
    QSignalSpy local_edits(&adapter, &CollabDocument::localEditApplied);
    QVERIFY(local_edits.isValid());
    QCOMPARE(adapter.qtDocument()->toPlainText(), QString::fromUtf8("a😀z"));

    QTextCursor cursor(adapter.qtDocument());
    cursor.setPosition(1);
    cursor.setPosition(3, QTextCursor::KeepAnchor);
    cursor.insertText(QString::fromUtf8("🌊"));
    QCOMPARE(session->text(), std::string("a🌊z"));
    QCOMPARE(adapter.qtDocument()->toPlainText(), QString::fromUtf8("a🌊z"));
    QCOMPARE(local_edits.size(), 1);

    const auto before_remote = session->recovery_frontier();
    auto remote = session->apply_local({{0, 0}}, {"R"});
    QVERIFY(remote.ok());
    auto deltas = session->edits_since(before_remote);
    QVERIFY(deltas.ok());
    adapter.applySessionEdits(deltas.value());
    QCOMPARE(session->text(), std::string("Ra🌊z"));
    QCOMPARE(adapter.qtDocument()->toPlainText(), QString::fromUtf8("Ra🌊z"));
    QCOMPARE(local_edits.size(), 1);

    QVERIFY(adapter.undo());
    QCOMPARE(adapter.qtDocument()->toPlainText(), QString::fromUtf8("a🌊z"));
    QVERIFY(adapter.redo());
    QCOMPARE(adapter.qtDocument()->toPlainText(), QString::fromUtf8("Ra🌊z"));
  }
};

QTEST_MAIN(DocumentAdapterTest)
#include "tst_document_adapter.moc"
