#include "b2b/b2bmappingwizardpage.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "b2b/b2bmappingwizard.h"
#include "controllers/controller.h"
#include "controllers/controllermanager.h"
#include "moc_b2bmappingwizardpage.cpp"

namespace mixxx::b2b {

MappingWizardPage::MappingWizardPage(QWidget* pParent,
        UserSettingsPointer pConfig,
        std::shared_ptr<ControllerManager> pManager)
        : DlgPreferencePage(pParent),
          m_pConfig(std::move(pConfig)),
          m_pManager(std::move(pManager)) {
    auto* pPage = new QVBoxLayout(this);

    // What it's for, and the controller.
    auto* pControllerBox = new QGroupBox(tr("Your controller"), this);
    auto* pControllerCol = new QVBoxLayout(pControllerBox);
    auto* pAbout = new QLabel(
            tr("If a knob, fader or button on your controller doesn't do what it should, teach it here. "
               "Start the wizard, click something on the decks or the mixer, then move it on your "
               "controller. Map your controller's left side onto deck 1 and its right side onto deck 2: "
               "in a room, they play your own decks."),
            pControllerBox);
    pAbout->setWordWrap(true);
    pControllerCol->addWidget(pAbout);
    auto* pPickRow = new QHBoxLayout();
    pPickRow->addWidget(new QLabel(tr("Controller:"), pControllerBox));
    m_pController = new QComboBox(pControllerBox);
    m_pController->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    pPickRow->addWidget(m_pController, 1);
    pControllerCol->addLayout(pPickRow);
    m_pMapping = new QLabel(pControllerBox);
    m_pMapping->setWordWrap(true);
    pControllerCol->addWidget(m_pMapping);
    m_pNote = new QLabel(pControllerBox);
    m_pNote->setWordWrap(true);
    pControllerCol->addWidget(m_pNote);
    auto* pButtons = new QHBoxLayout();
    m_pStart = new QPushButton(tr("Start the wizard"), pControllerBox);
    m_pOriginal = new QPushButton(tr("Back to the original mapping"), pControllerBox);
    pButtons->addWidget(m_pStart);
    pButtons->addWidget(m_pOriginal);
    pButtons->addStretch(1);
    pControllerCol->addLayout(pButtons);
    pPage->addWidget(pControllerBox);

    // What the wizard changed.
    auto* pChangesBox = new QGroupBox(tr("What you've changed"), this);
    m_pChangesBox = pChangesBox;
    m_pChanges = new QVBoxLayout(pChangesBox);
    pPage->addWidget(pChangesBox);
    pPage->addStretch(1);

    connect(m_pController, &QComboBox::currentIndexChanged, this, &MappingWizardPage::refresh);
    connect(m_pStart, &QPushButton::clicked, this, &MappingWizardPage::start);
    connect(m_pOriginal, &QPushButton::clicked, this, [this] {
        if (::Controller* pController = selected()) {
            mapwizard::backToOriginal(m_pConfig, m_pManager.get(), pController);
        }
    });
    // A mapping saved or switched (here, or by the wizard): show it. (The
    // controller takes it on its own thread, a moment later.)
    connect(mapwizard::Notifier::instance(), &mapwizard::Notifier::mappingsChanged, this, [this] {
        QTimer::singleShot(400, this, &MappingWizardPage::refresh);
    });
}

::Controller* MappingWizardPage::selected() const {
    const QString name = m_pController->currentData().toString();
    const QList<::Controller*> controllers = mapwizard::midiControllers(m_pManager.get());
    for (::Controller* pController : controllers) {
        if (pController->getName() == name) {
            return pController;
        }
    }
    return nullptr;
}

void MappingWizardPage::slotUpdate() {
    const QString was = m_pController->currentData().toString();
    m_pController->blockSignals(true);
    m_pController->clear();
    const QList<::Controller*> controllers = mapwizard::midiControllers(m_pManager.get());
    for (::Controller* pController : controllers) {
        m_pController->addItem(pController->getName(), pController->getName());
    }
    const int at = m_pController->findData(was);
    m_pController->setCurrentIndex(at >= 0 ? at : 0);
    m_pController->blockSignals(false);
    refresh();
}

void MappingWizardPage::refresh() {
    ::Controller* pController = selected();
    // (The changes, made again below.)
    while (QLayoutItem* pItem = m_pChanges->takeAt(0)) {
        delete pItem->widget();
        delete pItem;
    }
    if (!pController) {
        m_pMapping->setText(tr("No controller found. Plug yours in, then open Settings again."));
        m_pNote->hide();
        m_pStart->setEnabled(false);
        m_pOriginal->hide();
        m_pChangesBox->hide();
        return;
    }
    const mapwizard::Files files = mapwizard::files(m_pConfig, m_pManager.get(), pController);
    m_pMapping->setText(files.currentName.isEmpty()
                    ? tr("Mapping in use: none. The wizard starts one for it.")
                    : tr("Mapping in use: %1").arg(files.currentName));
    const bool inRoom = mapwizard::inRoom();
    m_pNote->setText(tr("Leave the room to use the wizard: in a room, what you move goes into the shared mix."));
    m_pNote->setVisible(inRoom);
    m_pStart->setEnabled(!inRoom);
    m_pOriginal->setVisible(files.yours);

    const QList<mapwizard::Change> changes = mapwizard::changes(m_pConfig, m_pManager.get(), pController);
    m_pChangesBox->setVisible(files.yours);
    if (changes.isEmpty()) {
        m_pChanges->addWidget(new QLabel(tr("Nothing yet."), m_pChangesBox));
    }
    for (const mapwizard::Change& change : changes) {
        auto* pRow = new QWidget(m_pChangesBox);
        auto* pRowLayout = new QHBoxLayout(pRow);
        pRowLayout->setContentsMargins(0, 0, 0, 0);
        pRowLayout->addWidget(new QLabel(change.name.isEmpty() ? change.control.group + QLatin1Char(',') +
                                                       change.control.item
                                                               : change.name,
                                      pRow),
                1);
        auto* pRemove = new QPushButton(tr("Remove"), pRow);
        const QList<uint16_t> midiKeys = change.midiKeys;
        connect(pRemove, &QPushButton::clicked, this, [this, midiKeys] {
            if (::Controller* pController = selected()) {
                mapwizard::removeChange(m_pConfig, m_pManager.get(), pController, midiKeys);
            }
        });
        pRowLayout->addWidget(pRemove);
        m_pChanges->addWidget(pRow);
    }
}

void MappingWizardPage::start() {
    ::Controller* pController = selected();
    if (!pController || mapwizard::inRoom()) {
        return;
    }
    emit mapwizard::Notifier::instance()->startRequested(pController);
    // The wizard runs on the booth: Settings out of the way.
    if (QWidget* pWindow = window()) {
        pWindow->close();
    }
}

} // namespace mixxx::b2b
