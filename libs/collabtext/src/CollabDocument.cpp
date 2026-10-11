#include "collabtext/CollabDocument.h"

#include <collabtext/Serialization.h>

#include <QPlainTextDocumentLayout>
#include <QTextCursor>

namespace CollabText {

CollabDocument::CollabDocument(Crdt::SharedIdentityAllocator allocator,
                               QObject *parent)
    : QObject(parent), m_engine(new CrdtEngine(std::move(allocator))),
      m_qtDoc(new QTextDocument(this)) {
  m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
  m_qtDoc->setUndoRedoEnabled(false);

  m_engine->setOnChange([this] { syncEngineToQt(); });
  m_engine->setOnLocalOp([this](const Crdt::Operation &operation) {
    auto encoded = encode_operation(operation);
    if (!encoded.ok()) {
      const auto message = encoded.error().message();
      emit operationError(QString::fromUtf8(message.data(),
                                             static_cast<qsizetype>(message.size())));
      return;
    }
    const auto &bytes = encoded.value();
    emit updateReady(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
  });
  connect(m_qtDoc, &QTextDocument::contentsChange, this,
          [this](int position, int charsRemoved, int charsAdded) {
            onQtContentsChange(position, charsRemoved, charsAdded);
          });
}

CollabDocument::~CollabDocument() { delete m_engine; }

void CollabDocument::insertText(int position, const QString &text) {
  if (position < 0 || position > m_qtDoc->toPlainText().size()) return;
  QTextCursor cursor(m_qtDoc);
  cursor.setPosition(position);
  cursor.insertText(text);
}

void CollabDocument::removeText(int position, int length) {
  const int document_length = m_qtDoc->toPlainText().size();
  if (position < 0 || length < 0 || position > document_length ||
      length > document_length - position)
    return;
  QTextCursor cursor(m_qtDoc);
  cursor.setPosition(position);
  cursor.setPosition(position + length, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
}

Result<ApplyOutcome> CollabDocument::applyUpdate(const QByteArray &update) {
  const std::string_view bytes(update.constData(), static_cast<size_t>(update.size()));
  auto decoded = Crdt::decode_operation(bytes);
  if (!decoded.ok()) return decoded.error();
  return m_engine->apply_remote(decoded.value());
}

bool CollabDocument::undo() { return m_engine->undo(); }

bool CollabDocument::redo() { return m_engine->redo(); }

void CollabDocument::onQtContentsChange(int position, int charsRemoved,
                                       int charsAdded) {
  if (m_suppressQtSignals) return;
  const QString visible = m_qtDoc->toPlainText();
  if (position < 0 || charsRemoved < 0 || charsAdded < 0 ||
      position > visible.size() || charsAdded > visible.size() - position) {
    emit operationError(QStringLiteral("Qt document reported an invalid edit range"));
    syncEngineToQt();
    return;
  }
  const QString inserted = visible.mid(position, charsAdded);
  auto edited = m_engine->try_replace(
      position, charsRemoved, inserted.toUtf8().toStdString());
  if (!edited.ok()) {
    const auto message = edited.error().message();
    emit operationError(QString::fromUtf8(message.data(),
                                           static_cast<qsizetype>(message.size())));
    syncEngineToQt();
  }
}

void CollabDocument::syncEngineToQt() {
  const QString before = m_qtDoc->toPlainText();
  const std::string engine_text = m_engine->text();
  const QString after = QString::fromUtf8(
      engine_text.data(), static_cast<qsizetype>(engine_text.size()));
  if (before == after) return;

  int prefix = 0;
  const int common_length = qMin(before.size(), after.size());
  while (prefix < common_length && before[prefix] == after[prefix]) ++prefix;
  if (prefix > 0 && prefix < before.size() && prefix < after.size() &&
      ((before[prefix - 1].isHighSurrogate() && before[prefix].isLowSurrogate()) ||
       (after[prefix - 1].isHighSurrogate() && after[prefix].isLowSurrogate())))
    --prefix;

  int suffix = 0;
  while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
         before[before.size() - suffix - 1] == after[after.size() - suffix - 1])
    ++suffix;
  const int before_suffix_start = before.size() - suffix;
  const int after_suffix_start = after.size() - suffix;
  const auto splits_pair = [&](const QString &value, int start) {
    return start > 0 && start < value.size() && value[start - 1].isHighSurrogate() &&
           value[start].isLowSurrogate();
  };
  if (suffix > 0 && before_suffix_start > prefix && after_suffix_start > prefix &&
      (splits_pair(before, before_suffix_start) ||
       splits_pair(after, after_suffix_start)))
    --suffix;

  QTextCursor cursor(m_qtDoc);
  m_suppressQtSignals = true;
  cursor.beginEditBlock();
  cursor.setPosition(prefix);
  cursor.setPosition(before.size() - suffix, QTextCursor::KeepAnchor);
  cursor.insertText(after.mid(prefix, after.size() - prefix - suffix));
  cursor.endEditBlock();
  m_suppressQtSignals = false;
}

} // namespace CollabText
