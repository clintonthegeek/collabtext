#include "MainWindow.h"
#include "Document.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>

namespace CollabEdit {

MainWindow::MainWindow(CollabText::Identity::Identity identity,
                       std::string replica_name,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_identity(std::move(identity))
    , m_replicaName(std::move(replica_name))
{
    m_doc = std::make_unique<Document>(m_identity, m_replicaName, this);
    setCentralWidget(m_doc->widget());
    connect(m_doc.get(), &Document::changed, this, [this] {
        replaceCentralWidget();
        updateTitleAndStatus();
    });

    buildMenus();
    buildStatusBar();
    resize(900, 700);
    updateTitleAndStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *actNew = fileMenu->addAction(tr("&New"));
    actNew->setShortcut(QKeySequence::New);
    connect(actNew, &QAction::triggered, this, &MainWindow::onFileNew);

    auto *actOpen = fileMenu->addAction(tr("&Open..."));
    actOpen->setShortcut(QKeySequence::Open);
    connect(actOpen, &QAction::triggered, this, &MainWindow::onFileOpen);

    fileMenu->addSeparator();

    m_actSave = fileMenu->addAction(tr("&Save"));
    m_actSave->setShortcut(QKeySequence::Save);
    connect(m_actSave, &QAction::triggered, this, &MainWindow::onFileSave);

    m_actSaveAs = fileMenu->addAction(tr("Save &As..."));
    m_actSaveAs->setShortcut(QKeySequence::SaveAs);
    connect(m_actSaveAs, &QAction::triggered, this, &MainWindow::onFileSaveAs);

    fileMenu->addSeparator();

    m_actClose = fileMenu->addAction(tr("&Close"));
    m_actClose->setShortcut(QKeySequence::Close);
    connect(m_actClose, &QAction::triggered, this, &MainWindow::onFileClose);

    auto *actQuit = fileMenu->addAction(tr("&Quit"));
    actQuit->setShortcut(QKeySequence::Quit);
    connect(actQuit, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    auto *docMenu = menuBar()->addMenu(tr("&Document"));
    m_actEnableCollab = docMenu->addAction(tr("&Enable Collab"));
    connect(m_actEnableCollab, &QAction::triggered, this, &MainWindow::onEnableCollab);

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *actAbout = helpMenu->addAction(tr("&About"));
    connect(actAbout, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::buildStatusBar() {
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);

    m_enableCollabBtn = new QPushButton(tr("Enable Collab"), this);
    statusBar()->addPermanentWidget(m_enableCollabBtn);
    connect(m_enableCollabBtn, &QPushButton::clicked, this, &MainWindow::onEnableCollab);
}

void MainWindow::updateTitleAndStatus() {
    QString name = m_doc->displayName();
    QString mode = m_doc->isCollab() ? tr("collab") : tr("plain");
    QString modified = m_doc->isModified() ? tr("*") : QString();
    setWindowTitle(QStringLiteral("%1%2 [%3] — collabedit")
                       .arg(modified, name, mode));

    QString path = m_doc->path().isEmpty() ? tr("(no file)") : m_doc->path();
    m_statusLabel->setText(QStringLiteral("%1  |  %2  |  replica: %3")
                               .arg(path, mode, QString::fromStdString(m_replicaName)));

    bool canEnable = !m_doc->isCollab() && !m_doc->path().isEmpty();
    m_actEnableCollab->setEnabled(canEnable);
    m_enableCollabBtn->setVisible(canEnable);
    m_actSaveAs->setEnabled(!m_doc->isCollab());
}

void MainWindow::replaceCentralWidget() {
    if (centralWidget() != m_doc->widget()) {
        // takeCentralWidget reparents the old widget away; the Document
        // owns the lifetime of its widgets, so this is safe.
        takeCentralWidget();
        setCentralWidget(m_doc->widget());
    }
}

void MainWindow::openFile(const QString &path) {
    QString err = m_doc->open(path);
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Open failed"), err);
    }
}

void MainWindow::onFileNew() {
    QString err = m_doc->newDoc();
    if (!err.isEmpty()) QMessageBox::warning(this, tr("New failed"), err);
}

void MainWindow::onFileOpen() {
    QString path = QFileDialog::getOpenFileName(this, tr("Open file"));
    if (path.isEmpty()) return;
    openFile(path);
}

void MainWindow::onFileSave() {
    QString err = m_doc->save();
    if (!err.isEmpty()) QMessageBox::warning(this, tr("Save failed"), err);
}

void MainWindow::onFileSaveAs() {
    if (m_doc->isCollab()) return;
    QString path = QFileDialog::getSaveFileName(this, tr("Save as"));
    if (path.isEmpty()) return;
    QString err = m_doc->saveAs(path);
    if (!err.isEmpty()) QMessageBox::warning(this, tr("Save As failed"), err);
}

void MainWindow::onFileClose() {
    m_doc->closeDoc();
}

void MainWindow::onEnableCollab() {
    auto reply = QMessageBox::question(
        this, tr("Enable Collab"),
        tr("Enable collaborative editing on '%1'?\n\n"
           "This creates a sidecar folder next to the file. The sidecar "
           "must be inside a Syncthing-shared folder for remote peers to "
           "join.")
            .arg(m_doc->displayName()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    QString err = m_doc->enableCollab();
    if (!err.isEmpty()) {
        QMessageBox::warning(this, tr("Enable Collab failed"), err);
    }
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("About collabedit"),
        tr("collabedit — minimal CRDT collaborative editor\n\n"
           "Identity: %1\nReplica: %2")
            .arg(QString::fromStdString(m_identity.display_name),
                 QString::fromStdString(m_replicaName)));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    m_doc->closeDoc();
    event->accept();
}

} // namespace CollabEdit
