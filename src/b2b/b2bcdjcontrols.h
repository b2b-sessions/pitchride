#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QObject>
#include <QTimer>
#include <memory>
#include <vector>

class ControlObject;
class ControlPotmeter;
class ControlProxy;
class ControlPushButton;

namespace mixxx::b2b {

/// Controls that make the B2B skins' players behave like the CDJ-3000 they
/// are drawn from, where Mixxx has no control that does the same thing. All are in [B2BBooth]; other skins never touch them.
///
/// Players (N = deck):
///   deckN_play          PLAY/PAUSE; in VINYL mode it starts and stops at the
///                       speed the VINYL SPEED ADJUST knob sets (CDJ: at once)
///   deckN_vinyl_speed   0 = instant .. 1 = slowest start / stop (about 3.5 s);
///                       VINYL mode only: also how the record slows under a
///                       hand on the jog and spins back up when let go
///   deckN_reverse       DIRECTION REV: stays on until pressed again
///   deckN_track_prev    TRACK SEARCH ◀◀: back to the start of the track; at
///                       the start, the previous track in the library list
///   deckN_track_next    TRACK SEARCH ▶▶: the next track in the library list
///   deckN_call_prev / deckN_call_next
///                       CUE/LOOP CALL ◀ ▶: the previous / next cue (hot cues
///                       and the cue point); paused, it becomes the cue point
///   deckN_loop_in / deckN_loop_out
///                       IN/CUE and OUT as on a CDJ: set the loop's start /
///                       end (on the beat with QUANTIZE, by Mixxx); pressed
///                       while the loop plays: IN / OUT ADJUST, the jog moves
///                       that point (whole beats with QUANTIZE, finely
///                       without); pressed again (or 10 s idle): done
///   deckN_reloop        RELOOP/EXIT (also ends an adjust)
///   deckN_loop4 / deckN_loop8
///                       4 / 8 BEAT: no loop playing, a 4 / 8 beat loop
///                       from here forwards (from the nearest beat with
///                       QUANTIZE); a loop playing (however it started):
///                       4 BEAT halves it, 8 BEAT doubles it, from the same
///                       start (no further than Mixxx's 1/32 .. 512 beats)
///   deckN_loop_beats    the playing loop's length in beats (0: no loop
///                       playing; -1: one with no beat grid to count it)
///   deckN_loop_in_light / deckN_loop_out_light / deckN_reloop_light
///   deckN_loop4_light / deckN_loop8_light (a 4 / 8 beat loop is playing)
///                       the keys' lights, as a CDJ's (flashing on the beat
///                       while looping, fast while adjusting)
///   deckN_jog_feel      JOG FEEL: 0 LIGHT (sensitive, instant, lets go at
///                       once) .. 0.5 (Mixxx's own jog) .. 1 HEAVY (needs more
///                       turning, smoother, settles back more gradually)
///   deckN_jog_cdj       JOG MODE: 1 = CDJ (the jog bends the pitch, or
///                       moves through the track when paused), 0 = VINYL (the
///                       top scratches, the outer ring bends)
///   The jog: a widget named "JogN" over the whole platter (its outer fifth
///   is the side ring) and a Spinny named "JogTopN" for the display in its
///   middle. A browse knob named "RotaryN" scrolls the library with the
///   mouse wheel.
///
class CdjControls : public QObject {
    Q_OBJECT
  public:
    explicit CdjControls(int decks = 4, QObject* pParent = nullptr);
    ~CdjControls() override;

