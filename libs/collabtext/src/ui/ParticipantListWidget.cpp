#include "ui/ParticipantListWidget.h"
#include "ui/AvatarWidget.h"
#include "ui/PresenceIndicator.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <algorithm>
#include <unordered_map>

namespace CollabText::Ui {

ParticipantListWidget::ParticipantListWidget(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
{
    m_layout->setContentsMargins(4, 4, 4, 4);
    m_layout->setSpacing(2);
}

void ParticipantListWidget::updateParticipants(
    const std::vector<CollabText::Identity::Identity> &identities,
    const std::vector<CollabText::Identity::Presence> &presences)
{
    // Build identity lookup by identity_id
    std::unordered_map<std::string, CollabText::Identity::Identity> id_map;
    for (const auto &id : identities) {
        id_map[id.identity_id] = id;
    }

    // Collapse active presences by identity_id, count devices
    std::unordered_map<std::string, int> device_counts;
    for (const auto &p : presences) {
        if (p.active) {
            device_counts[p.identity_id]++;
        }
    }

    // Build entries — one per identity_id that has at least one active presence
    m_entries.clear();
    for (const auto &[iid, count] : device_counts) {
        ParticipantEntry entry;
        auto it = id_map.find(iid);
        if (it != id_map.end()) {
            entry.identity = it->second;
        } else {
            // No matching identity: use identity_id as display_name
            entry.identity.identity_id = iid;
            entry.identity.display_name = iid;
        }
        entry.device_count = count;
        entry.best_activity = "idle";
        m_entries.push_back(std::move(entry));
    }

    // Sort by display_name
    std::sort(m_entries.begin(), m_entries.end(),
        [](const ParticipantEntry &a, const ParticipantEntry &b) {
            return a.identity.display_name < b.identity.display_name;
        });

    rebuild();
}

void ParticipantListWidget::rebuild()
{
    // Remove all child widgets from the layout
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    for (const auto &entry : m_entries) {
        QWidget *row = new QWidget(this);
        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);

        // Avatar
        auto *avatar = new AvatarWidget(row);
        avatar->setIdentity(entry.identity.display_name, entry.identity.color);
        rowLayout->addWidget(avatar);

        // Name label (bold)
        auto *nameLabel = new QLabel(QString::fromStdString(entry.identity.display_name), row);
        QFont boldFont = nameLabel->font();
        boldFont.setBold(true);
        nameLabel->setFont(boldFont);
        rowLayout->addWidget(nameLabel);

        // Status label (activity + device count)
        QString statusText = QString::fromStdString(entry.best_activity);
        if (entry.device_count > 1) {
            statusText += QString(" (%1 devices)").arg(entry.device_count);
        }
        auto *statusLabel = new QLabel(statusText, row);
        rowLayout->addWidget(statusLabel);

        // Presence indicator
        auto *indicator = new PresenceIndicator(row);
        indicator->setActivity(entry.best_activity);
        rowLayout->addWidget(indicator);

        rowLayout->addStretch();

        // Connect row click via event filter not needed — emit signal on click
        const QString idStr = QString::fromStdString(entry.identity.identity_id);
        connect(nameLabel, &QLabel::linkActivated, this, [this, idStr]() {
            emit participantClicked(idStr);
        });

        m_layout->addWidget(row);
    }

    // Stretch at bottom
    m_layout->addStretch();
}

} // namespace CollabText::Ui
