#pragma once
#include <QColor>
#include <QLineEdit>
#include <QListWidget>
#include <QString>
#include <QWidget>

namespace CollabText::Ui {

struct CommentDisplayInfo {
    QString id;
    QString authorName;
    QString body;
    QString contextSnippet;
    QColor authorColor;
};

class CommentsPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommentsPanelWidget(QWidget *parent = nullptr);
    void setComments(const QList<CommentDisplayInfo> &comments);

signals:
    void addCommentRequested(const QString &body);
    void commentClicked(const QString &commentId);

private:
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
