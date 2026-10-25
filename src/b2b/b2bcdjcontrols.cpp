#include "b2b/b2bcdjcontrols.h"

#include "b2b/b2bexplorer.h"
#include "b2b/b2btemporange.h"

#include <QApplication>
#include <QDebug>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "mixer/playerinfo.h"
#include "track/track.h"

namespace mixxx::b2b {

namespace {
const QString kGroup = QStringLiteral("[B2BBooth]");

ConfigKey deckKey(int deck, const char* what) {
    return ConfigKey(kGroup, QStringLiteral("deck%1_%2").arg(deck).arg(QLatin1String(what)));
}

QString channel(int deck) {
    return QStringLiteral("[Channel%1]").arg(deck);
}

double get(const QString& group, const char* item) {
    return ControlObject::get(ConfigKey(group, QLatin1String(item)));
}

void set(const QString& group, const char* item, double value) {
    const ConfigKey key(group, QLatin1String(item));
    if (ControlObject::exists(key)) {
        ControlObject::set(key, value);
    }
}

// A push button pressed and let go.
void press(const QString& group, const char* item) {
    set(group, item, 1.0);
    set(group, item, 0.0);
}

// VINYL SPEED ADJUST at its slowest: this long to start or stop (a whole
// stop; the knob's curve keeps the low end tight for scratching).
constexpr double kSlowestRampMs = 3500;
constexpr double kInstantMs = 25; // shorter than this: no ramp

} // namespace

CdjControls::CdjControls(int decks, QObject* pParent)
        : QObject(pParent),
          m_numDecks(decks) {
    m_clock.start();
    for (int i = 0; i < decks; ++i) {
        const int n = i + 1;
        auto d = std::make_unique<Deck>();
        d->play = std::make_unique<ControlPushButton>(deckKey(n, "play"));
        d->vinylSpeed = std::make_unique<ControlPotmeter>(
                deckKey(n, "vinyl_speed"), 0.0, 1.0, false, true, false, true, 0.0);
        d->reverse = std::make_unique<ControlPushButton>(deckKey(n, "reverse"));
        d->reverse->setButtonMode(ControlPushButton::TOGGLE);
        d->trackPrev = std::make_unique<ControlPushButton>(deckKey(n, "track_prev"));
        d->trackNext = std::make_unique<ControlPushButton>(deckKey(n, "track_next"));
        d->callPrev = std::make_unique<ControlPushButton>(deckKey(n, "call_prev"));
        d->callNext = std::make_unique<ControlPushButton>(deckKey(n, "call_next"));
        d->jogCdj = std::make_unique<ControlPushButton>(deckKey(n, "jog_cdj"), true, 0.0);
        d->jogCdj->setButtonMode(ControlPushButton::TOGGLE);
        d->jogFeel = std::make_unique<ControlPotmeter>(
                deckKey(n, "jog_feel"), 0.0, 1.0, false, true, false, true, 0.5);
        d->loopIn = std::make_unique<ControlPushButton>(deckKey(n, "loop_in"));
        d->loopOut = std::make_unique<ControlPushButton>(deckKey(n, "loop_out"));
        d->reloop = std::make_unique<ControlPushButton>(deckKey(n, "reloop"));
        d->loop4 = std::make_unique<ControlPushButton>(deckKey(n, "loop4"));
        d->loop8 = std::make_unique<ControlPushButton>(deckKey(n, "loop8"));
        d->lightIn = std::make_unique<ControlObject>(deckKey(n, "loop_in_light"));
        d->lightOut = std::make_unique<ControlObject>(deckKey(n, "loop_out_light"));
        d->lightReloop = std::make_unique<ControlObject>(deckKey(n, "reloop_light"));
        d->light4 = std::make_unique<ControlObject>(deckKey(n, "loop4_light"));
        d->light8 = std::make_unique<ControlObject>(deckKey(n, "loop8_light"));
        d->loopBeats = std::make_unique<ControlObject>(deckKey(n, "loop_beats"));
        d->bendTick.setInterval(10);
        connect(&d->bendTick, &QTimer::timeout, this, [this, i] { bendStep(i); });

        auto onPress = [this](ControlPushButton* pButton, auto fn) {
            connect(pButton, &ControlObject::valueChanged, this, [fn](double v) {
                if (v > 0) {
                    fn();
                }
            });
        };
        onPress(d->play.get(), [this, i] { playPressed(i); });
        onPress(d->trackPrev.get(), [this, i] { trackSearch(i, -1); });
        onPress(d->trackNext.get(), [this, i] { trackSearch(i, 1); });
        onPress(d->callPrev.get(), [this, i] { callCue(i, -1); });
        onPress(d->callNext.get(), [this, i] { callCue(i, 1); });
        onPress(d->loopIn.get(), [this, i] { loopInPressed(i); });
        onPress(d->loopOut.get(), [this, i] { loopOutPressed(i); });
        onPress(d->reloop.get(), [this, i] { reloopPressed(i); });
        onPress(d->loop4.get(), [this, i] { beatLoopPressed(i, 4); });
        onPress(d->loop8.get(), [this, i] { beatLoopPressed(i, 8); });
        connect(d->reverse.get(), &ControlObject::valueChanged, this, [n](double v) {
            set(channel(n), "reverse", v > 0 ? 1.0 : 0.0);
        });
        d->scratchStill.setSingleShot(true);
        d->scratchStill.setInterval(60);
        connect(&d->scratchStill, &QTimer::timeout, this, [this, i, n] {
            if (m_decks[i]->scratching) {
                m_decks[i]->scratchSpeed = 0;
                set(channel(n), "scratch2", 0.0);
            }
        });
        d->ramp.setInterval(10);
        connect(&d->ramp, &QTimer::timeout, this, [this, i] { rampTick(i); });
        m_decks.push_back(std::move(d));
    }

    // Settings Mixxx remembers from before B2B (a Mixxx used with another
    // skin): set once to how a CDJ comes, after the decks are made.
    m_pDefaults = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("cdj_defaults")), true, false, true, 0.0);
    QTimer::singleShot(0, this, &CdjControls::applyDefaults);

    m_lights.setInterval(25);
    connect(&m_lights, &QTimer::timeout, this, &CdjControls::updateLights);
    m_lights.start();
    if (qApp) {
        qApp->installEventFilter(this); // the jogs and the browse knobs
    }
    if (qEnvironmentVariableIsSet("B2B_SPEED_LOG")) {
        // Testing: each deck's speed, 20 times a second.
        auto* pLog = new QTimer(this);
        connect(pLog, &QTimer::timeout, this, [this] {
            for (int i = 0; i < m_numDecks; ++i) {
                const QString group = channel(i + 1);
                if (get(group, "track_loaded") <= 0) {
                    continue;
                }
                qInfo().noquote() << QStringLiteral("B2B speed: deck %1 play %2 scratch %3 %4 pos %5 vinyl %6")
                                             .arg(i + 1)
                                             .arg(get(group, "play"))
                                             .arg(get(group, "scratch2_enable"))
                                             .arg(get(group, "scratch2"), 0, 'f', 3)
                                             .arg(get(group, "playposition"), 0, 'f', 5)
                                             .arg(vinylMode(i) ? m_decks[i]->vinylSpeed->get() : -1.0, 0, 'f', 2);
            }
        });
        pLog->start(50);
        // And every loop point: how far it is from the nearest beat.
        auto* pLoops = new QTimer(this);
        connect(pLoops, &QTimer::timeout, this, [this] {
            static double last[4][3] = {};
            for (int i = 0; i < std::min(m_numDecks, 4); ++i) {
                const QString group = channel(i + 1);
                const double start = get(group, "loop_start_position");
                const double end = get(group, "loop_end_position");
                const double on = get(group, "loop_enabled");
                if (start == last[i][0] && end == last[i][1] && on == last[i][2]) {
                    continue;
                }
                last[i][0] = start;
                last[i][1] = end;
                last[i][2] = on;
                const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
                const auto pBeats = pTrack ? pTrack->getBeats() : mixxx::BeatsPointer();
                const double rate = get(group, "track_samplerate");
                auto off = [&](double samples) -> QString {
                    if (samples < 0 || !pBeats || rate <= 0) {
                        return QStringLiteral("-");
                    }
                    const auto pos = mixxx::audio::FramePos(samples / 2.0);
                    const auto beat = pBeats->findClosestBeat(pos);
                    return QString::number((pos - beat) / rate * 1000.0, 'f', 1) + QStringLiteral("ms");
                };
                double beats = -1;
                if (start >= 0 && end >= 0 && pBeats) {
                    const auto a = mixxx::audio::FramePos(start / 2.0);
                    const auto b = mixxx::audio::FramePos(end / 2.0);
                    const auto next = pBeats->findNextBeat(a);
                    const auto after = pBeats->findNthBeat(a, 2);
                    if (next.isValid() && after.isValid() && after > next) {
                        beats = (b - a) / (after - next);
                    }
                }
                qInfo().noquote() << QStringLiteral("B2B loop: deck %1 quantize %2 on %3 start %4 end %5 beats %6 size %7")
                                             .arg(i + 1)
                                             .arg(get(group, "quantize"))
                                             .arg(on)
                                             .arg(off(start), off(end))
                                             .arg(beats, 0, 'f', 3)
                                             .arg(get(group, "beatloop_size"));
            }
        });
        pLoops->start(20);
    }
}

