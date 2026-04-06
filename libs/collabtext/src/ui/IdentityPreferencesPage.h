#pragma once
#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"
#include <QWidget>

namespace CollabText::Ui {

class IdentityEditor;

class IdentityPreferencesPage : public QWidget {
    Q_OBJECT
public:
    explicit IdentityPreferencesPage(CollabText::Identity::IdentityStore &store,
                                     QWidget *parent = nullptr);
signals:
    void identitySaved(const CollabText::Identity::Identity &identity);
private:
    void onSave();
    CollabText::Identity::IdentityStore &m_store;
    IdentityEditor *m_editor;
};

} // namespace CollabText::Ui
