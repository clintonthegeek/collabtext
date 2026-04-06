#include "ui/IdentityEditor.h"
#include "ui/AvatarWidget.h"

#include <QFormLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QColorDialog>

namespace CollabText::Ui {

IdentityEditor::IdentityEditor(QWidget *parent)
    : QWidget(parent)
    , m_nameEdit(new QLineEdit(this))
    , m_statusEdit(new QLineEdit(this))
    , m_bioEdit(new QTextEdit(this))
    , m_colorButton(new QPushButton(this))
    , m_avatar(new AvatarWidget(this))
{
    m_bioEdit->setMaximumHeight(60);

    m_colorButton->setFixedSize(32, 32);
    m_colorButton->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #ccc;")
            .arg(m_color.name()));

    auto *layout = new QFormLayout(this);
    layout->addRow(m_avatar);
    layout->addRow(tr("Name"), m_nameEdit);
    layout->addRow(tr("Status"), m_statusEdit);
    layout->addRow(tr("Bio"), m_bioEdit);
    layout->addRow(tr("Color"), m_colorButton);
    setLayout(layout);

    connect(m_colorButton, &QPushButton::clicked, this, &IdentityEditor::onColorClicked);

    auto emitChanged = [this] { emit identityChanged(); };
    connect(m_nameEdit,   &QLineEdit::textChanged,         this, emitChanged);
    connect(m_statusEdit, &QLineEdit::textChanged,         this, emitChanged);
    connect(m_bioEdit,    &QTextEdit::textChanged,         this, emitChanged);
}

void IdentityEditor::setIdentity(const CollabText::Identity::Identity &id)
{
    m_identityId = QString::fromStdString(id.identity_id);
    m_publicKey  = QString::fromStdString(id.public_key);
    m_updated    = QString::fromStdString(id.updated);

    // Block signals while populating to avoid spurious identityChanged emissions
    m_nameEdit->blockSignals(true);
    m_statusEdit->blockSignals(true);
    m_bioEdit->blockSignals(true);

    m_nameEdit->setText(QString::fromStdString(id.display_name));
    m_statusEdit->setText(QString::fromStdString(id.status));
    m_bioEdit->setPlainText(QString::fromStdString(id.bio));

    m_nameEdit->blockSignals(false);
    m_statusEdit->blockSignals(false);
    m_bioEdit->blockSignals(false);

    QColor c(QString::fromStdString(id.color));
    if (c.isValid())
        m_color = c;
    else
        m_color = Qt::gray;

    m_colorButton->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #ccc;")
            .arg(m_color.name()));

    m_avatar->setIdentity(id.display_name, id.color);
}

CollabText::Identity::Identity IdentityEditor::identity() const
{
    CollabText::Identity::Identity id;
    id.identity_id  = m_identityId.toStdString();
    id.display_name = m_nameEdit->text().toStdString();
    id.status       = m_statusEdit->text().toStdString();
    id.bio          = m_bioEdit->toPlainText().toStdString();
    id.color        = m_color.name().toStdString();
    id.public_key   = m_publicKey.toStdString();
    id.updated      = m_updated.toStdString();
    return id;
}

void IdentityEditor::onColorClicked()
{
    QColor chosen = QColorDialog::getColor(m_color, this, tr("Choose Color"));
    if (!chosen.isValid())
        return;

    m_color = chosen;
    m_colorButton->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #ccc;")
            .arg(m_color.name()));

    m_avatar->setIdentity(m_nameEdit->text().toStdString(),
                          m_color.name().toStdString());
    emit identityChanged();
}

} // namespace CollabText::Ui