CdjControls::~CdjControls() {
    if (qApp) {
        qApp->removeEventFilter(this);
    }
}

// ------------------------------------------------------------------ players --

// ---------------------------------------------------------------- JOG FEEL --

namespace {
double feel(const ControlPotmeter* pKnob) {
    return std::clamp(pKnob->get(), 0.0, 1.0);
}
double audioBufferMs() {
    const double ms = ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")));
    return ms > 1 ? ms : 23.0;
}
// Mixxx averages jog pushes over this many audio buffers, at this strength.
constexpr int kMixxxJogBuffers = 25;
constexpr double kMixxxJogSensitivity = 0.1;
} // namespace

double CdjControls::feelSensitivity(int i) const {
    return std::pow(2.0, (0.5 - feel(m_decks[i]->jogFeel.get())) * 2.0);
}

double CdjControls::bendWindowMs(int i) const {
    return kMixxxJogBuffers * audioBufferMs() * std::pow(2.0, (feel(m_decks[i]->jogFeel.get()) - 0.5) * 2.0);
}

double CdjControls::scratchFollow(int i) const {
    // 0.5 = as before; LIGHT follows the hand at once, HEAVY drags.
    const double f = feel(m_decks[i]->jogFeel.get());
    return f < 0.5 ? 0.5 + (0.5 - f) * 0.7 : 0.5 - (f - 0.5) * 0.6;
}

