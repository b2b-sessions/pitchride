#pragma once

#include <QPointer>
#include <memory>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

class Controller;
class ControllerManager;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace mixxx::b2b {

/// Settings > Mapping wizard: for a controller that doesn't act right.
/// Which controller, the mapping it uses, Start (the wizard runs on the
/// booth: click something, move it on the controller), what the wizard has
/// changed (each can be removed), and Back to the original mapping. Built
/// like the other Settings pages, from the same parts.
class MappingWizardPage : public DlgPreferencePage {
    Q_OBJECT
  public:
    MappingWizardPage(QWidget* pParent,
            UserSettingsPointer pConfig,
            std::shared_ptr<ControllerManager> pManager);

  public slots:
    void slotUpdate() override;
    void slotApply() override {
    }
    void slotResetToDefaults() override {
    }

  private:
    ::Controller* selected() const;
    void refresh();
    void start();

    UserSettingsPointer m_pConfig;
    std::shared_ptr<ControllerManager> m_pManager;
    QComboBox* m_pController;
    QLabel* m_pMapping;
    QLabel* m_pNote;
    QPushButton* m_pStart;
    QPushButton* m_pOriginal;
    QWidget* m_pChangesBox;
    QVBoxLayout* m_pChanges;
};

} // namespace mixxx::b2b
