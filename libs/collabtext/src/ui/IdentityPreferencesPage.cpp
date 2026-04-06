#include "ui/IdentityPreferencesPage.h"
#include "ui/IdentityEditor.h"

#include <QVBoxLayout>
#include <QPushButton>

#include <chrono>
#include <ctime>

namespace CollabText::Ui {

IdentityPreferencesPage::IdentityPreferencesPage(CollabText::Identity::IdentityStore &store,
                                                  QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_editor(new IdentityEditor(this))
{
    auto *saveButton = new QPushButton(tr("Save"), this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_editor);
    layout->addWidget(saveButton);
    layout->addStretch();
    setLayout(layout);

    auto existing = m_store.load();
    if (existing.has_value())
        m_editor->setIdentity(*existing);

    connect(saveButton, &QPushButton::clicked, this, &IdentityPreferencesPage::onSave);
}

void IdentityPreferencesPage::onSave()
{
    auto id = m_editor->identity();

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    id.updated = buf;

    m_store.save(id);
    emit identitySaved(id);
}

} // namespace CollabText::Ui