double CdjControls::settleMs(int i) const {
    const double f = feel(m_decks[i]->jogFeel.get());
    return f > 0.5 ? (f - 0.5) * 2.0 * 300.0 : 0.0;
}

void CdjControls::bend(int i, double push) {
    Deck& d = *m_decks[i];
    if (d.ramp.isActive() || d.scratching) {
        return;
    }
    d.bendPushes.append({m_clock.elapsed(), push * feelSensitivity(i)});
    if (!d.bendTick.isActive()) {
        d.bendTick.start();
    }
    bendStep(i);
}

void CdjControls::bendStep(int i) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    const qint64 now = m_clock.elapsed();
    const double window = bendWindowMs(i);
    while (!d.bendPushes.isEmpty() && now - d.bendPushes.first().first > window) {
        d.bendPushes.removeFirst();
    }
    if (d.bendPushes.isEmpty() || get(group, "play") <= 0) {
        stopBend(i);
        return;
    }
    double sum = 0;
    for (const auto& push : std::as_const(d.bendPushes)) {
        sum += push.second;
    }
    // Each push moves the track the same distance as Mixxx's jog would,
    // spread over the window: a longer window is smoother, not further.
    const double offset = kMixxxJogSensitivity * audioBufferMs() / window * sum;
    if (!d.bendActive) {
        d.bendActive = true;
        // The deck's speed, without counting as scratching: keylock stays on
        // and the sound as it is.
        set(group, "scratch2_indicates_scratching", 0.0);
        set(group, "scratch2", fullSpeed(i) + offset);
        set(group, "scratch2_enable", 1.0);
    } else {
        set(group, "scratch2", fullSpeed(i) + offset);
    }
}

void CdjControls::stopBend(int i) {
    Deck& d = *m_decks[i];
    d.bendTick.stop();
    d.bendPushes.clear();
    if (d.bendActive) {
        d.bendActive = false;
        const QString group = channel(i + 1);
        set(group, "scratch2_enable", 0.0);
        set(group, "scratch2", 0.0);
        set(group, "scratch2_indicates_scratching", 1.0);
    }
}

bool CdjControls::vinylMode(int i) const {
    return m_decks[i]->jogCdj->get() <= 0;
}

double CdjControls::rampMs(int i) const {
    if (!vinylMode(i)) {
        return 0; // CDJ mode: PLAY/PAUSE at once, whatever the knob
    }
    const double v = std::clamp(m_decks[i]->vinylSpeed->get(), 0.0, 1.0);
    const double ms = v * v * kSlowestRampMs;
    return ms < kInstantMs ? 0 : ms;
}

double CdjControls::fullSpeed(int i) const {
    const QString group = channel(i + 1);
    double full = get(group, "rate_ratio");
    if (full <= 0) {
        full = 1.0;
    }
    return get(group, "reverse") > 0 ? -full : full;
}

void CdjControls::playPressed(int i) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    stopBend(i);
    if (get(group, "track_loaded") <= 0) {
        set(group, "play", get(group, "play") > 0 ? 0.0 : 1.0);
        return;
    }
    if (d.scratching) {
        // Hand on the record: PLAY/PAUSE only decides what happens when it's
        // let go (spin up, or stay stopped).
        if (d.ramp.isActive()) {
            d.ramp.stop();
        }
        set(group, "play", get(group, "play") > 0 ? 0.0 : 1.0);
        return;
    }
    if (d.ramp.isActive()) {
        // Pressed again while starting or stopping: the other way, from the
        // speed it's at now (no jump).
        if (d.rampStops || d.rampTo == 0.0) {
            set(group, "play", 1.0);
            startRamp(i, fullSpeed(i), false, false);
        } else {
            startRamp(i, 0.0, true, false);
        }
        return;
    }
    const bool playing = get(group, "play") > 0;
    // Previewing from the cue point (CUE held): Mixxx takes "pause" as "keep
    // playing when CUE is let go", so no ramp then.
    const bool previewing = get(group, "cue_preview") > 0;
    if (rampMs(i) <= 0 || previewing) {
        set(group, "play", playing ? 0.0 : 1.0);
        return;
    }
    if (playing) {
        set(group, "scratch2", fullSpeed(i));
        set(group, "scratch2_enable", 1.0);
        startRamp(i, 0.0, true, false);
    } else {
        set(group, "scratch2", 0.0);
        set(group, "scratch2_enable", 1.0);
        set(group, "play", 1.0); // lit at once, like the CDJ
        startRamp(i, fullSpeed(i), false, false);
    }
}

