#include "ui/IdentitySetupDialog.h"
#include "ui/IdentityEditor.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>

namespace CollabText::Ui {

IdentitySetupDialog::IdentitySetupDialog(CollabText::Identity::IdentityStore &store,
                                          QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_editor(new IdentityEditor(this))
{
    setWindowTitle(QStringLiteral("Welcome to CollabText"));
    setMinimumWidth(350);

    auto *header = new QLabel(
        QStringLiteral("<h2>Welcome to CollabText</h2>"
                       "<p>Set up your identity to get started. "
                       "Others will see this when collaborating with you.</p>"),
        this);
    header->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(header);
    layout->addWidget(m_editor);
    layout->addWidget(buttons);
    setLayout(layout);

    connect(buttons, &QDialogButtonBox::accepted, this, &IdentitySetupDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

CollabText::Identity::Identity IdentitySetupDialog::identity() const
{
    return m_identity;
}

void IdentitySetupDialog::onAccept()
{
    auto editorId = m_editor->identity();
    m_identity = m_store.generate(editorId.display_name);
    m_identity.status = editorId.status;
    m_identity.bio    = editorId.bio;
    m_identity.color  = editorId.color;
    m_store.save(m_identity);
    accept();
}

} // namespace CollabText::Ui
