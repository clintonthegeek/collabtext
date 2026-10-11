#include "collabtext/CollabDocument.h"

#include <collabtext/Serialization.h>

#include "crdt/Utf16.h"

#include <QPlainTextDocumentLayout>
#include <QTextCursor>

#include <limits>

namespace CollabText {

CollabDocument::CollabDocument(Crdt::SharedIdentityAllocator allocator,
                               QObject *parent)
    : QObject(parent), m_engine(new CrdtEngine(std::move(allocator))),
      m_qtDoc(new QTextDocument(this)) {
  initializeDocument(m_engine->text());
  m_engine->setOnChange([this] {
    m_backendText = m_engine->text();
    syncBackendToQt();
  });
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
}

CollabDocument::CollabDocument(DocumentSession &session, QObject *parent)
    : QObject(parent), m_session(&session), m_qtDoc(new QTextDocument(this)) {
  initializeDocument(session.text());
}

CollabDocument::~CollabDocument() { delete m_engine; }

void CollabDocument::initializeDocument(const std::string &text) {
  m_backendText = text;
  m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
  m_qtDoc->setUndoRedoEnabled(false);
  m_qtDoc->setPlainText(QString::fromUtf8(
      text.data(), static_cast<qsizetype>(text.size())));
  connect(m_qtDoc, &QTextDocument::contentsChange, this,
          [this](int position, int charsRemoved, int charsAdded) {
            onQtContentsChange(position, charsRemoved, charsAdded);
          });
}

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
  if (!m_engine)
    return Error(ErrorCode::malformed,
                 "raw operation updates are unavailable for DocumentSession adapters");
  const std::string_view bytes = update.isEmpty()
      ? std::string_view{}
      : std::string_view(update.constData(), static_cast<size_t>(update.size()));
  auto decoded = Crdt::decode_operation(bytes);
  if (!decoded.ok()) return decoded.error();
  return m_engine->apply_remote(decoded.value());
}

void CollabDocument::applySessionEdits(
    const std::vector<SessionTextEdit> &edits) {
  if (!m_session || edits.empty()) return;
  const size_t qt_limit = static_cast<size_t>(std::numeric_limits<int>::max());
  for (const auto &edit : edits) {
    const size_t start16 = Crdt::byte_to_utf16_offset(m_backendText, edit.old_start);
    const size_t end16 = Crdt::byte_to_utf16_offset(m_backendText, edit.old_end);
    if (edit.old_start > edit.old_end || edit.old_end > m_backendText.size() ||
        Crdt::utf16_to_byte_offset(m_backendText, start16) != edit.old_start ||
        Crdt::utf16_to_byte_offset(m_backendText, end16) != edit.old_end ||
        end16 > qt_limit) {
      emit operationError(QStringLiteral("session delta has an invalid Qt text range"));
      syncBackendToQt();
      return;
    }
  }
  m_suppressQtSignals = true;
  QTextCursor edit_block(m_qtDoc);
  edit_block.beginEditBlock();
  for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
    const size_t start = Crdt::byte_to_utf16_offset(m_backendText, it->old_start);
    const size_t end = Crdt::byte_to_utf16_offset(m_backendText, it->old_end);
    QTextCursor cursor(m_qtDoc);
    cursor.setPosition(static_cast<int>(start));
    cursor.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
    cursor.insertText(QString::fromUtf8(
        it->new_text.data(), static_cast<qsizetype>(it->new_text.size())));
  }
  edit_block.endEditBlock();
  m_backendText = m_session->text();
  m_suppressQtSignals = false;
}

bool CollabDocument::undo() {
  if (m_engine) return m_engine->undo();
  if (!m_session) return false;
  auto result = m_session->undo();
  if (!result.ok()) return false;
  m_backendText = m_session->text();
  syncBackendToQt();
  return true;
}

bool CollabDocument::redo() {
  if (m_engine) return m_engine->redo();
  if (!m_session) return false;
  auto result = m_session->redo();
  if (!result.ok()) return false;
  m_backendText = m_session->text();
  syncBackendToQt();
  return true;
}

void CollabDocument::onQtContentsChange(int position, int charsRemoved,
                                        int charsAdded) {
  if (m_suppressQtSignals) return;
  const QString visible = m_qtDoc->toPlainText();
  if (position < 0 || charsRemoved < 0 || charsAdded < 0 ||
      position > visible.size() || charsAdded > visible.size() - position) {
    emit operationError(QStringLiteral("Qt document reported an invalid edit range"));
    syncBackendToQt();
    return;
  }
  const QString inserted = visible.mid(position, charsAdded);
  if (m_engine) {
    auto edited = m_engine->try_replace(
        position, charsRemoved, inserted.toUtf8().toStdString());
    if (!edited.ok()) {
      const auto message = edited.error().message();
      emit operationError(QString::fromUtf8(message.data(),
                                             static_cast<qsizetype>(message.size())));
      syncBackendToQt();
      return;
    }
    emit localEditApplied(position, charsRemoved, charsAdded);
    return;
  }

  if (!m_session) return;
  const size_t old_length = Crdt::utf16_length(m_backendText);
  const size_t start16 = static_cast<size_t>(position);
  const size_t end16 = start16 + static_cast<size_t>(charsRemoved);
  if (end16 > old_length ||
      Crdt::byte_to_utf16_offset(m_backendText,
          Crdt::utf16_to_byte_offset(m_backendText, start16)) != start16 ||
      Crdt::byte_to_utf16_offset(m_backendText,
          Crdt::utf16_to_byte_offset(m_backendText, end16)) != end16) {
    emit operationError(QStringLiteral("Qt edit range splits a UTF-16 character"));
    syncBackendToQt();
    return;
  }
  const size_t start_byte = Crdt::utf16_to_byte_offset(m_backendText, start16);
  const size_t end_byte = Crdt::utf16_to_byte_offset(m_backendText, end16);
  if (end_byte > std::numeric_limits<uint32_t>::max()) {
    emit operationError(QStringLiteral("Qt edit offset exceeds session range"));
    syncBackendToQt();
    return;
  }
  auto edited = m_session->apply_local(
      {{static_cast<uint32_t>(start_byte), static_cast<uint32_t>(end_byte)}},
      {inserted.toUtf8().toStdString()});
  if (!edited.ok()) {
    const auto message = edited.error().message();
    emit operationError(QString::fromUtf8(message.data(),
                                           static_cast<qsizetype>(message.size())));
    syncBackendToQt();
    return;
  }
  m_backendText = m_session->text();
  if (m_qtDoc->toPlainText() != QString::fromUtf8(
          m_backendText.data(), static_cast<qsizetype>(m_backendText.size())))
    syncBackendToQt();
  emit localEditApplied(position, charsRemoved, charsAdded);
}

void CollabDocument::syncBackendToQt() {
  const QString before = m_qtDoc->toPlainText();
  const std::string current = m_engine ? m_engine->text()
                                      : (m_session ? m_session->text() : m_backendText);
  m_backendText = current;
  const QString after = QString::fromUtf8(
      current.data(), static_cast<qsizetype>(current.size()));
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
