#pragma once
#include "collabtext/Identity.h"
#include "collabtext/IdentityStore.h"
#include <QDialog>

namespace CollabText::Ui {

class IdentityEditor;

class IdentitySetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit IdentitySetupDialog(CollabText::Identity::IdentityStore &store,
                                  QWidget *parent = nullptr);
    CollabText::Identity::Identity identity() const;

private:
    void onAccept();
    CollabText::Identity::IdentityStore &m_store;
    IdentityEditor *m_editor;
    CollabText::Identity::Identity m_identity;
};

} // namespace CollabText::Ui
