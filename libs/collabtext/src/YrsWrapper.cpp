#include "collabtext/YrsWrapper.h"

#include <QDebug>

namespace CollabText {

// ---- ReadTransaction ------------------------------------------------------

ReadTransaction::ReadTransaction(YDoc *doc)
    : m_txn(ydoc_read_transaction(doc))
{
}

ReadTransaction::~ReadTransaction()
{
    if (m_txn)
        ytransaction_commit(m_txn);
}

// ---- WriteTransaction -----------------------------------------------------

WriteTransaction::WriteTransaction(YDoc *doc, const QByteArray &origin)
    : m_txn(ydoc_write_transaction(
          doc,
          origin.isEmpty() ? 0 : static_cast<uint32_t>(origin.size()),
          origin.isEmpty() ? nullptr : origin.constData()))
{
}

WriteTransaction::~WriteTransaction()
{
    if (m_txn && !m_committed)
        ytransaction_commit(m_txn);
}

void WriteTransaction::commit()
{
    if (m_txn && !m_committed) {
        ytransaction_commit(m_txn);
        m_committed = true;
    }
}

// ---- YrsDocument ----------------------------------------------------------

YrsDocument::YrsDocument(QObject *parent)
    : QObject(parent)
{
    // Create doc with UTF-16 offset encoding to match Qt's QString/QTextCursor
    YOptions opts = yoptions();
    opts.encoding = Y_OFFSET_UTF16;
    m_doc = ydoc_new_with_options(opts);

    // Get (or create) the root "text" shared type
    m_text = ytext(m_doc, "text");

    // Set up undo manager scoped to our text type
    YUndoManagerOptions undoOpts;
    undoOpts.capture_timeout_millis = 300; // group keystrokes within 300ms
    m_undo = yundo_manager(m_doc, &undoOpts);
    yundo_manager_add_scope(m_undo, m_text);

    setupObservers();
}

YrsDocument::~YrsDocument()
{
    if (m_textSub)
        yunobserve(m_textSub);
    if (m_updateSub)
        yunobserve(m_updateSub);
    if (m_undo)
        yundo_manager_destroy(m_undo);
    if (m_doc)
        ydoc_destroy(m_doc);
}

// ---- Text operations ------------------------------------------------------

void YrsDocument::insert(uint32_t index, const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    WriteTransaction txn(m_doc);
    ytext_insert(m_text, txn.raw(), index, utf8.constData(), nullptr);
}

void YrsDocument::remove(uint32_t index, uint32_t length)
{
    WriteTransaction txn(m_doc);
    ytext_remove_range(m_text, txn.raw(), index, length);
}

QString YrsDocument::text() const
{
    ReadTransaction txn(m_doc);
    if (!txn.raw())
        return {};
    char *raw = ytext_string(m_text, txn.raw());
    QString result = QString::fromUtf8(raw);
    ystring_destroy(raw);
    return result;
}

uint32_t YrsDocument::length() const
{
    ReadTransaction txn(m_doc);
    if (!txn.raw())
        return 0;
    return ytext_len(m_text, txn.raw());
}

// ---- Undo / Redo ----------------------------------------------------------

bool YrsDocument::undo()
{
    return yundo_manager_undo(m_undo) != 0;
}

bool YrsDocument::redo()
{
    return yundo_manager_redo(m_undo) != 0;
}

bool YrsDocument::canUndo() const
{
    return yundo_manager_undo_stack_len(m_undo) > 0;
}

bool YrsDocument::canRedo() const
{
    return yundo_manager_redo_stack_len(m_undo) > 0;
}

// ---- Cursor tracking ------------------------------------------------------

QByteArray YrsDocument::createStickyIndex(uint32_t index, int8_t assoc)
{
    ReadTransaction txn(m_doc);
    if (!txn.raw())
        return {}; // transaction unavailable (doc busy), skip this cycle

    YStickyIndex *si = ysticky_index_from_index(m_text, txn.raw(), index, assoc);
    if (!si)
        return {};

    uint32_t len = 0;
    char *encoded = ysticky_index_encode(si, &len);
    QByteArray result(encoded, static_cast<int>(len));
    ybinary_destroy(encoded, len);
    ysticky_index_destroy(si);
    return result;
}

int32_t YrsDocument::resolveStickyIndex(const QByteArray &encoded) const
{
    YStickyIndex *si = ysticky_index_decode(encoded.constData(),
                                            static_cast<uint32_t>(encoded.size()));
    if (!si)
        return -1;

    ReadTransaction txn(m_doc);
    if (!txn.raw()) {
        ysticky_index_destroy(si);
        return -1; // transaction unavailable, skip
    }

    Branch *branch = nullptr;
    uint32_t index = 0;
    ysticky_index_read(si, txn.raw(), &branch, &index);
    ysticky_index_destroy(si);
    return static_cast<int32_t>(index);
}

// ---- Sync protocol --------------------------------------------------------

QByteArray YrsDocument::stateVector() const
{
    ReadTransaction txn(m_doc);
    if (!txn.raw())
        return {};
    uint32_t len = 0;
    char *sv = ytransaction_state_vector_v1(txn.raw(), &len);
    QByteArray result(sv, static_cast<int>(len));
    ybinary_destroy(sv, len);
    return result;
}

QByteArray YrsDocument::stateDiff(const QByteArray &remoteStateVector) const
{
    ReadTransaction txn(m_doc);
    if (!txn.raw())
        return {};
    uint32_t len = 0;
    char *diff = ytransaction_state_diff_v1(
        txn.raw(),
        remoteStateVector.constData(),
        static_cast<uint32_t>(remoteStateVector.size()),
        &len);
    QByteArray result(diff, static_cast<int>(len));
    ybinary_destroy(diff, len);
    return result;
}

bool YrsDocument::applyUpdate(const QByteArray &update)
{
    m_applyingRemote = true;
    WriteTransaction txn(m_doc);
    uint8_t result = ytransaction_apply(
        txn.raw(),
        update.constData(),
        static_cast<uint32_t>(update.size()));
    txn.commit(); // commit while m_applyingRemote is still true
    m_applyingRemote = false;
    return result == 0;
}

// ---- Observers ------------------------------------------------------------

// Static callback trampolines (C function pointers → Qt signals)

static void textObserverCallback(void *state, const YTextEvent *event)
{
    auto *self = static_cast<YrsDocument *>(state);
    if (!self->isApplyingRemote())
        return; // local edits: QTextDocument already has the content

    uint32_t deltaLen = 0;
    YDeltaOut *rawDelta = ytext_event_delta(event, &deltaLen);

    QVector<TextDelta> deltas;
    deltas.reserve(static_cast<int>(deltaLen));

    for (uint32_t i = 0; i < deltaLen; ++i) {
        TextDelta d;
        switch (rawDelta[i].tag) {
        case Y_EVENT_CHANGE_ADD:
            d.tag = TextDelta::Insert;
            if (rawDelta[i].insert) {
                char *str = youtput_read_string(rawDelta[i].insert);
                if (str) {
                    d.text = QString::fromUtf8(str);
                    d.len = static_cast<uint32_t>(d.text.size());
                }
            }
            break;
        case Y_EVENT_CHANGE_DELETE:
            d.tag = TextDelta::Delete;
            d.len = rawDelta[i].len;
            break;
        case Y_EVENT_CHANGE_RETAIN:
            d.tag = TextDelta::Retain;
            d.len = rawDelta[i].len;
            break;
        default:
            continue;
        }
        deltas.append(d);
    }

    ytext_delta_destroy(rawDelta, deltaLen);
    emit self->remoteTextChanged(deltas);
}

static void updateObserverCallback(void *state, uint32_t len, const char *data)
{
    auto *self = static_cast<YrsDocument *>(state);
    if (self->isApplyingRemote())
        return; // don't echo remote updates back out
    QByteArray update(data, static_cast<int>(len));
    emit self->updateProduced(update);
}

void YrsDocument::setupObservers()
{
    m_textSub = ytext_observe(m_text, this, textObserverCallback);
    m_updateSub = ydoc_observe_updates_v1(m_doc, this, updateObserverCallback);
}

} // namespace CollabText
