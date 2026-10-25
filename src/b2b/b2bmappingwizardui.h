#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTimer>
#include <memory>

#include "controllers/midi/midimessage.h"
#include "preferences/configobject.h"
#include "preferences/usersettings.h"

class Controller;
class ControllerManager;
class QLabel;
class QPushButton;
class QWidget;

namespace mixxx::b2b {

/// The mapping wizard on the booth (Settings > Mapping wizard > Start). A
/// strip under the top bar talks you through it:
///   1. "click something on the decks or the mixer": only PitchRide's
///      players and mixer count; the click picks, it doesn't press. What
///      you picked is framed.
///   2. "now move it on your controller": the controller is listened to.
///   3. "that's MIXER · CH 1 HI now. try it.": what you moved does it
///      from now (while the wizard runs; saved when you finish).
/// Undo, Pick another, Finish (saves "(yours)": mapwizard::save) or Cancel
/// (nothing saved). While it runs, the booth only listens to the strip.
class MappingWizardUi : public QObject {
    Q_OBJECT
  public:
    explicit MappingWizardUi(QObject* pParent = nullptr);
    ~MappingWizardUi() override;

    /// The window, and the top bar the strip sits under.
    void setWindow(QWidget* pWindow, QWidget* pTopBar);
    void setControllerManager(std::shared_ptr<ControllerManager> pManager, UserSettingsPointer pConfig);
    /// Teach this (MIDI) controller.
    void start(::Controller* pController);
    bool running() const {
        return m_step != Step::Off;
    }

  protected:
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    enum class Step { Off, Pick, Listen, Learned };
    struct Learned {
        ConfigKey control;
        QString name;
        MidiInputMappings mappings;
    };
    void setStep(Step step, const QString& note);
    /// A click on the booth (pressed on pWidget): the control to learn.
    void clicked(QWidget* pWidget);
    void messageReceived(unsigned char status, unsigned char control, unsigned char value);
    /// The controller went quiet after a move: what was it?
    void quiet();
    void nothingCame();
    void undo();
    void finish();
    void cancel();
    void stop();
    /// What's learned so far, live on the controller (so you can try it).
    void pushToController();
    MidiInputMappings allLearned() const;
    void place();
    void frame(QWidget* pWidget);

    QPointer<QWidget> m_pWindow;
    QPointer<QWidget> m_pTopBar;
    std::shared_ptr<ControllerManager> m_pManager;
    UserSettingsPointer m_pConfig;
    QPointer<::Controller> m_pController;
    Step m_step = Step::Off;
    ConfigKey m_control;
    QString m_controlName;
    QList<QPair<MidiKey, unsigned char>> m_messages;
    QList<Learned> m_learned;
    QPointer<QWidget> m_pStrip;
    QLabel* m_pTitle = nullptr;
    QLabel* m_pNote = nullptr;
    QPushButton* m_pUndo = nullptr;
    QPushButton* m_pAnother = nullptr;
    QPushButton* m_pFinish = nullptr;
    QPushButton* m_pCancel = nullptr;
    QPointer<QWidget> m_pFrame;   // round what was picked
    QPointer<QWidget> m_pFramed;  // what was picked
    QTimer m_quiet;      // the controller stopped sending: learn
    QTimer m_waiting;    // nothing came at all
    QTimer m_placing;    // the window moves and resizes
};

} // namespace mixxx::b2b