void CdjControls::startRamp(int i, double to, bool stops, bool hold, double wholeMs) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    stopBend(i);
    const double from = get(group, "scratch2_enable") > 0 ? get(group, "scratch2") : (get(group, "play") > 0 ? fullSpeed(i) : 0.0);
    const double full = std::max(0.01, std::fabs(fullSpeed(i)));
    // A part of a whole start / stop takes its part of the time.
    const double ms = (wholeMs >= 0 ? wholeMs : rampMs(i)) * std::min(1.0, std::fabs(to - from) / full);
    d.rampFrom = from;
    d.rampTo = to;
    d.rampStops = stops;
    d.rampHold = hold;
    d.rampMs = ms;
    set(group, "scratch2", from);
    set(group, "scratch2_enable", 1.0);
    if (ms < kInstantMs) {
        set(group, "scratch2", to);
        finishRamp(i);
        return;
    }
    d.rampClock.start();
    d.ramp.start();
}

void CdjControls::rampTick(int i) {
    Deck& d = *m_decks[i];
    const double t = std::min(1.0, d.rampClock.elapsed() / d.rampMs);
    set(channel(i + 1), "scratch2", d.rampFrom + (d.rampTo - d.rampFrom) * t);
    if (t >= 1.0) {
        finishRamp(i);
    }
}

void CdjControls::finishRamp(int i) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    d.ramp.stop();
    if (d.rampHold) {
        set(group, "scratch2", 0.0); // the hand holds the record still
        return;
    }
    if (d.rampStops) {
        set(group, "play", 0.0);
    }
    set(group, "scratch2_enable", 0.0);
    set(group, "scratch2", 0.0);
}

// ------------------------------------------------------------------- loops --

namespace {
constexpr double kAdjustDegreesPerBeat = 24; // QUANTIZE: this much jog, a beat
constexpr double kAdjustMsPerDegree = 2;     // no QUANTIZE: fine
constexpr qint64 kAdjustTimeoutMs = 10000;   // an adjust nobody touches ends
constexpr qint64 kFastFlashMs = 90;

mixxx::BeatsPointer beatsOf(const QString& group) {
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
    return pTrack ? pTrack->getBeats() : mixxx::BeatsPointer();
}

// The loop sizes Mixxx has (LoopingControl's s_dBeatSizes): 1/32 .. 512.
constexpr double kLeastLoopBeats = 1.0 / 32.0;
constexpr double kMostLoopBeats = 512.0;
constexpr double kSameLength = 0.02; // a loop this close to a size (in parts of it) is that size

/// The deck's loop (set, whether playing or not) in beats, exactly (a 4.3
/// beat loop is 4.3); -1 with no loop or no beat grid.
double loopBeats(const QString& group) {
    const double startSamples = get(group, "loop_start_position");
    const double endSamples = get(group, "loop_end_position");
    if (startSamples < 0 || endSamples <= startSamples) {
        return -1;
    }
    const mixxx::BeatsPointer pBeats = beatsOf(group);
    if (!pBeats) {
        return -1;
    }
    const auto start = mixxx::audio::FramePos(startSamples / 2.0);
    const auto end = mixxx::audio::FramePos(endSamples / 2.0);
    const auto prev = pBeats->findPrevBeat(start + 1.0);
    const auto next = pBeats->findNextBeat(start + 1.0);
    const double beat = prev.isValid() && next.isValid() ? next - prev : 0.0;
    return beat > 0 ? (end - start) / beat : -1;
}

/// The loop size (1/32 .. 512, a power of two) this length is, or 0.
double loopSizeOf(double beats) {
    for (double size = kLeastLoopBeats; size <= kMostLoopBeats; size *= 2.0) {
        if (std::fabs(beats - size) <= size * kSameLength) {
            return size;
        }
    }
    return 0;
}
} // namespace

