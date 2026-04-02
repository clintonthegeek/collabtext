# File-Based Sync and Remote Cursors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace in-process signal wiring with file-based CRDT sync and add remote cursor + selection rendering, so both panes communicate only through a shared folder on disk.

**Architecture:** Each PeerPane owns its own CollabDocument + SyncManager with a unique replica ID. Both SyncManagers point at the same shared folder. CRDT updates flow as binary blobs through numbered files. Cursor positions flow as ephemeral JSON with serialized YStickyIndex blobs. Remote cursors are rendered via QPlainTextEdit's extraSelections mechanism with painted name labels.

**Tech Stack:** Qt6 Widgets, yrs/yffi (via libyrs.a), QJsonDocument, QSaveFile, QPlainTextEdit::ExtraSelection

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `libs/collabtext/include/collabtext/SyncManager.h` | Add ephemeral state API and signal |
| Modify | `libs/collabtext/src/SyncManager.cpp` | Implement ephemeral write/read, timer interval change |
| Modify | `libs/collabtext/include/collabtext/CollabDocument.h` | Add stickyIndexAt / resolveSticky convenience methods |
| Modify | `libs/collabtext/src/CollabDocument.cpp` | Implement convenience methods |
| Rewrite | `app/main.cpp` | New PeerPane with SyncManager, remote cursor rendering, cursor label painting |

No new files. No CMake changes (no new source files, no new dependencies — QJsonDocument is in Qt6::Core which is already linked).

---

### Task 1: Add ephemeral state to SyncManager

**Files:**
- Modify: `libs/collabtext/include/collabtext/SyncManager.h`
- Modify: `libs/collabtext/src/SyncManager.cpp`

- [ ] **Step 1: Add ephemeral API to SyncManager header**

Add to the public section of SyncManager, after `stop()`:

```cpp
void setEphemeralState(const QJsonObject &state);
```

Add to the signals section:

```cpp
void remoteEphemeralChanged(const QString &replicaId, const QJsonObject &state);
```

Add to the private section, after `m_pendingUpdates`:

```cpp
QJsonObject m_ephemeralState;
```

Add `#include <QJsonObject>` to the includes.

- [ ] **Step 2: Implement setEphemeralState**

In `SyncManager.cpp`, add:

```cpp
void SyncManager::setEphemeralState(const QJsonObject &state)
{
    m_ephemeralState = state;
}
```

- [ ] **Step 3: Implement writeEphemeral**

Replace the stub `writePresence()` method. Rename it to `writeEphemeral()` in both header and source.

In the header, rename `void writePresence();` to `void writeEphemeral();`.

In the source, replace the entire `writePresence()` body:

```cpp
void SyncManager::writeEphemeral()
{
    QJsonObject obj = m_ephemeralState;
    obj[QStringLiteral("last_heartbeat")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QString path = QStringLiteral("%1/replicas/%2/ephemeral.json")
                       .arg(m_sharedFolder, m_replicaId);
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.commit();
    }
}
```

Add these includes to `SyncManager.cpp`:

```cpp
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
```

Update the `syncCycle()` call from `writePresence()` to `writeEphemeral()`.

- [ ] **Step 4: Implement readRemoteEphemerals**

Add a new private method declaration in the header:

```cpp
void readRemoteEphemerals();
```

Implement in source:

```cpp
void SyncManager::readRemoteEphemerals()
{
    QDir replicasDir(m_sharedFolder + QStringLiteral("/replicas"));
    if (!replicasDir.exists())
        return;

    for (const auto &entry : replicasDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry == m_replicaId)
            continue;

        QString path = replicasDir.filePath(entry) + QStringLiteral("/ephemeral.json");
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        QJsonObject obj = doc.object();

        // Staleness check: ignore if heartbeat > 5 seconds old
        QString heartbeat = obj.value(QStringLiteral("last_heartbeat")).toString();
        if (!heartbeat.isEmpty()) {
            QDateTime ts = QDateTime::fromString(heartbeat, Qt::ISODateWithMs);
            if (ts.isValid() && ts.msecsTo(QDateTime::currentDateTimeUtc()) > 5000)
                continue;
        }

        emit remoteEphemeralChanged(entry, obj);
    }
}
```

- [ ] **Step 5: Wire readRemoteEphemerals into syncCycle**

Update `syncCycle()` to:

```cpp
void SyncManager::syncCycle()
{
    flushLocalUpdates();
    readRemoteUpdates();
    writeEphemeral();
    readRemoteEphemerals();
}
```

- [ ] **Step 6: Change sync interval to 500ms**

In `SyncManager::start()`, change:

```cpp
m_timer.start(1000);
```

to:

```cpp
m_timer.start(500);
```

- [ ] **Step 7: Build and verify**

