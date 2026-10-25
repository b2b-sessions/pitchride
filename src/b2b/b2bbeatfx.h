#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <array>
#include <functional>
#include <memory>

class ControlObject;
class ControlPotmeter;
class ControlPushButton;
class QWidget;

namespace mixxx::b2b {

class BeatFxScreen;
class BeatFxPad;

/// The club mixer's BEAT FX section (Back2Back, mixer layout 1), like a
/// Pioneer DJM-A9's: effect unit 1 runs the "Club Mixer Beat FX" effect
/// (b2bbeatfxeffect.h) and these controls play it. Everything that makes
/// a sound is one of the effect's parameters, so the other DJ in a session
/// hears (and their mixer shows) the same; the lights and the display
/// follow the parameters, so they show the other DJ's changes too.
///
/// Controls, group [B2BBooth]:
///   beatfx_beat_down / beatfx_beat_up  BEAT ◀ ▶ (and back to beat timing)
///       beatfx_beat_down_light / beatfx_beat_up_light: lit briefly
///   beatfx_time       TIME knob 0..1: manual ms (REVERB: %) for the effect
///   beatfx_on         ON/OFF (a press switches); beatfx_on_light flashes on
///                     the beat while on (or the X-PAD is held)
///   beatfx_auto_tap   AUTO/TAP: hold to go back to AUTO; beatfx_auto_light
///   beatfx_tap        TAP tempo; beatfx_tap_light flashes on every beat
///   beatfx_quantize   QUANTIZE: press = on / off, hold = UTILITY;
///       beatfx_quantize_light
///   beatfx_low / beatfx_mid / beatfx_hi  FX FREQUENCY; *_light
///   beatfx_bpm, beatfx_phase   (for the engine) the tempo and beat of the
///       master / mic / crossfader sides, or of the taps
/// LEVEL/DEPTH is the effect's own parameter4. The display and the X-PAD
/// are drawn into the skin's BeatFxScreen / BeatFxPad.
class BeatFx : public QObject {
    Q_OBJECT
  public:
    explicit BeatFx(int decks, QObject* pParent = nullptr);
    ~BeatFx() override;

    void setWindow(QWidget* pWindow);
    /// The mixer with this section is on screen (the effect is loaded).
    void setActive(bool active);
    bool isActive() const {
        return m_active;
    }
    /// Reset the room: off, and its settings back to the effect's defaults
    /// (level, frequency bands...; each effect's beat to where it starts).
    /// Which effect: the caller picks it (pickedLocally) right after.
    void reset();
    /// The FX SELECT knob picked effect fx (0..13) here.
    void pickedLocally(int fx);
    /// When the other DJ changes the effect or the input: tell the mixer
    /// (its knob and CH SELECT show it).
    void setOnRemoteFx(std::function<void(int)> fn) {
        m_onRemoteFx = std::move(fn);
    }

    // For the display and the pad.
    struct View {
        int fx = 0;
        double beats = 1;
        double timeMs = 0;
        double level = 0.7;
        bool on = false;
        bool xpad = false;
        bool quantize = true;
        bool autoBpm = true;
        double bpm = 0;
        double shownMs = 0; // TIME: the beat value's length, or the manual value
        int wave = 0;
        double phase = 0; // where the beat is
    };
    View view() const;
    /// The values BEAT ◀ ▶ step through for effect fx, and their labels.
    static const QVector<double>& values(int fx);
    static QString label(int fx, double value);
    static QString name(int fx);
    int valueIndex(int fx, double value) const;

    // The X-PAD.
    void padTouched(double x01);
    void padReleased();
    void setWave(int wave);

  private:
    void tick();
    double param(int n) const;
    void setParam(int n, double value);
    void stepBeat(int direction);
    void timeTurned(double v);
    void tapPressed();
    void applyEngaged();
    void sourceTempo(double* pBpm, double* pPhase) const;
    void openUtility();

    int m_numDecks;
    bool m_active = false;
    QPointer<QWidget> m_pWindow;
    QPointer<BeatFxScreen> m_pScreen;
    QPointer<BeatFxPad> m_pPad;
    QTimer m_timer;
    std::function<void(int)> m_onRemoteFx;

    std::unique_ptr<ControlPushButton> m_beatDown;
    std::unique_ptr<ControlPushButton> m_beatUp;
    std::unique_ptr<ControlObject> m_beatDownLight;
    std::unique_ptr<ControlObject> m_beatUpLight;
    QElapsedTimer m_beatDownLit;
    QElapsedTimer m_beatUpLit;
    std::unique_ptr<ControlPotmeter> m_time;
    std::unique_ptr<ControlPushButton> m_on;
    bool m_onState = false; // ON/OFF
    std::unique_ptr<ControlObject> m_onLight;
    std::unique_ptr<ControlPushButton> m_autoTap;
    std::unique_ptr<ControlObject> m_autoLight;
    std::unique_ptr<ControlPushButton> m_tap;
    std::unique_ptr<ControlObject> m_tapLight;
    std::unique_ptr<ControlPushButton> m_quantize;
    std::unique_ptr<ControlObject> m_quantizeLight;
    std::array<std::unique_ptr<ControlPushButton>, 3> m_band;
    std::array<std::unique_ptr<ControlObject>, 3> m_bandLight;

    // Per effect, here: its beat value (index) and manual TIME.
    std::array<int, 14> m_valueIndex{};
    std::array<double, 14> m_manual{};
    int m_lastFx = -1;

    QElapsedTimer m_autoTapHeld;
    QElapsedTimer m_quantizeHeld;
    QTimer m_holdCheck;
    bool m_quantizeHoldDone = false;
    QVector<qint64> m_taps; // ms
    QElapsedTimer m_clock;
    qint64 m_lastTap = -1;
    double m_tapPhaseBase = 0;

    bool m_padHeld = false;
    int m_padIndex = -1;
};

} // namespace mixxx::b2b
