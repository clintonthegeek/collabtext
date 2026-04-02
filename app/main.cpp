#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QVBoxLayout>

#include <collabtext/CollabDocument.h>

class EditorPane : public QWidget {
    Q_OBJECT
public:
    EditorPane(const QString &label, uint16_t replicaId, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_doc(new CollabText::CollabDocument(replicaId, this))
        , m_edit(new QPlainTextEdit(this))
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(label, this));
        layout->addWidget(m_edit);

        m_edit->setDocument(m_doc->qtDocument());
        m_edit->setPlaceholderText(
            QStringLiteral("Type here (%1)...").arg(label));

        connect(m_edit->document(), &QTextDocument::contentsChange,
                this, &EditorPane::onContentsChange);
    }

    CollabText::CollabDocument *collabDoc() const { return m_doc; }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded)
    {
        if (m_applying) return;
        m_applying = true;

        // Detect format-only changes (e.g. setBlockFormat after Enter)
        if (charsRemoved > 0 && charsAdded > 0) {
            QTextCursor cursor(m_edit->document());
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString newContent = cursor.selectedText();
            newContent.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            QString oldContent = QString::fromStdString(
                m_doc->engine()->text()).mid(position, charsRemoved);
            if (newContent == oldContent) {
                m_applying = false;
                return;
            }
        }

        if (charsRemoved > 0)
            m_doc->engine()->remove(position, charsRemoved);
        if (charsAdded > 0) {
            QTextCursor cursor(m_edit->document());
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString inserted = cursor.selectedText();
            inserted.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            m_doc->engine()->insert(position, inserted.toStdString());
        }

        m_applying = false;
    }

private:
    bool m_applying = false;
    CollabText::CollabDocument *m_doc;
    QPlainTextEdit *m_edit;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("CollabText Test Harness"));
        resize(900, 500);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        auto *paneA = new EditorPane(QStringLiteral("Editor A"), 0, central);
        auto *paneB = new EditorPane(QStringLiteral("Editor B"), 1, central);
        layout->addWidget(paneA);
        layout->addWidget(paneB);
        setCentralWidget(central);

        statusBar()->showMessage(
            QStringLiteral("Native C++ CRDT engine. Sync not yet wired."));
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