Run: `cd /home/clinton/dev/collabtext/build-dev && make -j$(nproc)`

Expected: Clean build, no errors.

- [ ] **Step 8: Commit**

```bash
git add libs/collabtext/include/collabtext/SyncManager.h libs/collabtext/src/SyncManager.cpp
git commit -m "feat: add ephemeral state read/write to SyncManager"
```

---

### Task 2: Add sticky index convenience methods to CollabDocument

**Files:**
- Modify: `libs/collabtext/include/collabtext/CollabDocument.h`
- Modify: `libs/collabtext/src/CollabDocument.cpp`

- [ ] **Step 1: Add declarations to CollabDocument.h**

Add after `bool redo();`:

```cpp
// Cursor tracking via CRDT-anchored positions
QByteArray stickyIndexAt(int position, int8_t assoc = 0);
int resolveSticky(const QByteArray &encoded) const;
```

- [ ] **Step 2: Implement in CollabDocument.cpp**

Add after the `redo()` method:

```cpp
QByteArray CollabDocument::stickyIndexAt(int position, int8_t assoc)
{
    return m_crdt->createStickyIndex(static_cast<uint32_t>(position), assoc);
}

int CollabDocument::resolveSticky(const QByteArray &encoded) const
{
    return m_crdt->resolveStickyIndex(encoded);
}
```

- [ ] **Step 3: Build and verify**

Run: `cd /home/clinton/dev/collabtext/build-dev && make -j$(nproc)`

Expected: Clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/include/collabtext/CollabDocument.h libs/collabtext/src/CollabDocument.cpp
git commit -m "feat: add sticky index convenience methods to CollabDocument"
```

---

### Task 3: Rewrite test harness with file-based sync

**Files:**
- Rewrite: `app/main.cpp`

- [ ] **Step 1: Write the new PeerPane class**

Replace the entire `app/main.cpp` with the new harness. PeerPane now owns a SyncManager and uses file-based sync instead of direct signal wiring.

```cpp
#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QVBoxLayout>

#include <collabtext/CollabDocument.h>
#include <collabtext/SyncManager.h>

static const QString SHARED_FOLDER =
    QDir::tempPath() + QStringLiteral("/collabtext-test");

class PeerPane : public QWidget {
    Q_OBJECT
public:
    PeerPane(const QString &replicaId, const QString &displayName,
             const QColor &color, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_displayName(displayName)
        , m_color(color)
        , m_doc(new CollabText::CollabDocument(this))
        , m_sync(new CollabText::SyncManager(m_doc->crdt(), this))
        , m_edit(new QPlainTextEdit(this))
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(displayName, this));
        layout->addWidget(m_edit);

        m_edit->setDocument(m_doc->qtDocument());
        m_edit->setPlaceholderText(
            QStringLiteral("Type here (%1)...").arg(displayName));

        // Intercept local edits → mirror into CRDT
        connect(m_edit->document(), &QTextDocument::contentsChange,
                this, &PeerPane::onContentsChange);

        // Capture cursor position changes → update ephemeral state
        connect(m_edit, &QPlainTextEdit::cursorPositionChanged,
                this, &PeerPane::onCursorPositionChanged);

        // Receive remote cursor data from SyncManager
        connect(m_sync,
                &CollabText::SyncManager::remoteEphemeralChanged,
                this, &PeerPane::onRemoteEphemeral);