void CdjControls::loopInPressed(int i) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    if (d.adjust == 1) {
        d.adjust = 0; // done adjusting
    } else if (get(group, "loop_enabled") > 0) {
        d.adjust = 1; // IN ADJUST: the jog moves the start
        d.adjustTurn = 0;
        d.adjustIdle.start();
    } else {
        // A new loop's start, where it is now (Mixxx puts it on the nearest
        // beat with QUANTIZE on).
        d.adjust = 0;
        press(group, "loop_in");
    }
    updateLights();
}

void CdjControls::loopOutPressed(int i) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    if (d.adjust == 2) {
        d.adjust = 0;
    } else if (get(group, "loop_enabled") > 0) {
        d.adjust = 2; // OUT ADJUST: the jog moves the end
        d.adjustTurn = 0;
        d.adjustIdle.start();
    } else {
        d.adjust = 0;
        press(group, "loop_out"); // the end (nearest beat with QUANTIZE): loops
    }
    updateLights();
}

void CdjControls::reloopPressed(int i) {
    m_decks[i]->adjust = 0;
    press(channel(i + 1), "reloop_toggle");
    updateLights();
}

void CdjControls::beatLoopPressed(int i, int beats) {
    m_decks[i]->adjust = 0;
    const QString group = channel(i + 1);
    if (get(group, "loop_enabled") > 0) {
        // Looping (however it started): 4 BEAT halves it, 8 BEAT doubles
        // it, from the same start.
        resizeLoop(group, beats == 4 ? 0.5 : 2.0);
    } else {
        // Forwards from here, always: Mixxx's loop anchor (a setting some
        // skins have, remembered) would put the loop before the press.
        set(group, "loop_anchor", 0.0);
        const QByteArray activate = QStringLiteral("beatloop_%1_activate").arg(beats).toLatin1();
        press(group, activate.constData()); // on the nearest beat with QUANTIZE
    }
    updateLights();
}

void CdjControls::resizeLoop(const QString& group, double factor) {
    const double length = loopBeats(group);
    if (length > 0) {
        const double target = length * factor;
        // Mixxx's smallest and largest loops: no further.
        if (target < kLeastLoopBeats * (1.0 - kSameLength) ||
                target > kMostLoopBeats * (1.0 + kSameLength)) {
            return;
        }
        if (const double size = loopSizeOf(target)) {
            // A loop of whole beats (or halves, quarters...): Mixxx's own
            // loop size, kept on the grid from the same start; if the
            // play position is past the new end it carries on in time
            // inside the loop. (Past the track's end: Mixxx refuses it.)
            set(group, "beatloop_size", size);
            return;
        }
    }
    // Any other length (a loop set by hand, or no beat grid): exactly half
    // or double, from the same start.
    set(group, "loop_scale", factor);
}

void CdjControls::applyDefaults() {
    if (!ControlObject::exists(ConfigKey(channel(1), QStringLiteral("quantize")))) {
        QTimer::singleShot(500, this, &CdjControls::applyDefaults); // decks not made yet
        return;
    }
    const bool first = m_pDefaults->get() < 1;
    for (int i = 0; i < m_numDecks; ++i) {
        const QString group = channel(i + 1);
        // The tempo fader moves in the range's steps while it's held (a
        // tempo set any other way, by SYNC or a typed BPM, is left exact).
        Deck& d = *m_decks[i];
        if (!d.rate && ControlObject::exists(ConfigKey(group, QStringLiteral("rate")))) {
            d.rate = std::make_unique<ControlProxy>(group, QStringLiteral("rate"), this);
            d.rate->connectValueChanged(this, [this, i](double) {
                if (m_decks[i]->faderHeld) {
                    snapTempo(i);
                }
            });
        }
        set(group, "loop_anchor", 0.0); // loops go forwards (no key for it)
        if (first) {
            set(group, "quantize", 1.0); // on, as a CDJ comes; the key still turns it off
        }
    }
    if (first) {
        qInfo() << "B2B: CDJ defaults applied (QUANTIZE on, loops forwards)";
        m_pDefaults->set(1);
    }
}

void CdjControls::snapTempo(int i) {
    const QString group = channel(i + 1);
    const double span = get(group, "rateRange");
    const double dir = get(group, "rate_dir") < 0 ? -1.0 : 1.0;
    if (span <= 0) {
        return;
    }
    const double pct = get(group, "rate") * span * dir * 100.0;
    const double snapped = tempo::snapPct(pct, span);
    const double fader = std::clamp(snapped / (span * dir * 100.0), -1.0, 1.0);
    if (std::fabs(fader - get(group, "rate")) > 1e-9) {
        set(group, "rate", fader);
    }
}

