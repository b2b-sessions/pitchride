#include "b2b/b2bmappingwizardui.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>

#include "b2b/b2bmappingwizard.h"
#include "b2b/b2btheme.h"
#include "controllers/controller.h"
#include "controllers/controllerlearningeventfilter.h"
#include "controllers/controllermanager.h"
#include "controllers/learningutils.h"
#include "controllers/midi/midicontroller.h"
#include "controllers/midi/midiutils.h"
#include "moc_b2bmappingwizardui.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kStripHeight = 64;
constexpr int kQuietMs = 700;       // after the last message from a move
constexpr int kWaitingMs = 12000;   // nothing at all from the controller
constexpr int kFramePx = 3;

QString stripStyle() {
    using namespace theme;
    return QStringLiteral(
                   "QFrame#MappingWizard { background: %1; border: 0; border-bottom: 1px solid %2; }"
                   "QLabel { background: transparent; }"
                   "QLabel#Title { color: %3; %4 font-size: 11px; letter-spacing: 1px; }"
                   "QLabel#Note { color: %5; %6 font-size: 14px; }")
                   .arg(QLatin1String(kGround),
                           QLatin1String(kKeyline),
                           QLatin1String(kCopper),
                           css(Face::Label),
                           QLatin1String(kType),
                           css(Face::Voice)) +
            buttons(true);
}

bool inside(QObject* pObject, QWidget* pAncestor) {
    for (QWidget* p = qobject_cast<QWidget*>(pObject); p; p = p->parentWidget()) {
        if (p == pAncestor) {
            return true;
        }
    }
    return false;
}
} // namespace

MappingWizardUi::MappingWizardUi(QObject* pParent)
        : QObject(pParent) {
    qRegisterMetaType<MidiInputMappings>("MidiInputMappings");
    m_quiet.setSingleShot(true);
    m_quiet.setInterval(kQuietMs);
    connect(&m_quiet, &QTimer::timeout, this, &MappingWizardUi::quiet);
    m_waiting.setSingleShot(true);
    m_waiting.setInterval(kWaitingMs);
    connect(&m_waiting, &QTimer::timeout, this, &MappingWizardUi::nothingCame);
    m_placing.setInterval(300);
    connect(&m_placing, &QTimer::timeout, this, &MappingWizardUi::place);
}

MappingWizardUi::~MappingWizardUi() {
    if (running()) {
        stop();
    }
}

void MappingWizardUi::setWindow(QWidget* pWindow, QWidget* pTopBar) {
    m_pWindow = pWindow;
    m_pTopBar = pTopBar;
}

void MappingWizardUi::setControllerManager(std::shared_ptr<ControllerManager> pManager,
        UserSettingsPointer pConfig) {
    m_pManager = std::move(pManager);
    m_pConfig = std::move(pConfig);
}

void MappingWizardUi::start(::Controller* pController) {
    auto* pMidi = qobject_cast<MidiController*>(pController);
    if (running() || !pMidi || !m_pWindow || !m_pManager || mapwizard::inRoom()) {
        return;
    }
    qInfo() << "B2B: the mapping wizard starts for" << pController->getName();
    m_pController = pController;
    m_learned.clear();
    m_messages.clear();
    if (!m_pStrip) {
        auto* pStrip = new QFrame(m_pWindow);
        pStrip->setObjectName(QStringLiteral("MappingWizard"));
        pStrip->setStyleSheet(stripStyle());
        auto* pRow = new QHBoxLayout(pStrip);
        pRow->setContentsMargins(28, 0, 24, 0);
        pRow->setSpacing(10);
        auto* pText = new QVBoxLayout();
        pText->setSpacing(4);
        pText->addStretch(1);
        m_pTitle = new QLabel(pStrip);
        m_pTitle->setObjectName(QStringLiteral("Title"));
        pText->addWidget(m_pTitle);
        m_pNote = new QLabel(pStrip);
        m_pNote->setObjectName(QStringLiteral("Note"));
        m_pNote->setTextFormat(Qt::PlainText);
        pText->addWidget(m_pNote);
        pText->addStretch(1);
        pRow->addLayout(pText, 1);
        auto button = [pStrip, pRow](const QString& text, const char* name) {
            auto* pButton = new QPushButton(text, pStrip);
            if (name) {
                pButton->setObjectName(QLatin1String(name));
            }
            pButton->setCursor(Qt::PointingHandCursor);
            pButton->setFocusPolicy(Qt::NoFocus);
            theme::caps(pButton);
            pRow->addWidget(pButton, 0, Qt::AlignVCenter);
            return pButton;
        };
        m_pUndo = button(tr("Undo"), nullptr);
        m_pAnother = button(tr("Pick another"), nullptr);
        m_pFinish = button(tr("Finish"), "Primary");
        m_pCancel = button(tr("Cancel"), "Quiet");
        connect(m_pUndo, &QPushButton::clicked, this, &MappingWizardUi::undo);
        connect(m_pAnother, &QPushButton::clicked, this, [this] {
            setStep(Step::Pick, tr("click something on the decks or the mixer."));
        });
        connect(m_pFinish, &QPushButton::clicked, this, &MappingWizardUi::finish);
        connect(m_pCancel, &QPushButton::clicked, this, &MappingWizardUi::cancel);
        m_pStrip = pStrip;
    }
    if (!m_pFrame) {
        m_pFrame = new QWidget(m_pWindow);
        m_pFrame->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_pFrame->setStyleSheet(QStringLiteral("background: transparent; border: %1px solid %2;")
                                        .arg(kFramePx)
                                        .arg(QLatin1String(theme::kAction)));
        m_pFrame->hide();
    }
    // Listen to the controller: everything it sends comes here too (and
    // what's learned works at once, while the rest acts as it did).
    connect(pMidi, &MidiController::messageReceived, this, &MappingWizardUi::messageReceived, Qt::QueuedConnection);
    // Switched off (Settings > Controllers): on, or it can't be heard.
    mapwizard::switchOn(m_pConfig, m_pManager.get(), pController);
    QMetaObject::invokeMethod(pMidi, "startLearning", Qt::QueuedConnection); // (a protected slot)
    qApp->installEventFilter(this);
    m_placing.start();
    m_pStrip->show();
    m_pStrip->raise();
    setStep(Step::Pick,
            tr("click something on the decks or the mixer. your controller's left side is deck 1, "
               "its right side deck 2."));
}