        // Start file-based sync
        m_sync->start(SHARED_FOLDER, replicaId);
    }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded)
    {
        if (m_applying)
            return;
        m_applying = true;

        // Detect format-only changes (e.g. setBlockFormat after Enter)
        if (charsRemoved > 0 && charsAdded > 0) {
            QTextCursor cursor(m_edit->document());
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString newContent = cursor.selectedText();
            newContent.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

            QString crdtText = m_doc->crdt()->text();
            QString oldContent = crdtText.mid(position, charsRemoved);
            if (newContent == oldContent) {
                m_applying = false;
                return;
            }
        }

        if (charsRemoved > 0)
            m_doc->crdt()->remove(static_cast<uint32_t>(position),
                                  static_cast<uint32_t>(charsRemoved));
        if (charsAdded > 0) {
            QTextCursor cursor(m_edit->document());
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString inserted = cursor.selectedText();
            inserted.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            m_doc->crdt()->insert(static_cast<uint32_t>(position), inserted);
        }

        m_applying = false;
    }

    void onCursorPositionChanged()
    {
        QTextCursor tc = m_edit->textCursor();
        int head = tc.position();
        int anchor = tc.anchor();

        QByteArray headSticky = m_doc->stickyIndexAt(head, 0);
        QByteArray anchorSticky = m_doc->stickyIndexAt(anchor, 0);

        QJsonObject state;
        state[QStringLiteral("name")] = m_displayName;
        state[QStringLiteral("color")] = m_color.name();
        state[QStringLiteral("cursor_head")] =
            QString::fromLatin1(headSticky.toBase64());
        state[QStringLiteral("cursor_anchor")] =
            QString::fromLatin1(anchorSticky.toBase64());

        m_sync->setEphemeralState(state);
    }

    void onRemoteEphemeral(const QString &replicaId, const QJsonObject &state)
    {
        Q_UNUSED(replicaId);

        QString headB64 = state.value(QStringLiteral("cursor_head")).toString();
        QString anchorB64 =
            state.value(QStringLiteral("cursor_anchor")).toString();
        QColor color(
            state.value(QStringLiteral("color")).toString(QStringLiteral("#888")));
        m_remoteName =
            state.value(QStringLiteral("name")).toString(QStringLiteral("?"));

        QByteArray headEnc = QByteArray::fromBase64(headB64.toLatin1());
        QByteArray anchorEnc = QByteArray::fromBase64(anchorB64.toLatin1());

        if (headEnc.isEmpty())
            return;

        int headPos = m_doc->resolveSticky(headEnc);
        int anchorPos = anchorEnc.isEmpty() ? headPos
                                            : m_doc->resolveSticky(anchorEnc);
        if (headPos < 0)
            return;
        if (anchorPos < 0)
            anchorPos = headPos;

        m_remoteHead = headPos;
        m_remoteAnchor = anchorPos;
        m_remoteColor = color;
        m_hasRemoteCursor = true;

        updateRemoteCursorSelections();
    }

private:
    void updateRemoteCursorSelections()
    {
        if (!m_hasRemoteCursor) {
            m_edit->setExtraSelections({});
            m_edit->viewport()->update();
            return;
        }

        QList<QTextEdit::ExtraSelection> selections;
        int docLen = m_edit->document()->characterCount();

        if (m_remoteHead != m_remoteAnchor) {
            // Selection highlight
            QTextEdit::ExtraSelection sel;
            QColor bg = m_remoteColor;
            bg.setAlpha(50);
            sel.format.setBackground(bg);
            sel.cursor = QTextCursor(m_edit->document());
            int lo = qMin(m_remoteAnchor, m_remoteHead);
            int hi = qMax(m_remoteAnchor, m_remoteHead);
            sel.cursor.setPosition(qMin(lo, docLen - 1));
            sel.cursor.setPosition(qMin(hi, docLen - 1),
                                   QTextCursor::KeepAnchor);
            selections.append(sel);
        }

        // Cursor line: highlight 1 char at head with a left border
        {
            QTextEdit::ExtraSelection cur;
            cur.cursor = QTextCursor(m_edit->document());
            int pos = qMin(m_remoteHead, docLen - 1);
            cur.cursor.setPosition(qMax(pos, 0));
            if (pos < docLen - 1)
                cur.cursor.setPosition(pos + 1, QTextCursor::KeepAnchor);
            QColor bg = m_remoteColor;
            bg.setAlpha(30);
            cur.format.setBackground(bg);
            QPen pen(m_remoteColor, 2);
            cur.format.setProperty(QTextFormat::OutlinePen, QVariant::fromValue(pen));
            selections.append(cur);
        }

        m_edit->setExtraSelections(selections);
        m_edit->viewport()->update();
    }

    bool m_applying = false;
    bool m_hasRemoteCursor = false;
    int m_remoteHead = 0;
    int m_remoteAnchor = 0;
    QColor m_remoteColor;
    QString m_remoteName;
    QString m_displayName;
    QColor m_color;

    CollabText::CollabDocument *m_doc;
    CollabText::SyncManager *m_sync;
    QPlainTextEdit *m_edit;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("CollabText Test Harness"));
        resize(900, 500);

        // Ensure shared folder exists and is clean
        QDir().mkpath(SHARED_FOLDER);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        auto *peerA = new PeerPane(
            QStringLiteral("peer-a"), QStringLiteral("Peer A"),
            QColor(59, 130, 246), central);   // blue
        auto *peerB = new PeerPane(
            QStringLiteral("peer-b"), QStringLiteral("Peer B"),
            QColor(34, 197, 94), central);    // green

        layout->addWidget(peerA);
        layout->addWidget(peerB);
        setCentralWidget(central);

        statusBar()->showMessage(
            QStringLiteral("Syncing via: %1 (500ms cycle)").arg(SHARED_FOLDER));
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"
```

- [ ] **Step 2: Build and verify**

Run: `cd /home/clinton/dev/collabtext/build-dev && make -j$(nproc)`

Expected: Clean build, no errors.

- [ ] **Step 3: Run the app and test**

Run: `cd /home/clinton/dev/collabtext/build-dev && ./app/collabtext-testapp`

Test manually:
1. Type text in Peer A. After ~500ms it should appear in Peer B.
2. Type text in Peer B. After ~500ms it should appear in Peer A.
3. Click in Peer A to set a cursor position. After ~500ms, a colored cursor indicator should appear in Peer B's pane at the corresponding position.
4. Select text in Peer A. After ~500ms, the selection should appear highlighted in Peer B's pane.
5. Press Enter, Backspace, delete text — all should sync without crashes.
6. Verify the shared folder has files: `ls -R /tmp/collabtext-test/`

- [ ] **Step 4: Commit**

```bash
git add app/main.cpp
git commit -m "feat: file-based sync + remote cursor rendering in test harness"
```

---

### Task 4: Add cursor label painting

**Files:**
- Modify: `app/main.cpp`

This is a refinement — painting the peer's name above their cursor. This uses `installEventFilter` on the QPlainTextEdit's viewport to paint after Qt's normal rendering.

- [ ] **Step 1: Add event filter for cursor label painting**

In PeerPane's constructor, after the `m_sync->start()` line, add:

```cpp
m_edit->viewport()->installEventFilter(this);
```

Add a protected `eventFilter` override in PeerPane (above the `private slots:` section):

```cpp
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == m_edit->viewport() && event->type() == QEvent::Paint
            && m_hasRemoteCursor)
        {
            // Let the viewport paint first
            obj->event(event);

            // Now paint our label on top
            QPainter painter(m_edit->viewport());
            painter.setRenderHint(QPainter::Antialiasing);

            QTextCursor tc(m_edit->document());
            int pos = qBound(0, m_remoteHead,
                             m_edit->document()->characterCount() - 1);
            tc.setPosition(pos);
            QRect cr = m_edit->cursorRect(tc);

            // Draw name label above cursor
            QFont font = painter.font();
            font.setPointSize(8);
            font.setBold(true);
            painter.setFont(font);

            QFontMetrics fm(font);
            QString label = m_remoteName;
            int labelW = fm.horizontalAdvance(label) + 8;
            int labelH = fm.height() + 4;

            QRect labelRect(cr.left(), cr.top() - labelH - 2, labelW, labelH);

            // Keep label on screen
            if (labelRect.top() < 0)
                labelRect.moveTop(cr.bottom() + 2);
            if (labelRect.right() > m_edit->viewport()->width())
                labelRect.moveRight(m_edit->viewport()->width());

            painter.setPen(Qt::NoPen);
            painter.setBrush(m_remoteColor);
            painter.drawRoundedRect(labelRect, 3, 3);

            painter.setPen(Qt::white);
            painter.drawText(labelRect, Qt::AlignCenter, label);

            // Draw cursor line
            painter.setPen(QPen(m_remoteColor, 2));
            painter.drawLine(cr.topLeft(), cr.bottomLeft());

            return true; // we handled the paint
        }
        return QWidget::eventFilter(obj, event);
    }
