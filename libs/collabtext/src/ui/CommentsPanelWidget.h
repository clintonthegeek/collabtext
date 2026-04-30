#pragma once
#include <QColor>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QString>
#include <QWidget>

namespace CollabText::Ui {

struct CommentDisplayInfo {
    QString id;
    QString authorName;
    QString body;
    QString contextSnippet;
    QColor authorColor;
    bool resolved = false;
};

class CommentsPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommentsPanelWidget(QWidget *parent = nullptr);
    void setComments(const QList<CommentDisplayInfo> &comments);

signals:
    void addCommentRequested(const QString &body);
    void commentClicked(const QString &commentId);
    void resolveRequested(const QString &commentId);
    void unresolveRequested(const QString &commentId);
    void deleteRequested(const QString &commentId);

private:
    void rebuildList();

    QList<CommentDisplayInfo> m_all;
    bool m_showResolved = false;
    QPushButton *m_toggleResolved;
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