void MappingWizardUi::setStep(Step step, const QString& note) {
    m_step = step;
    m_quiet.stop();
    if (step == Step::Listen) {
        m_messages.clear();
        m_waiting.start();
    } else {
        m_waiting.stop();
    }
    if (step == Step::Pick) {
        frame(nullptr);
    }
    const QString controller = m_pController ? m_pController->getName() : QString();
    m_pTitle->setText(tr("Mapping wizard \xC2\xB7 %1").arg(controller).toUpper());
    m_pNote->setText(note);
    m_pUndo->setVisible(!m_learned.isEmpty() && step != Step::Listen);
    m_pAnother->setVisible(step == Step::Learned);
    m_pFinish->setText(m_learned.isEmpty() ? tr("Close") : tr("Finish"));
    place();
}

bool MappingWizardUi::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (!running() || !m_pWindow) {
        return false;
    }
    switch (pEvent->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
        break;
    default:
        return false;
    }
    // The strip is used as ever; the rest of the window only picks.
    if (!inside(pObject, m_pWindow) || inside(pObject, m_pStrip)) {
        return false;
    }
    if (pEvent->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent*>(pEvent)->button() == Qt::LeftButton) {
        clicked(qobject_cast<QWidget*>(pObject));
    }
    return true;
}

void MappingWizardUi::clicked(QWidget* pWidget) {
    if (!pWidget || !m_pManager) {
        return;
    }
    ControllerLearningEventFilter* pFilter = m_pManager->getControllerLearningEventFilter();
    ConfigKey key;
    QWidget* pFound = nullptr;
    for (QWidget* p = pWidget; p && p != m_pWindow; p = p->parentWidget()) {
        if (pFilter->controlFor(p, &key)) {
            pFound = p;
            break;
        }
    }
    const QString name = pFound ? mapwizard::controlName(key) : QString();
    if (name.isEmpty()) {
        m_pNote->setText(tr("that one can't be learned. click a knob, fader or button on the decks or the mixer."));
        return;
    }
    m_control = key;
    m_controlName = name;
    frame(pFound);
    setStep(Step::Listen, tr("now move %1 on your controller.").arg(name));
}

void MappingWizardUi::messageReceived(unsigned char status, unsigned char control, unsigned char value) {
    if (m_step != Step::Listen) {
        return;
    }
    const MidiKey key(status, control);
    if (MidiUtils::isClockSignal(key)) {
        return;
    }
    m_waiting.stop();
    m_messages.append(qMakePair(key, value));
    m_quiet.start(); // learn once it stops
}