```

Add these includes at the top of main.cpp:

```cpp
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QTextCursor>
```

- [ ] **Step 2: Build and verify**

Run: `cd /home/clinton/dev/collabtext/build-dev && make -j$(nproc)`

Expected: Clean build, no errors.

- [ ] **Step 3: Run and verify cursor labels**

Run: `cd /home/clinton/dev/collabtext/build-dev && ./app/collabtext-testapp`

Test:
1. Type text in Peer A. Click at a position.
2. In Peer B, after ~500ms, a blue "Peer A" label should appear above the cursor position, with a blue cursor line.
3. Type in Peer B, click at a position. In Peer A, a green "Peer B" label should appear.
4. Select text in one pane. The other should show the selection highlight AND the cursor label at the head of the selection.

- [ ] **Step 4: Commit**

```bash
git add app/main.cpp
git commit -m "feat: paint remote cursor name labels above cursor position"
```

---

### Task 5: Clean up stale shared folder on startup

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Clear old data from previous runs**

In `MainWindow`'s constructor, before creating PeerPanes, add cleanup of the shared folder so stale ops from a previous run don't cause confusion:

```cpp
// Clean previous run's data
QDir shared(SHARED_FOLDER);
if (shared.exists())
    shared.removeRecursively();
QDir().mkpath(SHARED_FOLDER);
```

This ensures each launch starts fresh with empty CRDT state on both sides.

- [ ] **Step 2: Build, run, verify**

Run: `cd /home/clinton/dev/collabtext/build-dev && make -j$(nproc) && ./app/collabtext-testapp`

Verify: app starts with empty panes (no stale text from a previous run).

- [ ] **Step 3: Commit**

```bash
git add app/main.cpp
git commit -m "fix: clean shared folder on startup to avoid stale state"
```