void CdjControls::adjustLoop(int i, double degrees) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    const bool in = d.adjust == 1;
    const double start = get(group, "loop_start_position");
    const double end = get(group, "loop_end_position");
    if (start < 0 || end < 0) {
        return;
    }
    d.adjustIdle.start();
    const double rate = get(group, "track_samplerate");
    const mixxx::BeatsPointer pBeats = beatsOf(group);
    auto point = mixxx::audio::FramePos((in ? start : end) / 2.0); // samples are stereo
    if (get(group, "quantize") > 0 && pBeats) {
        // QUANTIZE: a beat at a time, always on the grid.
        d.adjustTurn += degrees;
        int steps = static_cast<int>(d.adjustTurn / kAdjustDegreesPerBeat);
        if (steps == 0) {
            return;
        }
        d.adjustTurn -= steps * kAdjustDegreesPerBeat;
        for (; steps > 0; --steps) {
            const auto next = pBeats->findNextBeat(point + 1.0);
            if (next.isValid()) {
                point = next;
            }
        }
        for (; steps < 0; ++steps) {
            const auto prev = pBeats->findPrevBeat(point - 1.0);
            if (prev.isValid()) {
                point = prev;
            }
        }
    } else if (rate > 0) {
        point = point + degrees * kAdjustMsPerDegree / 1000.0 * rate;
    } else {
        return;
    }
    // Never past the other end (a loop at least 10 ms long).
    const double minFrames = rate > 0 ? rate * 0.01 : 441;
    const double samples = point.value() * 2.0;
    if (in) {
        if (point.value() < 0 || samples > end - 2 * minFrames) {
            return;
        }
        set(group, "loop_start_position", samples);
    } else {
        const double length = get(group, "track_samples");
        if (samples < start + 2 * minFrames || (length > 0 && samples > length)) {
            return;
        }
        set(group, "loop_end_position", samples);
    }
}

void CdjControls::updateLights() {
    const qint64 now = m_clock.elapsed();
    const bool fastOn = (now / kFastFlashMs) % 2 == 0;
    for (int i = 0; i < m_numDecks; ++i) {
        Deck& d = *m_decks[i];
        const QString group = channel(i + 1);
        const bool loaded = get(group, "track_loaded") > 0;
        const double samples = get(group, "track_samples");
        if (samples != d.loadedSamples) {
            d.loadedSamples = samples; // a new track: nothing being adjusted
            d.adjust = 0;
        }
        const bool hasStart = loaded && get(group, "loop_start_position") >= 0;
        const bool hasEnd = loaded && get(group, "loop_end_position") >= 0;
        const bool looping = loaded && get(group, "loop_enabled") > 0;
        if (d.adjust && (!looping || d.adjustIdle.elapsed() > kAdjustTimeoutMs)) {
            d.adjust = 0; // the loop was exited, or nobody touched the jog
        }
        // On the beat while looping: lit for the first half of each beat.
        const bool beatOn = get(group, "beat_distance") < 0.5;
        double in = 0;
        double out = 0;
        double reloop = 0;
        if (d.adjust == 1) {
            in = fastOn;
            reloop = 1;
        } else if (d.adjust == 2) {
            out = fastOn;
            reloop = 1;
        } else if (looping) {
            in = beatOn;
            out = beatOn;
            reloop = 1;
        } else if (hasStart && !hasEnd) {
            in = 1; // the start is set: OUT will close the loop
        } else if (hasStart && hasEnd) {
            reloop = 1; // a loop to go back into
        }
        // 4 / 8 BEAT: lit while a loop that long plays (worked out from the
        // loop itself, so a friend's loop lights them here too).
        double four = 0;
        double eight = 0;
        // The loop's length on the deck's screen (while it plays).
        const double length = looping && hasEnd ? loopBeats(group) : -1;
        const double shown = length > 0 ? length : (looping && hasEnd ? -1 : 0);
        if (d.loopBeats->get() != shown) {
            d.loopBeats->set(shown);
        }
        if (looping && hasEnd && d.adjust == 0 && length > 0) {
            // Exactly (a 4.3 beat loop isn't a 4).
            four = std::fabs(length - 4.0) < 0.02 ? 1 : 0;
            eight = std::fabs(length - 8.0) < 0.02 ? 1 : 0;
        }
        if (d.light4->get() != four) {
            d.light4->set(four);
        }
        if (d.light8->get() != eight) {
            d.light8->set(eight);
        }
        if (d.lightIn->get() != in) {
            d.lightIn->set(in);
        }
        if (d.lightOut->get() != out) {
            d.lightOut->set(out);
        }
        if (d.lightReloop->get() != reloop) {
            d.lightReloop->set(reloop);
        }
    }
}

void CdjControls::trackSearch(int i, int direction) {
    const QString group = channel(i + 1);
    const QString library = QStringLiteral("[Library]");
    if (direction < 0) {
        const double samples = get(group, "track_samples");
        const double rate = get(group, "track_samplerate");
        const double seconds = rate > 0 ? get(group, "playposition") * samples / rate / 2.0 : 0.0;
        if (seconds > 2.0) {
            set(group, "playposition", 0.0); // back to the start of this track
            return;
        }
        press(library, "MoveUp");
    } else {
        press(library, "MoveDown");
    }
    // The list moves its selection a moment later: load after that.
    QTimer::singleShot(150, this, [group] { press(group, "LoadSelectedTrack"); });
}

