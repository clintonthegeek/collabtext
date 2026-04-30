#pragma once

#include "collabtext/Identity.h"

#include <QObject>
#include <QString>

#include <filesystem>
#include <string>

class QWidget;
class QPlainTextEdit;

namespace CollabEdit {

class CollabPane;

class Document : public QObject {
    Q_OBJECT
public:
    Document(CollabText::Identity::Identity identity,
             std::string replica_name,
             QObject *parent = nullptr);
    ~Document() override;

    QWidget *widget() const;
    QString  displayName() const;
    QString  path() const;
    bool     isCollab() const;
    bool     isModified() const;

    QString newDoc();
    QString open(const QString &path);
    QString save();
    QString saveAs(const QString &path);
    QString enableCollab();
    void    closeDoc();

signals:
    void changed();

private:
    std::filesystem::path sidecarPath(const QString &filePath) const;
    QString openInPlainMode(const QString &path);
    QString openInCollabMode(const QString &path);

    void emitChanged() { emit changed(); }

    CollabText::Identity::Identity m_identity;
    std::string m_replicaName;

    QString m_path;
    bool    m_collab = false;

    QPlainTextEdit *m_plainEdit = nullptr;
    CollabPane     *m_collabPane = nullptr;
    QWidget        *m_emptyWidget = nullptr;
    QWidget        *m_currentWidget = nullptr;
};

} // namespace CollabEdit