void MappingWizardUi::quiet() {
    if (m_step != Step::Listen) {
        return;
    }
    const MidiInputMappings mappings = LearningUtils::guessMidiInputMappings(m_control, m_messages);
    m_messages.clear();
    if (mappings.isEmpty()) {
        setStep(Step::Listen,
                tr("couldn't tell what you moved. move %1 again, one thing at a time.").arg(m_controlName));
        return;
    }
    // A control learned again, or an input that did something else here:
    // the new one wins.
    for (int i = m_learned.size() - 1; i >= 0; --i) {
        bool replaced = m_learned[i].control == m_control;
        for (const MidiInputMapping& old : std::as_const(m_learned[i].mappings)) {
            for (const MidiInputMapping& now : mappings) {
                replaced = replaced || old.key.key == now.key.key;
            }
        }
        if (replaced) {
            m_learned.removeAt(i);
        }
    }
    m_learned.append(Learned{m_control, m_controlName, mappings});
    pushToController();
    qInfo() << "B2B: the mapping wizard learned" << m_control.group << m_control.item;
    setStep(Step::Learned, tr("that's %1 now. try it.").arg(m_controlName));
}

void MappingWizardUi::nothingCame() {
    if (m_step == Step::Listen) {
        m_pNote->setText(tr("nothing came from your controller. move %1 again, or pick something else.")
                                 .arg(m_controlName));
        m_waiting.start();
    }
}

void MappingWizardUi::undo() {
    if (m_learned.isEmpty()) {
        return;
    }
    const QString name = m_learned.takeLast().name;
    pushToController();
    setStep(Step::Pick, tr("%1 is as it was. click something on the decks or the mixer.").arg(name));
}

MidiInputMappings MappingWizardUi::allLearned() const {
    MidiInputMappings all;
    for (const Learned& learned : m_learned) {
        all.append(learned.mappings);
    }
    return all;
}

void MappingWizardUi::pushToController() {
    if (!m_pController) {
        return;
    }
    // (The controller's own thread: queued, in this order.)
    QMetaObject::invokeMethod(m_pController, "clearTemporaryInputMappings", Qt::QueuedConnection);
    const MidiInputMappings all = allLearned();
    if (!all.isEmpty()) {
        QMetaObject::invokeMethod(m_pController,
                "learnTemporaryInputMappings",
                Qt::QueuedConnection,
                Q_ARG(MidiInputMappings, all));
    }
}

void MappingWizardUi::finish() {
    const MidiInputMappings all = allLearned();
    if (all.isEmpty() || !m_pController) {
        stop();
        return;
    }
    QString saved;
    const bool ok = mapwizard::save(m_pConfig, m_pManager.get(), m_pController, all, &saved);
    stop();
    if (m_pWindow) {
        toast(m_pWindow,
                ok ? tr("saved as %1. change it any time in settings, mapping wizard.").arg(saved)
                   : tr("couldn't save the mapping. nothing was changed."));
    }
}

void MappingWizardUi::cancel() {
    const bool any = !m_learned.isEmpty();
    m_learned.clear();
    stop();
    if (any && m_pWindow) {
        toast(m_pWindow, tr("nothing was saved. your controller is as it was."));
    }
}

void MappingWizardUi::stop() {
    qInfo() << "B2B: the mapping wizard ends";
    m_step = Step::Off;
    m_quiet.stop();
    m_waiting.stop();
    m_placing.stop();
    qApp->removeEventFilter(this);
    if (m_pController) {
        disconnect(m_pController, nullptr, this, nullptr);
        QMetaObject::invokeMethod(m_pController, "clearTemporaryInputMappings", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_pController, "stopLearning", Qt::QueuedConnection);
    }
    m_pController.clear();
    frame(nullptr);
    if (m_pStrip) {
        m_pStrip->hide();
    }
}

void MappingWizardUi::frame(QWidget* pWidget) {
    m_pFramed = pWidget;
    if (m_pFrame) {
        m_pFrame->setVisible(pWidget != nullptr);
    }
    place();
}

void MappingWizardUi::place() {
    if (!m_pWindow) {
        return;
    }
    if (m_pStrip && m_pStrip->isVisible()) {
        const int top = m_pTopBar ? m_pTopBar->mapTo(m_pWindow, QPoint(0, m_pTopBar->height())).y() : 0;
        m_pStrip->setGeometry(0, top, m_pWindow->width(), kStripHeight);
        m_pStrip->raise();
    }
    if (m_pFrame && m_pFramed && m_pFramed->isVisible()) {
        const QRect r(m_pFramed->mapTo(m_pWindow, QPoint(0, 0)), m_pFramed->size());
        m_pFrame->setGeometry(r.adjusted(-kFramePx - 2, -kFramePx - 2, kFramePx + 2, kFramePx + 2));
        m_pFrame->show();
        m_pFrame->raise();
    } else if (m_pFrame) {
        m_pFrame->hide();
    }
}

} // namespace mixxx::b2b
