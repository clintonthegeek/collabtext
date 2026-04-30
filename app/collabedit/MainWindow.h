#pragma once

#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"

#include <QMainWindow>

#include <memory>
#include <string>

class QAction;
class QLabel;
class QPushButton;

namespace CollabEdit {

class Document;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(CollabText::Identity::Identity identity,
               std::string replica_name,
               QWidget *parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onFileNew();
    void onFileOpen();
    void onFileSave();
    void onFileSaveAs();
    void onFileClose();
    void onEnableCollab();
    void onAbout();

private:
    void buildMenus();
    void buildStatusBar();
    void updateTitleAndStatus();
    void replaceCentralWidget();

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    std::unique_ptr<Document> m_doc;

    QAction *m_actEnableCollab = nullptr;
    QAction *m_actSave = nullptr;
    QAction *m_actSaveAs = nullptr;
    QAction *m_actClose = nullptr;

    QLabel *m_statusLabel = nullptr;
    QPushButton *m_enableCollabBtn = nullptr;
};

} // namespace CollabEdit
