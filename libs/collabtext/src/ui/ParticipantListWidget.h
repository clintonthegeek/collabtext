#pragma once
#include "collabtext/Identity.h"
#include <QWidget>
#include <QVBoxLayout>
#include <string>
#include <vector>

namespace CollabText::Ui {

class ParticipantListWidget : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantListWidget(QWidget *parent = nullptr);

    void updateParticipants(
        const std::vector<CollabText::Identity::Identity> &identities,
        const std::vector<CollabText::Identity::Presence> &presences);

signals:
    void participantClicked(const QString &identityId);

private:
    void rebuild();

    struct ParticipantEntry {
        CollabText::Identity::Identity identity;
        int device_count = 0;
        std::string best_activity;
    };

    std::vector<ParticipantEntry> m_entries;
    QVBoxLayout *m_layout;
};

} // namespace CollabText::Ui
