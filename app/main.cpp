#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"
#include <collabtext/CollabDocument.h>

using namespace CollabText::Ui;

class EditorPane : public QWidget {
    Q_OBJECT
public:
    EditorPane(const QString &label, uint16_t replicaId,
               const QColor &cursorColor, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_doc(new CollabText::CollabDocument(replicaId, this))
        , m_edit(new CollabPlainTextEdit(this))
        , m_color(cursorColor)
        , m_label(label)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *header = new QLabel(label, this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;")
                                  .arg(cursorColor.name()));
        layout->addWidget(header);
        layout->addWidget(m_edit);

        m_edit->setDocument(m_doc->qtDocument());
        m_edit->setPlaceholderText(
            QStringLiteral("Type here... (Alt+Click for multi-cursor, "
                           "Ctrl+Alt+Up/Down to add cursors)"));

        auto *statusLabel = new QLabel(this);
        layout->addWidget(statusLabel);
        connect(m_edit->multiCursorController(),
                &MultiCursorController::cursorsChanged, this,
                [this, statusLabel]() {
                    int n = m_edit->multiCursorController()->cursorCount();
                    statusLabel->setText(
                        n > 1 ? QStringLiteral("%1 cursors").arg(n)
                              : QStringLiteral("1 cursor"));
                });
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    CollabText::CollabDocument *collabDoc() const { return m_doc; }
    QColor cursorColor() const { return m_color; }
    QString label() const { return m_label; }

private:
    CollabText::CollabDocument *m_doc;
    CollabPlainTextEdit *m_edit;
    QColor m_color;
    QString m_label;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("CollabText — Multi-Cursor Demo"));
        resize(1000, 600);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        m_paneA = new EditorPane(QStringLiteral("Alice"), 1,
                                  QColor(65, 105, 225), central);
        m_paneB = new EditorPane(QStringLiteral("Bob"), 2,
                                  QColor(220, 20, 60), central);
        layout->addWidget(m_paneA);
        layout->addWidget(m_paneB);
        setCentralWidget(central);

        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncRemoteCursors);
        syncTimer->start(100);

        statusBar()->showMessage(
            QStringLiteral("Alt+Click: add cursor | Ctrl+Alt+Up/Down: column cursor | "
                           "Escape: clear extra cursors"));
    }

private slots:
    void syncRemoteCursors() {
        auto aliceCursor = m_paneA->editor()->textCursor();
        RemoteCursor aliceRemote;
        aliceRemote.position = aliceCursor.position();
        aliceRemote.anchor = aliceCursor.anchor();
        aliceRemote.color = m_paneA->cursorColor();
        aliceRemote.label = m_paneA->label();

        auto bobCursor = m_paneB->editor()->textCursor();
        RemoteCursor bobRemote;
        bobRemote.position = bobCursor.position();
        bobRemote.anchor = bobCursor.anchor();
        bobRemote.color = m_paneB->cursorColor();
        bobRemote.label = m_paneB->label();

        m_paneA->editor()->multiCursorController()->setRemoteCursors({bobRemote});
        m_paneB->editor()->multiCursorController()->setRemoteCursors({aliceRemote});
    }

private:
    EditorPane *m_paneA;
    EditorPane *m_paneB;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"
