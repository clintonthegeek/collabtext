#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTextCursor>
#include <QTimer>
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

        // Intercept local edits -> mirror into CRDT
        connect(m_edit->document(), &QTextDocument::contentsChange,
                this, &PeerPane::onContentsChange);

        // Capture cursor position changes -> update ephemeral state
        connect(m_edit, &QPlainTextEdit::cursorPositionChanged,
                this, &PeerPane::onCursorPositionChanged);

        // Receive remote cursor data from SyncManager
        connect(m_sync,
                &CollabText::SyncManager::remoteEphemeralChanged,
                this, &PeerPane::onRemoteEphemeral);

        // Start file-based sync
        m_sync->start(SHARED_FOLDER, replicaId);

        // Install event filter for cursor label painting
        m_edit->viewport()->installEventFilter(this);
    }

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

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded)
    {
        if (m_applying || m_doc->crdt()->isApplyingRemote())
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
        if (m_doc->crdt()->isApplyingRemote())
            return;

        // Store plain character offsets. Both peers converge to the same
        // CRDT state via sync, so offsets are meaningful. No yrs transaction
        // needed — avoids all lock contention with the CRDT engine.
        QTextCursor tc = m_edit->textCursor();

        QJsonObject state;
        state[QStringLiteral("name")] = m_displayName;
        state[QStringLiteral("color")] = m_color.name();
        state[QStringLiteral("cursor_head")] = tc.position();
        state[QStringLiteral("cursor_anchor")] = tc.anchor();

        m_sync->setEphemeralState(state);
    }

    void onRemoteEphemeral(const QString &replicaId, const QJsonObject &state)
    {
        Q_UNUSED(replicaId);

        int headPos = state.value(QStringLiteral("cursor_head")).toInt(-1);
        int anchorPos = state.value(QStringLiteral("cursor_anchor")).toInt(headPos);
        QColor color(
            state.value(QStringLiteral("color")).toString(QStringLiteral("#888")));
        m_remoteName =
            state.value(QStringLiteral("name")).toString(QStringLiteral("?"));

        if (headPos < 0)
            return;

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

        // Clean previous run's data
        QDir shared(SHARED_FOLDER);
        if (shared.exists())
            shared.removeRecursively();
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
