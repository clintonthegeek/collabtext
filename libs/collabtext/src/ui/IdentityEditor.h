#pragma once
#include "collabtext/Identity.h"
#include <QWidget>
#include <QColor>

class QLineEdit;
class QTextEdit;
class QPushButton;

namespace CollabText::Ui {

class AvatarWidget;

class IdentityEditor : public QWidget {
    Q_OBJECT
public:
    explicit IdentityEditor(QWidget *parent = nullptr);
    void setIdentity(const CollabText::Identity::Identity &identity);
    CollabText::Identity::Identity identity() const;
signals:
    void identityChanged();
private:
    void onColorClicked();
    QLineEdit *m_nameEdit;
    QLineEdit *m_statusEdit;
    QTextEdit *m_bioEdit;
    QPushButton *m_colorButton;
    AvatarWidget *m_avatar;
    QString m_identityId;
    QString m_publicKey;
    QString m_updated;
    QColor m_color{Qt::gray};
};

} // namespace CollabText::Ui