void CdjControls::callCue(int i, int direction) {
    const QString group = channel(i + 1);
    const double samples = get(group, "track_samples");
    if (samples <= 0) {
        return;
    }
    const double here = get(group, "playposition") * samples;
    const double margin = std::max(1.0, get(group, "track_samplerate")) * 2.0 * 0.2; // 0.2 s
    QList<double> cues;
    for (int h = 1; h <= 8; ++h) {
        const double pos = ControlObject::get(ConfigKey(group, QStringLiteral("hotcue_%1_position").arg(h)));
        if (pos >= 0) {
            cues.append(pos);
        }
    }
    const double cuePoint = get(group, "cue_point");
    if (cuePoint >= 0) {
        cues.append(cuePoint);
    }
    double best = -1;
    for (double pos : cues) {
        if (direction < 0 && pos < here - margin && (best < 0 || pos > best)) {
            best = pos;
        } else if (direction > 0 && pos > here + margin && (best < 0 || pos < best)) {
            best = pos;
        }
    }
    if (best < 0) {
        return;
    }
    set(group, "playposition", best / samples);
    if (get(group, "play") <= 0) {
        // Paused: the called cue becomes the cue point, as on the CDJ.
        QTimer::singleShot(50, this, [group] { press(group, "cue_set"); });
    }
}

bool CdjControls::eventFilter(QObject* pObject, QEvent* pEvent) {
    const QEvent::Type type = pEvent->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove &&
            type != QEvent::MouseButtonRelease && type != QEvent::MouseButtonDblClick &&
            type != QEvent::Wheel) {
        return false;
    }
    const QString name = pObject->objectName();
    if (name.startsWith(QLatin1String("Jog")) && (name.size() == 4 || name.size() == 7)) {
        const bool top = name.size() == 7; // "JogTopN": the display in the middle
        if (!top || name.startsWith(QLatin1String("JogTop"))) {
            const int deck = name.at(name.size() - 1).digitValue() - 1;
            if (deck >= 0 && deck < m_numDecks && pObject->isWidgetType()) {
                return jogEvent(deck, static_cast<QWidget*>(pObject), pEvent, top);
            }
        }
    }
    if (name.size() == 6 && name.startsWith(QLatin1String("Tempo")) &&
            (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease ||
                    type == QEvent::MouseButtonDblClick)) {
        // The tempo fader held: its moves go in the range's steps.
        const int deck = name.at(5).digitValue() - 1;
        if (deck >= 0 && deck < m_numDecks) {
            m_decks[deck]->faderHeld = type != QEvent::MouseButtonRelease;
            // (Let go, or a double-click's reset: on a step after Mixxx's
            // own handling of it.)
            QTimer::singleShot(0, this, [this, deck] { snapTempo(deck); });
        }
        return false;
    }
    if (type == QEvent::Wheel && name.size() == 6 && name.startsWith(QLatin1String("Tempo"))) {
        // The tempo slider: 0.1 % a notch (WIDE: its 0.5 % step), on the
        // range's steps (Mixxx moves a slider 1/127 of its travel).
        const int deck = name.at(5).digitValue();
        const int dy = static_cast<QWheelEvent*>(pEvent)->angleDelta().y();
        if (deck >= 1 && deck <= 4 && dy != 0) {
            const QString group = channel(deck);
            const double range = get(group, "rateRange");
            if (range > 0) {
                const double notches = dy / 120.0;
                const double notchPct = std::max(0.1, tempo::kRanges[tempo::indexOf(range)].stepPct);
                const double rate = get(group, "rate") + notches * notchPct / 100.0 / range;
                set(group, "rate", std::clamp(rate, -1.0, 1.0));
                snapTempo(deck - 1);
            }
        }
        return true;
    }
    if (type == QEvent::Wheel && name.size() == 7 && name.startsWith(QLatin1String("Rotary"))) {
        // The browse knob: turn it to move through the list.
        const int dy = static_cast<QWheelEvent*>(pEvent)->angleDelta().y();
        if (dy != 0 && !LibraryExplorer::moveSelection(dy > 0 ? -1 : 1)) {
            set(QStringLiteral("[Library]"), "MoveVertical", dy > 0 ? -1.0 : 1.0);
        }
        return true;
    }
    if (name.size() == 7 && name.startsWith(QLatin1String("Rotary"))) {
        // Pressing the knob on a folder list opens the selected item (and
        // does not load a track).
        if ((type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) &&
                static_cast<QMouseEvent*>(pEvent)->button() == Qt::LeftButton &&
                LibraryExplorer::activateSelection()) {
            m_eatRotaryRelease = true;
            return true;
        }
        if (type == QEvent::MouseButtonRelease && m_eatRotaryRelease) {
            m_eatRotaryRelease = false;
            return true;
        }
    }
    return false;
}