  private:
    struct Deck {
        std::unique_ptr<ControlPushButton> play;
        std::unique_ptr<ControlPotmeter> vinylSpeed;
        std::unique_ptr<ControlPushButton> reverse;
        std::unique_ptr<ControlPushButton> trackPrev;
        std::unique_ptr<ControlPushButton> trackNext;
        std::unique_ptr<ControlPushButton> callPrev;
        std::unique_ptr<ControlPushButton> callNext;
        std::unique_ptr<ControlPushButton> jogCdj;
        std::unique_ptr<ControlPotmeter> jogFeel; // 0 LIGHT .. 0.5 (as before) .. 1 HEAVY
        // Loops, the CDJ way
        std::unique_ptr<ControlPushButton> loopIn;
        std::unique_ptr<ControlPushButton> loop4;
        std::unique_ptr<ControlPushButton> loop8;
        std::unique_ptr<ControlPushButton> loopOut;
        std::unique_ptr<ControlPushButton> reloop;
        std::unique_ptr<ControlObject> lightIn;
        std::unique_ptr<ControlObject> lightOut;
        std::unique_ptr<ControlObject> lightReloop;
        std::unique_ptr<ControlObject> light4; // a 4-beat loop is playing
        std::unique_ptr<ControlObject> light8;
        std::unique_ptr<ControlObject> loopBeats; // the playing loop's length in beats
        // The tempo fader: held with the mouse, it moves in the range's steps.
        std::unique_ptr<ControlProxy> rate;
        bool faderHeld = false;
        int adjust = 0; // 1 IN ADJUST, 2 OUT ADJUST (the jog moves that point)
        bool adjusting = false; // the jog held, adjusting
        double adjustTurn = 0;  // degrees turned, not yet a whole beat
        QElapsedTimer adjustIdle;
        double loadedSamples = -1; // a new track clears the adjust mode
        // Pitch bend while playing: the jog's pushes, averaged over a
        // window (as Mixxx's own jog does), sent as the deck's speed so a
        // friend in a session hears it too.
        QList<QPair<qint64, double>> bendPushes; // (ms, push)
        QTimer bendTick;
        bool bendActive = false;
        // Start / stop ramp
        QTimer ramp;
        QElapsedTimer rampClock;
        double rampMs = 0;
        double rampFrom = 0;
        double rampTo = 0;
        bool rampStops = false; // play goes off at the end (PLAY/PAUSE stopping)
        bool rampHold = false;  // the hand on the jog: hold the record still at the end
        // Jog dragged with the mouse: bending (CDJ mode, or the outer ring)
        // or scratching (vinyl mode, on top)
        bool jogging = false;
        bool scratching = false;
        double jogAngle = 0;
        double scratchSpeed = 0;
        QElapsedTimer scratchClock;
        QTimer scratchStill; // the hand stops: so does the record
    };

    bool eventFilter(QObject* pObject, QEvent* pEvent) override;
    bool jogEvent(int deck, QWidget* pJog, QEvent* pEvent, bool top);

    void playPressed(int deck);
    /// Speed from the current speed to `to` (0 or full), as fast as VINYL
    /// SPEED ADJUST says: a whole stop takes rampMs(); a part of one, its part.
    void startRamp(int deck, double to, bool stops, bool hold, double ms = -1);
    // JOG FEEL
    double feelSensitivity(int deck) const; // x2 LIGHT .. x1 .. x0.5 HEAVY
    double bendWindowMs(int deck) const;    // how long a push lasts (smoothing, settling)
    double scratchFollow(int deck) const;   // how closely the record follows the hand
    double settleMs(int deck) const;        // let go: back to speed over this (HEAVY)
    void bend(int deck, double push);
    // Loops
    void loopInPressed(int deck);
    void loopOutPressed(int deck);
    void reloopPressed(int deck);
    void beatLoopPressed(int deck, int beats);
    /// The deck's tempo on its range's step (±6: 0.02 %, ±10 / ±16: 0.05 %,
    /// WIDE: 0.5 %), the fader moved there.
    void snapTempo(int deck);
    /// The playing loop half (0.5) or double (2) as long, from its start.
    void resizeLoop(const QString& group, double factor);
    void applyDefaults();
    void adjustLoop(int deck, double degrees);
    void updateLights();
    void bendStep(int deck);
    void stopBend(int deck);
    void rampTick(int deck);
    void finishRamp(int deck);
    double rampMs(int deck) const;   // a whole start or stop (0: instant)
    double fullSpeed(int deck) const; // as the engine plays it: tempo, direction
    bool vinylMode(int deck) const;
    void trackSearch(int deck, int direction);
    void callCue(int deck, int direction);

    int m_numDecks;
    QElapsedTimer m_clock;
    QTimer m_lights;
    std::vector<std::unique_ptr<Deck>> m_decks;
    bool m_eatRotaryRelease = false;
    std::unique_ptr<ControlObject> m_pDefaults; // CDJ defaults applied (version)
};

} // namespace mixxx::b2b