bool CdjControls::jogEvent(int i, QWidget* pJog, QEvent* pEvent, bool top) {
    Deck& d = *m_decks[i];
    const QString group = channel(i + 1);
    if (pEvent->type() == QEvent::Wheel) {
        // The wheel turns the jog a little (a nudge, or a step when paused).
        const int dy = static_cast<QWheelEvent*>(pEvent)->angleDelta().y();
        if (d.adjust) {
            // A notch: a beat (QUANTIZE), or 10 ms.
            adjustLoop(i, (dy > 0 ? 1.0 : -1.0) * (get(group, "quantize") > 0 ? kAdjustDegreesPerBeat : 5.0));
            return true;
        }
        set(group, "jog", dy > 0 ? 2.0 : -2.0);
        return true;
    }
    auto* pMouse = static_cast<QMouseEvent*>(pEvent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF pos = pMouse->position();
#else
    const QPointF pos = pMouse->localPos();
#endif
    const double cx = pos.x() - pJog->width() / 2.0;
    const double cy = pos.y() - pJog->height() / 2.0;
    const double radius = std::min(pJog->width(), pJog->height()) / 2.0;
    const double angle = std::atan2(cx, -cy) * 180.0 / M_PI;
    switch (pEvent->type()) {
    case QEvent::MouseButtonPress: {
        if (pMouse->button() != Qt::LeftButton) {
            return false;
        }
        const double r = std::hypot(cx, cy);
        if (r > radius) {
            return false; // a corner of the square, not the jog
        }
        d.jogAngle = angle;
        if (d.adjust) {
            d.adjusting = true; // IN / OUT ADJUST: the jog moves the loop point
            return true;
        }
        const bool sideRing = !top && r > radius * 0.8;
        if (d.jogCdj->get() > 0 || sideRing) {
            d.jogging = true; // bend the pitch (or move, paused)
        } else {
            // Vinyl mode, hand on top: the record slows to a stop under it
            // (as quickly as VINYL SPEED ADJUST says).
            stopBend(i);
            d.scratching = true;
            d.scratchSpeed = 0;
            d.scratchClock.start();
            d.scratchStill.setInterval(static_cast<int>(60 * std::pow(1.6, (feel(d.jogFeel.get()) - 0.5) * 2.0)));
            if (get(group, "play") > 0 && !(d.ramp.isActive() && d.rampStops)) {
                startRamp(i, 0.0, false, true);
            } else {
                if (d.ramp.isActive()) {
                    d.ramp.stop();
                }
                set(group, "scratch2", 0.0);
                set(group, "scratch2_enable", 1.0);
            }
        }
        return true;
    }
    case QEvent::MouseMove: {
        if (!d.jogging && !d.scratching && !d.adjusting) {
            return false;
        }
        double delta = angle - d.jogAngle;
        if (delta > 180) {
            delta -= 360;
        } else if (delta < -180) {
            delta += 360;
        }
        d.jogAngle = angle;
        if (d.adjusting) {
            adjustLoop(i, delta);
            return true;
        }
        if (d.jogging) {
            if (get(group, "play") > 0) {
                bend(i, delta / 3.0); // a nudge, felt through JOG FEEL
            } else {
                set(group, "jog", delta / 3.0 * feelSensitivity(i)); // paused: through the track
            }
        } else {
            // Moving the record: the hand has it now (no more slowing down).
            if (d.ramp.isActive()) {
                d.ramp.stop();
            }
            // A turntable at 33 1/3 rpm turns 0.2 degrees a millisecond.
            const double ms = std::max<qint64>(1, d.scratchClock.restart());
            const double speed = delta / ms / 0.2 * feelSensitivity(i);
            const double follow = scratchFollow(i);
            d.scratchSpeed = (1.0 - follow) * d.scratchSpeed + follow * speed;
            set(group, "scratch2", d.scratchSpeed);
            d.scratchStill.start();
        }
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (d.adjusting) {
            d.adjusting = false;
            return true;
        }
        if (d.scratching) {
            d.scratching = false;
            d.scratchStill.stop();
            if (get(group, "play") > 0) {
                // Let go: it spins back up to speed (VINYL SPEED ADJUST), or
                // with no spin-up, settles as JOG FEEL says (HEAVY: gradually).
                startRamp(i, fullSpeed(i), false, false, rampMs(i) > 0 ? -1.0 : settleMs(i));
            } else {
                if (d.ramp.isActive()) {
                    d.ramp.stop();
                }
                set(group, "scratch2_enable", 0.0);
                set(group, "scratch2", 0.0);
            }
            return true;
        }
        if (d.jogging) {
            d.jogging = false;
            return true;
        }
        return false;
    case QEvent::MouseButtonDblClick:
        return true;
    default:
        return false;
    }
}

} // namespace mixxx::b2b

#include "moc_b2bcdjcontrols.cpp"
