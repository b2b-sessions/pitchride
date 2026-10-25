#include "b2b/b2bengineadapter.h"

#include <QString>

#include "b2b/b2bmixtiming.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "audio/frame.h"
#include "control/control.h"
#include "control/controlobject.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"

namespace mixxx::b2b {

namespace {

using ::b2b::ControlEvent;
using ::b2b::DeckSnapshot;
using ::b2b::FixedString;
using ::b2b::Nanos;

constexpr double kPi = 3.14159265358979323846;
constexpr Nanos kSnapshotInterval = 100 * ::b2b::kNanosPerMs;
// "A deck each", the mixer row: our own move on the friend's channel wins
// over what comes back for this long after it (the echo of it, or theirs at
// the same moment), on top of the round trip.
constexpr Nanos kMixTouchHold = 250 * ::b2b::kNanosPerMs;
// A change on the friend's channel within this long of applying one of
// their moves there is that move (not ours): at once.
constexpr int64_t kRemoteMixWindow = 40 * ::b2b::kNanosPerMs;
constexpr double kJumpThresholdSec = 0.010; // a position change this unexpected is a seek/loop/hotcue
constexpr double kPausedPositionToleranceSec = 0.050;
constexpr Nanos kReanchorThreshold = 50 * ::b2b::kNanosPerMs;
constexpr int kLogEveryCallbacks = 8;

// Catch-up after handing over: play at most this much slower (key lock on),
// easing in and out over the first and last eighth of the catch-up.
constexpr double kMaxCatchUpTrim = 0.06;
constexpr double kRampFraction = 1.0 / 8.0;
constexpr double kCatchUpThresholdSec = 0.015; // drift correction is looser while catching up
// Listening delay changes: follow increases quickly, decreases only when worth it.
constexpr double kRetargetUpNs = 5e6;
constexpr double kRetargetDownNs = 20e6;
constexpr double kJumpInsteadNs = 1.5e9; // shifts this big jump rather than slew
constexpr Nanos kBeatWaitTimeout = 2 * ::b2b::kNanosPerSec;
// The live side repeats its whole state this often, so the listener heals any
// difference (a refused deck prep, a message lost around a handover).
constexpr Nanos kRefreshInterval = 15 * ::b2b::kNanosPerSec; // ~900 controls: gentle on slow links
constexpr int kRefreshPerCallback = 4; // a trickle, not a burst (slow links)
constexpr Nanos kPrepGrace = 1500 * ::b2b::kNanosPerMs;
// A deck each: the friend's decks are steered onto theirs by a tiny change of
// speed (never more than this), and only jumped if far off. Our own decks
// report smaller jumps (a quantize snap) than when taking turns.
constexpr double kSteerMaxTrim = 0.006;
constexpr double kSteerSeconds = 0.8;    // how long a difference takes to close
constexpr double kSteerJumpSec = 0.015;
constexpr double kTogetherJumpSec = 0.003;
constexpr Nanos kSnapAfterStart = 500 * ::b2b::kNanosPerMs; // just after a start or jump...
constexpr double kSnapAfterStartSec = 0.002;               // ...put right anything over this

const FixedString<64> kHandoverKey("[B2B],handover");

// Effect knobs go to the audio engine as messages, a buffer later.
bool isEffectKey(std::string_view key) {
    return key.rfind("[EqualizerRack", 0) == 0 || key.rfind("[QuickEffectRack", 0) == 0 ||
            key.rfind("[EffectRack", 0) == 0;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

uint64_t parseHex(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<uint64_t>(c - 'A' + 10);
        } else {
            return 0;
        }
    }
    return v;
}

// Catch-up speed profile over progress p in [0, 1]: eases in, holds, eases
// out. velocity() peaks at 1; displacement() goes 0 -> 1.
double velocity(double p) {
    const double r = kRampFraction;
    if (p <= 0 || p >= 1) {
        return 0;
    }
    if (p < r) {
        return 0.5 * (1 - std::cos(kPi * p / r));
    }
    if (p > 1 - r) {
        return 0.5 * (1 - std::cos(kPi * (1 - p) / r));
    }
    return 1;
}

double displacement(double p) {
    const double r = kRampFraction;
    auto rampArea = [r](double q) { return 0.5 * q - (r / (2 * kPi)) * std::sin(kPi * q / r); };
    double a;
    if (p <= 0) {
        return 0;
    } else if (p >= 1) {
        return 1;
    } else if (p < r) {
        a = rampArea(p);
    } else if (p <= 1 - r) {
        a = 0.5 * r + (p - r);
    } else {
        a = (1 - r) - rampArea(1 - p);
    }
    return a / (1 - r);
}

} // namespace

EngineAdapter::EngineAdapter(::b2b::Session& session,
        const ::b2b::LocalClock& clock,
        ::b2b::EventLog* log,
        std::vector<DeckRefs> decks,
        std::vector<Watched> watched,
        ControlObject* engineSampleRate,
        ControlObject* outputLatencyMs,
        double correctThresholdMs,
        bool startLive,
        Nanos startOffset)
        : m_session(session),
          m_clock(clock),
          m_log(log),
          m_decks(std::move(decks)),
          m_watched(std::move(watched)),
          m_engineSampleRate(engineSampleRate),
          m_outputLatencyMs(outputLatencyMs),
          m_correctThresholdSec(correctThresholdMs / 1000.0),
          m_live(startLive),
          m_offset(static_cast<double>(startOffset)) {
    m_pending.reserve(1024);
    m_snapshots.reserve(512);
    m_liveShared = startLive;
    m_offsetShared = startOffset;
    for (auto& id : m_deckTrackId) {
        id.store(0);
    }
    for (auto& w : m_watched) {
        w.last = w.control->get();
    }
    m_playWatched.fill(-1);
    for (size_t i = 0; i < m_watched.size(); ++i) {
        if (m_watched[i].isPlay && m_watched[i].deck >= 0 && m_watched[i].deck < kMaxDecks) {
            m_playWatched[m_watched[i].deck] = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < m_watched.size(); ++i) {
        if (m_watched[i].key == std::string_view("[Master],crossfader")) {
            m_watched[i].isCrossfader = true;
            m_crossfaderWatched = static_cast<int>(i);
        }
    }
    for (size_t d = 0; d < m_decks.size() && d < kMaxDecks; ++d) {
        const QByteArray group = m_decks[d].group.toUtf8();
        m_groupKeys[d].assign(std::string_view(group.constData(), static_cast<size_t>(group.size())));
        m_posKeys[d].assign((m_groupKeys[d].str() + ",b2b_pos"));
    }
}

Nanos EngineAdapter::bufferLocalTime(int frames, double sampleRate) {
    // The sound card plays samples at a steady rate, so count samples for a
    // smooth timeline and only nudge it gently towards the system clock.
    // Callback timing jitter therefore never reaches the music.
    const double latencyMs = m_outputLatencyMs ? m_outputLatencyMs->get() : 0.0;
    const Nanos measured = m_clock.now() + ::b2b::fromMs(latencyMs);
    if (!m_timingStarted || sampleRate != m_lastSampleRate) {
        m_anchor = measured;
        m_framesRendered = 0;
        m_lastSampleRate = sampleRate;
        m_timingStarted = true;
    }
    Nanos local = m_anchor + static_cast<Nanos>(static_cast<double>(m_framesRendered) * 1e9 / sampleRate);
    if (std::llabs(measured - local) > kReanchorThreshold) {
        m_anchor = measured - static_cast<Nanos>(static_cast<double>(m_framesRendered) * 1e9 / sampleRate);
    } else {
        m_anchor += (measured - local) / 64;
    }
    local = m_anchor + static_cast<Nanos>(static_cast<double>(m_framesRendered) * 1e9 / sampleRate);
    m_framesRendered += frames;
    return local;
}

void EngineAdapter::process(int frames) {
    ++m_callbacks;
    const double sampleRate = m_engineSampleRate ? m_engineSampleRate->get() : 0.0;
    if (sampleRate <= 0 || frames <= 0) {
        return;
    }
    const Nanos local = bufferLocalTime(frames, sampleRate);
    if (!m_session.ready()) {
        return;
    }
    const Nanos trueStart = m_clock.toTrue(local);
    const double bufferSec = frames / sampleRate;
    const Nanos shared = m_session.sharedFromLocal(local);

    const int requestedMode = m_requestedMode.exchange(-1, std::memory_order_acq_rel);
    if (requestedMode >= 0 && static_cast<Mode>(requestedMode) != m_mode) {
        switchMode(static_cast<Mode>(requestedMode), shared - static_cast<Nanos>(m_offset), local, trueStart);
    }
    if (m_mode == Mode::DeckEach) {
        m_resetLive.store(-1, std::memory_order_release); // roles don't apply
        processTogether(shared - static_cast<Nanos>(m_offset), trueStart, bufferSec);
        m_offsetShared.store(static_cast<Nanos>(m_offset), std::memory_order_release);
        if (m_log && m_callbacks % kLogEveryCallbacks == 0) {
            logPositions(trueStart);
        }
        return;
    }

    // The peer reconnected: roles start afresh (the host is live).
    const int reset = m_resetLive.exchange(-1, std::memory_order_acq_rel);
    if (reset == 1 && !m_live) {
        becomeLive(shared - static_cast<Nanos>(m_offset), trueStart);
    } else if (reset == 0 && m_live) {
        becomeListener(shared - static_cast<Nanos>(m_offset), local, trueStart);
    }

    if (m_slew.active) {
        stepCatchUp(local);
    }
    Nanos contentStart = shared - static_cast<Nanos>(m_offset);

    if (m_live) {
        if (m_handoverRequested.exchange(false, std::memory_order_acq_rel)) {
            const bool snap = m_style.load(std::memory_order_acquire) == static_cast<int>(HandoverStyle::Snap);
            if (snap && referenceDeck() >= 0) {
                m_waitingForBeat = true; // hand over exactly on the next beat
                m_beatWaitStart = local;
                m_lastBeatDistance = -1;
            } else {
                sendEvent(contentStart, kHandoverKey, ++m_epoch, nullptr, trueStart);
                becomeListener(contentStart, local, trueStart);
            }
        }
        if (m_waitingForBeat) {
            const int ref = referenceDeck();
            const double bd = ref >= 0 && m_decks[ref].beatDistance ? m_decks[ref].beatDistance->get() : 0.0;
            const bool onBeat = ref < 0 || (m_lastBeatDistance >= 0 && bd < m_lastBeatDistance - 0.5);
            m_lastBeatDistance = bd;
            if (onBeat || local - m_beatWaitStart > kBeatWaitTimeout) {
                m_waitingForBeat = false;
                sendEvent(contentStart, kHandoverKey, ++m_epoch, nullptr, trueStart);
                becomeListener(contentStart, local, trueStart);
            }
        }
    }

    if (m_live) {
        processLive(contentStart, trueStart, bufferSec);
    } else {
        // Adopt a new target if the peer's offset or our measured delay moved.
        const Nanos peerOffset = m_peerOffset.load(std::memory_order_acquire);
        const Nanos delay = m_listenDelay.load(std::memory_order_acquire);
        const bool fromLive = m_peerOffsetFromLive.load(std::memory_order_acquire);
        if (delay > 0 && fromLive && (peerOffset != m_appliedPeerOffset || delay != m_appliedListenDelay)) {
            m_appliedPeerOffset = peerOffset;
            m_appliedListenDelay = delay;
            const double target = static_cast<double>(peerOffset + delay);
            const double current = m_slew.active ? m_slew.to : m_offset;
            const double diff = target - current;
            if (diff > kRetargetUpNs || diff < -kRetargetDownNs) {
                retarget(local, target, false);
                contentStart = shared - static_cast<Nanos>(m_offset);
            }
        }
        processListener(contentStart, trueStart, bufferSec);
    }
    m_offsetShared.store(static_cast<Nanos>(m_offset), std::memory_order_release);

    if (m_log && m_callbacks % kLogEveryCallbacks == 0) {
        logPositions(trueStart);
    }
}

// ---------------------------------------------------------------------------
// Roles and handovers

void EngineAdapter::resetDeckStates() {
    for (DeckState& st : m_state) {
        const uint64_t liveTrack = st.liveTrackId;
        st = DeckState{};
        st.liveTrackId = liveTrack;
    }
}

void EngineAdapter::becomeLive(Nanos contentStart, Nanos trueStart) {
    if (m_slew.active) {
        endCatchUp(); // keep the offset we have reached: our music carries on as it is
    }
    m_live = true;
    m_waitingForBeat = false;
    m_pending.clear();
    m_snapshots.clear();
    resetDeckStates();
    for (Watched& w : m_watched) {
        if (w.displayOnly && w.control->get() > 0.5) {
            w.control->set(1.0, nullptr); // the sync button was only lit: engage it for real
        }
        w.last = w.control->get();
    }
    m_liveShared.store(true, std::memory_order_release);
    m_notices.push({Notice::BecameLive, 0, 0});
    if (m_log) {
        m_log->add({'H', 1, ::b2b::toMs(static_cast<Nanos>(m_offset)), -1, 1.0, trueStart,
                static_cast<double>(contentStart)});
    }
}

void EngineAdapter::becomeListener(Nanos contentStart, Nanos local, Nanos trueStart) {
    m_live = false;
    m_liveShared.store(false, std::memory_order_release);
    m_waitingForBeat = false;
    m_pending.clear();
    m_snapshots.clear();
    resetDeckStates();
    for (Watched& w : m_watched) {
        if (w.displayOnly && w.control->get() > 0.5) {
            // Our own sync engine must not move decks the peer now drives:
            // switch it off, the listener loop lights the button again.
            w.control->set(0.0, nullptr);
        }
        w.fromLive = true; // everything is the live side's from now on
        w.appliedStamp = std::numeric_limits<Nanos>::min(); // a new live side, a new timeline
    }
    for (size_t d = 0; d < m_decks.size(); ++d) {
        m_state[d].liveTrackId = m_deckTrackId[d].load(std::memory_order_acquire);
    }

    // Fall behind the new live side by our listening delay.
    const Nanos delay = m_listenDelay.load(std::memory_order_acquire);
    const Nanos peerOffset = m_peerOffset.load(std::memory_order_acquire);
    m_appliedPeerOffset = peerOffset;
    m_appliedListenDelay = delay;
    const double target = std::max(static_cast<double>(peerOffset + delay), m_offset + static_cast<double>(delay));
    const double before = m_offset;
    const bool snap = m_style.load(std::memory_order_acquire) == static_cast<int>(HandoverStyle::Snap);
    retarget(local, target, snap);

    Notice n;
    n.kind = Notice::BecameListener;
    n.shiftMs = (std::max(m_slew.active ? m_slew.to : m_offset, target) - before) / 1e6;
    n.durationSec = m_slew.active ? m_slew.durationNs / 1e9 : 0.0;
    m_notices.push(n);
    if (m_log) {
        m_log->add({'H', 0, n.shiftMs, -1, n.durationSec, trueStart, static_cast<double>(contentStart)});
    }
}

int EngineAdapter::referenceDeck() const {
    // The loudest playing deck sets the beat.
    int best = -1;
    double bestVolume = -1;
    for (size_t d = 0; d < m_decks.size(); ++d) {
        if (!deckPlaying(static_cast<int>(d))) {
            continue;
        }
        const double v = m_decks[d].volume ? m_decks[d].volume->get() : 1.0;
        if (v > bestVolume) {
            bestVolume = v;
            best = static_cast<int>(d);
        }
    }
    return best;
}

double EngineAdapter::beatLengthSec(int deck) const {
    const double bpm = deck >= 0 && m_decks[deck].bpm ? m_decks[deck].bpm->get() : 0.0;
    return bpm > 20 && bpm < 400 ? 60.0 / bpm : 0.5;
}

void EngineAdapter::jumpOffset(double newOffset) {
    // Content time jumps back by the change: playing decks go back with it.
    const double shiftSec = (newOffset - m_offset) / 1e9;
    for (size_t d = 0; d < m_decks.size(); ++d) {
        if (deckPlaying(static_cast<int>(d))) {
            seekTo(static_cast<int>(d), positionSec(static_cast<int>(d)) - shiftSec * speed(static_cast<int>(d)));
        }
    }
    m_offset = newOffset;
}

void EngineAdapter::retarget(Nanos local, double target, bool snap) {
    const int ref = referenceDeck();
    if (m_slew.active) {
        endCatchUp();
    }
    const double shift = target - m_offset;
    if (ref < 0) {
        m_offset = target; // nothing is playing: nobody hears the change
        return;
    }
    if (snap && shift > 0) {
        // Jump back a whole number of beats, so the music stays on the beat.
        const double beatNs = beatLengthSec(ref) * 1e9;
        const double beats = std::ceil(shift / beatNs);
        jumpOffset(m_offset + beats * beatNs);
        return;
    }
    if (std::fabs(shift) > kJumpInsteadNs) {
        jumpOffset(target);
        return;
    }
    startCatchUp(local, target, false);
}

void EngineAdapter::startCatchUp(Nanos local, double target, bool /*instant*/) {
    const int ref = referenceDeck();
    const double bars = std::max(0.25, m_catchUpBars.load(std::memory_order_acquire));
    double durationNs = bars * 4 * beatLengthSec(ref) * 1e9;
    const double shift = std::fabs(target - m_offset);
    // Never change speed by more than kMaxCatchUpTrim: take longer instead.
    durationNs = std::max(durationNs, shift / ((1 - kRampFraction) * kMaxCatchUpTrim));
    m_slew.active = true;
    m_slew.startLocal = local;
    m_slew.durationNs = durationNs;
    m_slew.from = m_offset;
    m_slew.to = target;
    m_catchUpProgress.store(0.0, std::memory_order_relaxed);
}

void EngineAdapter::stepCatchUp(Nanos local) {
    const double p = static_cast<double>(local - m_slew.startLocal) / m_slew.durationNs;
    if (p >= 1) {
        m_offset = m_slew.to;
        endCatchUp();
        m_notices.push({Notice::CatchUpDone, 0, 0});
        return;
    }
    const double change = m_slew.to - m_slew.from;
    m_offset = m_slew.from + change * displacement(p);
    // How much slower than real time content advances now (positive = slower).
    const double trim = change * velocity(p) / ((1 - kRampFraction) * m_slew.durationNs);
    for (size_t d = 0; d < m_decks.size(); ++d) {
        const DeckRefs& deck = m_decks[d];
        if (deck.wheel) {
            const double ratio = deck.rateRatio ? deck.rateRatio->get() : 1.0;
            deck.wheel->set(deckPlaying(static_cast<int>(d)) ? -trim * ratio : 0.0, nullptr);
        }
        if (deck.keylock && deck.keylock->get() < 0.5) {
            deck.keylock->set(1.0, nullptr); // keep the pitch steady while the tempo eases
        }
    }
    m_catchUpProgress.store(p, std::memory_order_relaxed);
}

void EngineAdapter::endCatchUp() {
    m_slew.active = false;
    for (DeckRefs& deck : m_decks) {
        if (deck.wheel) {
            deck.wheel->set(0.0, nullptr);
        }
    }
    for (Watched& w : m_watched) {
        if (w.isKeylock && w.control->get() != w.last) {
            w.control->set(w.last, nullptr);
        }
    }
    m_catchUpProgress.store(-1.0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Live

void EngineAdapter::sendEvent(Nanos time, const FixedString<64>& key, double value,
        const char* text, Nanos trueStart) {
    ControlEvent ev;
    ev.seq = m_nextSeq++;
    ev.time = time;
    ev.key = key;
    ev.value = value;
    ev.text = text ? text : "";
    m_session.sendEvent(ev);
    if (m_log && !(text && (text[0] == 'p' || text[0] == 's'))) {
        m_log->add({'E', static_cast<int64_t>(ev.seq), 0.0, deckForKey(key.view()), value, trueStart, 0.0});
    }
}

void EngineAdapter::applyPrep(const ControlEvent& ev) {
    // The listener changed a deck that is not playing (loading, setting
    // tempo, EQ...). Accept it unless the deck has started playing here.
    const std::string_view key = ev.key.view();
    const int d = deckForKey(key);
    if (d < 0) {
        return;
    }
    if (endsWith(key, ",b2b_prep_load")) {
        if (!deckPlaying(d)) {
            m_loadRequests.push({d, parseHex(ev.text.view())});
        }
        return;
    }
    Watched* w = findWatched(key);
    if (!w || w->deck < 0 || w->isPlay || w->displayOnly) {
        return;
    }
    if (deckPlaying(d)) {
        w->last = std::numeric_limits<double>::quiet_NaN(); // refuse: resend our value
        return;
    }
    w->control->set(fromWire(*w, ev.value), nullptr);
    w->last = w->control->get(); // not echoed back: the listener already has it
    m_prepChanges.fetch_add(1, std::memory_order_relaxed);
}

void EngineAdapter::processLive(Nanos contentStart, Nanos trueStart, double /*bufferSec*/) {
    const int numDecks = static_cast<int>(m_decks.size());

    // From the listener: deck prep only. (Anything else is a leftover from
    // before we went live.)
    ControlEvent incoming;
    while (m_session.popReceivedEvent(incoming)) {
        const bool prep = incoming.text == "prep" || endsWith(incoming.key.view(), ",b2b_prep_load");
        if (prep) {
            applyPrep(incoming);
        }
        if (m_log) {
            m_log->add({'R', static_cast<int64_t>(incoming.seq), 0.0, deckForKey(incoming.key.view()),
                    incoming.value, trueStart, prep ? 1.0 : 0.0});
        }
    }
    DeckSnapshot snap;
    while (m_session.popReceivedSnapshot(snap)) {
    }

    // The peer just (re)connected: send it the full current state.
    const uint32_t generation = m_session.peerGeneration();
    const bool sendAll = generation != m_seenPeerGeneration;
    m_seenPeerGeneration = generation;

    // 1. Deck positions: report jumps (seeks, hot cues, loops) and play/pause
    //    with the exact position, so the listener lands on the same sample.
    for (int d = 0; d < numDecks; ++d) {
        DeckState& st = m_state[d];
        if (!positionKnown(d)) {
            st.havePrev = false; // loading: start afresh once it's there
            continue;
        }
        const double pos = positionSec(d);
        const double spd = speed(d);
        const bool playing = deckPlaying(d);
        const double rateIfPlaying = playing ? m_decks[d].buffer->getRateRatio() : 0.0;

        // Where the deck should be now if it simply kept playing, measured on
        // the content timeline. Anything else is a jump to report: a seek, hot
        // cue or loop, but also an audio dropout (xrun) that made the music
        // slip against the clock. Either way the listener must match it now.
        bool jumped = false;
        if (st.havePrev) {
            const double elapsed = static_cast<double>(contentStart - st.prevShared) / 1e9;
            const double expected = st.prevPosSec + spd * elapsed;
            jumped = std::fabs(pos - expected) > kJumpThresholdSec;
        }
        const bool playChanged = st.havePrev && playing != st.prevPlaying;
        if (sendAll || jumped || playChanged) {
            char rateText[32];
            std::snprintf(rateText, sizeof(rateText), "%.9f", rateIfPlaying);
            sendEvent(contentStart, m_posKeys[d], pos, rateText, trueStart);
        }
        st.prevPosSec = pos;
        st.prevShared = contentStart;
        st.prevPlaying = playing;
        st.havePrev = true;

        // 2. Regular snapshots so the listener can catch any drift.
        if (contentStart - st.lastSnapshot >= kSnapshotInterval) {
            st.lastSnapshot = contentStart;
            DeckSnapshot s;
            s.time = contentStart;
            s.deck = static_cast<uint8_t>(d + 1);
            s.playing = playing;
            s.positionSec = pos;
            s.rate = playing ? spd : 0.0;
            s.trackId = m_deckTrackId[d].load(std::memory_order_acquire);
            m_session.sendSnapshot(s);
        }
    }

    // 3. Control changes (faders, EQs, effects, play, tempo, sync...).
    for (Watched& w : m_watched) {
        const double v = w.control->get();
        if (v != w.last || (sendAll && w.isPlay)) {
            w.last = v;
            sendEvent(contentStart, w.key, toWire(w, v), nullptr, trueStart);
        }
    }

    // 4. Now and then the whole state again (and all of it when the peer
    //    connects), a few controls per callback so it never comes in a burst.
    if (sendAll || contentStart - m_lastRefresh >= kRefreshInterval) {
        m_lastRefresh = contentStart;
        m_refreshCursor = 0;
    }
    for (int budget = kRefreshPerCallback; budget > 0 && m_refreshCursor < m_watched.size(); ++m_refreshCursor) {
        Watched& w = m_watched[m_refreshCursor];
        if (!w.isPlay) {
            sendEvent(contentStart, w.key, toWire(w, w.last), "state", trueStart);
            --budget;
        }
    }
}

// ---------------------------------------------------------------------------
// Listener

void EngineAdapter::processListener(Nanos contentStart, Nanos trueStart, double bufferSec) {
    ControlEvent incoming;
    while (m_session.popReceivedEvent(incoming)) {
        auto pos = std::upper_bound(m_pending.begin(), m_pending.end(), incoming,
                [](const ControlEvent& a, const ControlEvent& b) { return a.time < b.time; });
        m_pending.insert(pos, incoming);
    }
    DeckSnapshot snap;
    while (m_session.popReceivedSnapshot(snap)) {
        if (m_snapshots.size() >= m_snapshots.capacity()) {
            m_snapshots.erase(m_snapshots.begin());
        }
        m_snapshots.push_back(snap);
    }
    for (DeckState& st : m_state) {
        st.seekedThisCallback = false;
    }
    // Diagnostics: did play/pause change since our last callback without B2B
    // doing it? (Mixxx itself stopping at the end of a track also shows here.)
    for (size_t d = 0; d < m_decks.size(); ++d) {
        const double play = m_decks[d].play->get();
        if (m_state[d].lastPlay >= 0 && play != m_state[d].lastPlay) {
            m_outsidePlayChanges.fetch_add(1, std::memory_order_relaxed);
            if (m_log) {
                m_log->add({'L', static_cast<int64_t>(d), positionSec(static_cast<int>(d)),
                        static_cast<int>(d), play, trueStart, 0.0});
            }
        }
    }

    // Apply every change that belongs to this callback. Rounding to the
    // nearest callback keeps the error within half a buffer either way.
    const Nanos due = contentStart + static_cast<Nanos>(bufferSec * 0.5e9);
    while (!m_pending.empty() && m_pending.front().time < due) {
        const ControlEvent ev = m_pending.front();
        m_pending.erase(m_pending.begin());
        applyEvent(ev, contentStart, trueStart);
        if (m_live) {
            return; // the peer handed control to us: from here on, we lead
        }
    }

    // The live side owns the mix: put back anything changed locally (a stray
    // click, or a controller sending input). Except deck prep: settings on a
    // deck that isn't playing go to the live side instead. Play/pause is left
    // to the snapshot check, which also handles the end of a track.
    for (Watched& w : m_watched) {
        if (!w.fromLive || w.isPlay || (w.isKeylock && m_slew.active)) {
            continue;
        }
        const double v = w.control->get();
        if (v == w.last) {
            continue;
        }
        if (w.displayOnly) {
            setSyncDisplay(w, w.last);
            m_overrides.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Deck prep only when the *live side* has the deck stopped. Going by
        // our own deck here let a fader moved while our copy was paused or
        // still loading (but playing on the live side) stick at our value.
        if (w.deck >= 0 && !livePlaying(w.deck)) {
            sendEvent(contentStart, w.key, toWire(w, v), "prep", trueStart);
            w.last = v;
            w.lastPrep = contentStart;
            m_prepChanges.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        w.control->set(w.last, nullptr);
        m_overrides.fetch_add(1, std::memory_order_relaxed);
        if (m_log) {
            m_log->add({'O', 0, v, w.deck, w.last, trueStart, w.deck >= 0 ? m_decks[w.deck].play->get() : -1.0});
        }
    }

    correctFromSnapshots(contentStart, trueStart);

    for (size_t d = 0; d < m_decks.size(); ++d) {
        m_state[d].lastPlay = m_decks[d].play->get();
    }
}

void EngineAdapter::setSyncDisplay(Watched& w, double v) {
    if (v > 0.5) {
        if (w.control->get() < 0.5) {
            w.displayOnly->setAndConfirm(1.0); // light the button without engaging sync
        }
    } else if (w.control->get() > 0.5) {
        w.control->set(0.0, nullptr); // off (disengages sync if it was engaged)
    }
}

void EngineAdapter::applyEvent(const ControlEvent& ev, Nanos contentStart, Nanos trueStart) {
    const std::string_view key = ev.key.view();
    if (ev.key.view() == kHandoverKey.view()) {
        becomeLive(contentStart, trueStart);
        return;
    }
    const int d = deckForKey(key);

    if (endsWith(key, ",b2b_load")) {
        if (d >= 0) {
            const uint64_t id = parseHex(ev.text.view());
            m_state[d].liveTrackId = id;
            m_loadRequests.push({d, id});
        }
    } else if (endsWith(key, ",b2b_pos")) {
        if (d >= 0) {
            DeckState& st = m_state[d];
            st.lastPositionEvent = ev.time;
            st.errCount = 0;
            const uint64_t mine = m_deckTrackId[d].load(std::memory_order_acquire);
            if (mine != 0 && (st.liveTrackId == 0 || st.liveTrackId == mine)) {
                const double rate = std::strtod(ev.text.c_str(), nullptr);
                // Where the live deck is at the start of *our* callback.
                const double target = ev.value + static_cast<double>(contentStart - ev.time) / 1e9 * rate;
                seekTo(d, target);
            }
        }
    } else if (Watched* w = findWatched(key)) {
        if (ev.text == "state" && ev.time < w->appliedStamp) {
            return; // a refresh that took longer than a newer change: stale
        }
        if (ev.time > w->appliedStamp) {
            w->appliedStamp = ev.time;
        }
        if (ev.text == "state") {
            // A refresh: only act on a real difference, and not while our own
            // deck prep is still on its way to the live side.
            const double current = w->displayOnly ? (w->control->get() > 0.5 ? 1.0 : 0.0)
                                                  : toWire(*w, w->control->get());
            if (current == ev.value || contentStart - w->lastPrep < kPrepGrace ||
                    (w->isKeylock && m_slew.active)) {
                w->fromLive = true;
                if (current == ev.value) {
                    w->last = w->displayOnly ? current : w->control->get();
                }
                return;
            }
            m_resyncs.fetch_add(1, std::memory_order_relaxed);
        }
        if (w->displayOnly) {
            setSyncDisplay(*w, ev.value);
            w->last = ev.value > 0.5 ? 1.0 : 0.0;
        } else if (!(w->isKeylock && m_slew.active)) {
            w->control->set(fromWire(*w, ev.value), nullptr);
            w->last = w->control->get(); // as stored (controls may clamp or round)
        } else {
            w->last = ev.value; // restored when the catch-up ends
        }
        w->fromLive = true;
        if (d >= 0 && w->isPlay) {
            m_state[d].lastPositionEvent = ev.time;
        }
    }

    m_appliedEvents.fetch_add(1, std::memory_order_relaxed);
    if (m_log) {
        // extra: how far from the ideal moment it was applied (ms, + = late)
        // beat: 1 = a state refresh, 0 = a change
        // beat: 1 = a state refresh, 0 = a change; +10 in "a deck each"
        m_log->add({'E', static_cast<int64_t>(ev.seq),
                (ev.text == "state" ? 1.0 : 0.0) + (m_mode == Mode::DeckEach ? 10.0 : 0.0), d, ev.value, trueStart,
                ::b2b::toMs(contentStart - ev.time)});
    }
}

void EngineAdapter::correctFromSnapshots(Nanos contentStart, Nanos trueStart) {
    const int numDecks = static_cast<int>(m_decks.size());
    for (int d = 0; d < numDecks; ++d) {
        DeckState& st = m_state[d];
        if (st.seekedThisCallback || (m_mode == Mode::DeckEach && owns(d)) || !positionKnown(d)) {
            continue;
        }
        // Latest snapshot for this deck that is not in our future.
        const DeckSnapshot* snap = nullptr;
        for (auto it = m_snapshots.rbegin(); it != m_snapshots.rend(); ++it) {
            if (it->deck == d + 1 && it->time <= contentStart) {
                snap = &*it;
                break;
            }
        }
        if (!snap || snap->trackId == 0) {
            continue;
        }
        st.liveTrackId = snap->trackId;
        if (snap->trackId != m_deckTrackId[d].load(std::memory_order_acquire)) {
            continue; // our copy of the track is not loaded (yet)
        }
        if (snap->time < st.lastPositionEvent) {
            continue; // older than a play/seek we already applied
        }
        if (m_mode == Mode::DeckEach && !snap->playing && deckPlaying(d)) {
            // "A deck each": the friend has their deck stopped, so ours stays
            // stopped too. Played here (a click, a key, a controller) only we
            // would hear it: the friend wouldn't, and they own it. The main
            // thread says so and offers to ask for the deck.
            m_decks[d].play->set(0.0, nullptr);
            m_blockedPlay[d].store(true, std::memory_order_release);
            m_overrides.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (snap->time == st.lastSnapshotUsed) {
            continue; // judge each snapshot once, not every callback
        }
        st.lastSnapshotUsed = snap->time;
        if (!snap->playing && m_mode == Mode::DeckEach && m_decks[d].wheel && m_decks[d].wheel->get() != 0) {
            m_decks[d].wheel->set(0.0, nullptr);
        }
        if (!snap->playing) {
            // The live side has this deck stopped: it's ours to prepare (cue,
            // preview, scrub) like the second DJ in a real booth. Holding it
            // to the live side's paused position would undo every CUE press
            // and turn the next one into a new cue point. Play and stop still
            // arrive as events; when the live side starts this deck we follow
            // it again.
            st.errCount = 0;
            continue;
        }

        const double elapsed = static_cast<double>(contentStart - snap->time) / 1e9;
        const double expected = snap->playing ? snap->positionSec + elapsed * snap->rate : snap->positionSec;
        const bool ownPlaying = deckPlaying(d);
        const double pos = positionSec(d);
        const double err = pos - expected;

        if (ownPlaying != snap->playing) {
            m_decks[d].play->set(snap->playing ? 1.0 : 0.0, nullptr);
            seekTo(d, expected);
            st.lastPositionEvent = snap->time;
            st.errCount = 0;
            m_corrections.fetch_add(1, std::memory_order_relaxed);
            if (m_log) {
                m_log->add({'K', d, expected, d, snap->playing ? 1.0 : 0.0, trueStart, err * 1000.0});
            }
            continue;
        }
        if (m_log) {
            m_log->add({'D', d, expected, d, snap->playing ? 1.0 : 0.0, trueStart, err * 1000.0});
        }

        if (m_mode == Mode::DeckEach) {
            // Mixed against our own decks: play at exactly the friend's
            // speed (what their deck really does, SYNC included), and close
            // any small difference in position with a tiny, inaudible change
            // of speed. Jump only if it's well off.
            st.errHistory[st.errCount % kErrHistory] = err;
            st.errCount++;
            double steer = 0;
            const double previous = st.errCount >= 2 ? st.errHistory[(st.errCount - 2) % kErrHistory] : 1e9;
            const bool moving = std::fabs(speed(d)) > 0.01 || std::fabs(snap->rate) < 0.01;
            if (contentStart - st.lastPositionEvent < kSnapAfterStart && std::fabs(err) > kSnapAfterStartSec &&
                    std::fabs(err - previous) < 0.001 && moving) {
                // Just started (or jumped), and two readings agree: put it
                // right at once, before anyone hears the difference.
                seekTo(d, expected);
                st.errCount = 0;
                m_corrections.fetch_add(1, std::memory_order_relaxed);
                if (m_log) {
                    m_log->add({'K', d, expected, d, 1.0, trueStart, err * 1000.0});
                }
            } else if (st.errCount >= 3) {
                double recent[3] = {
                        st.errHistory[(st.errCount - 1) % kErrHistory],
                        st.errHistory[(st.errCount - 2) % kErrHistory],
                        st.errHistory[(st.errCount - 3) % kErrHistory],
                };
                std::sort(recent, recent + 3);
                const double persistent = recent[1];
                if (std::fabs(persistent) > kSteerJumpSec) {
                    seekTo(d, pos - persistent);
                    st.errCount = 0;
                    m_corrections.fetch_add(1, std::memory_order_relaxed);
                    if (m_log) {
                        m_log->add({'K', d, pos - persistent, d, 1.0, trueStart, persistent * 1000.0});
                    }
                } else {
                    steer = std::clamp(persistent / kSteerSeconds, -kSteerMaxTrim, kSteerMaxTrim);
                }
            }
            if (m_decks[d].wheel) {
                // While playing, Mixxx adds the wheel straight onto the speed.
                const double wheel = m_decks[d].wheel->get();
                const double ownSpeed = speed(d) - wheel;
                const double ratio = std::fabs(snap->rate) > 0.01 ? std::fabs(snap->rate) : 1.0;
                m_decks[d].wheel->set((snap->rate - ownSpeed) - steer * ratio, nullptr);
            }
            continue;
        }

        // Correct only a *persistent* error (median of the last three
        // snapshots), so momentary timing jitter never causes a jump.
        st.errHistory[st.errCount % kErrHistory] = err;
        st.errCount++;
        if (st.errCount < 3) {
            continue;
        }
        double last3[3] = {
                st.errHistory[(st.errCount - 1) % kErrHistory],
                st.errHistory[(st.errCount - 2) % kErrHistory],
                st.errHistory[(st.errCount - 3) % kErrHistory],
        };
        std::sort(last3, last3 + 3);
        const double persistent = last3[1];
        double threshold = snap->playing ? m_correctThresholdSec : kPausedPositionToleranceSec;
        if (m_slew.active) {
            threshold = std::max(threshold, kCatchUpThresholdSec);
        }
        if (std::fabs(persistent) > threshold) {
            seekTo(d, pos - persistent);
            st.errCount = 0;
            m_corrections.fetch_add(1, std::memory_order_relaxed);
            if (m_log) {
                m_log->add({'K', d, pos - persistent, d, snap->playing ? 1.0 : 0.0, trueStart,
                        persistent * 1000.0});
            }
        }
    }

    // Forget snapshots more than two seconds old.
    const Nanos cutoff = contentStart - 2 * ::b2b::kNanosPerSec;
    auto keepFrom = std::find_if(m_snapshots.begin(), m_snapshots.end(),
            [cutoff](const DeckSnapshot& s) { return s.time >= cutoff; });
    m_snapshots.erase(m_snapshots.begin(), keepFrom);
}

// ---------------------------------------------------------------------------
// A deck each

void EngineAdapter::Scheduled::set(Nanos time, double value, Nanos now) {
    if (time <= now) {
        current = value;
        return;
    }
    if (count == kSize) {
        // Very busy: drop the oldest planned change.
        current = values[0];
        for (int i = 1; i < count; ++i) {
            times[i - 1] = times[i];
            values[i - 1] = values[i];
        }
        --count;
    }
    int i = count;
    while (i > 0 && times[i - 1] > time) {
        times[i] = times[i - 1];
        values[i] = values[i - 1];
        --i;
    }
    times[i] = time;
    values[i] = value;
    ++count;
}

double EngineAdapter::Scheduled::at(Nanos now) {
    int done = 0;
    while (done < count && times[done] <= now) {
        current = values[done];
        ++done;
    }
    if (done > 0) {
        for (int i = done; i < count; ++i) {
            times[i - done] = times[i];
            values[i - done] = values[i];
        }
        count -= done;
    }
    return current;
}

void EngineAdapter::shiftOwnDecks(double seconds) {
    // Our decks are heard `lead` after they play: when that starts (or stops),
    // a playing deck moves along by the lead to stay on the shared timeline.
    for (size_t d = 0; d < m_decks.size(); ++d) {
        if (owns(static_cast<int>(d)) && deckPlaying(static_cast<int>(d))) {
            seekTo(static_cast<int>(d), positionSec(static_cast<int>(d)) + seconds * speed(static_cast<int>(d)));
        }
    }
}

void EngineAdapter::switchMode(Mode mode, Nanos contentStart, Nanos local, Nanos trueStart) {
    if (m_slew.active) {
        endCatchUp();
    }
    m_waitingForBeat = false;
    m_pending.clear();
    m_snapshots.clear();
    resetDeckStates();

    if (mode == Mode::DeckEach) {
        // Both peers on the host's timeline.
        const double target = static_cast<double>(m_requestedOffset.load(std::memory_order_acquire));
        if (target != m_offset) {
            jumpOffset(target);
            contentStart = m_session.sharedFromLocal(local) - static_cast<Nanos>(m_offset);
        }
        m_mode = Mode::DeckEach;
        m_owned = m_ownedMask.load(std::memory_order_acquire);
        const Nanos lead = m_requestedLead.load(std::memory_order_acquire);
        m_lead = lead >= 0 ? lead : 0;
        for (size_t d = 0; d < m_decks.size(); ++d) {
            m_state[d].liveTrackId = m_deckTrackId[d].load(std::memory_order_acquire);
            m_volume[d].reset(m_decks[d].volume ? m_decks[d].volume->get() : 1.0);
            m_pregain[d].reset(m_decks[d].pregain ? m_decks[d].pregain->get() : 1.0);
            m_pregain[d].count = 0;
        }
        for (Watched& w : m_watched) {
            const bool mine = w.deck >= 0 && owns(w.deck);
            if (w.displayOnly) {
                if (mine && w.control->get() > 0.5) {
                    w.control->set(1.0, nullptr); // was only lit: engage our own sync
                } else if (!mine && w.control->get() > 0.5) {
                    w.control->set(0.0, nullptr); // the friend's deck: their sync, not ours
                }
            }
            w.fromLive = w.deck >= 0 && !mine;
            w.last = w.control->get();
            w.localStamp = 0;
            w.appliedStamp = std::numeric_limits<Nanos>::min();
        }
        m_crossfader.reset(m_crossfaderWatched >= 0 ? m_watched[m_crossfaderWatched].last : 0.0);
        shiftOwnDecks(static_cast<double>(m_lead) / 1e9);
        m_leadShared.store(m_lead, std::memory_order_release);
    } else {
        // Back to taking turns: the host is live, the guest listens.
        shiftOwnDecks(-static_cast<double>(m_lead) / 1e9);
        for (DeckRefs& deck : m_decks) {
            if (deck.wheel) {
                deck.wheel->set(0.0, nullptr); // no more steering
            }
        }
        m_mode = Mode::TakeTurns;
        if (m_pMixer) {
            m_pMixer->b2bTiming() = EngineMixer::B2BTiming();
        }
        for (DeckRefs& deck : m_decks) {
            if (deck.buffer) {
                deck.buffer->setB2BLeadSeconds(0);
            deck.buffer->setB2BFollowed(false);
            deck.buffer->setB2BHeardDelaySeconds(0);
            }
        }
        m_timingSet = false;
        m_leadShared.store(0, std::memory_order_release);
        if (m_session.role() == ::b2b::Role::Leader) {
            becomeLive(contentStart, trueStart);
        } else {
            becomeListener(contentStart, local, trueStart);
        }
    }
    m_modeShared.store(static_cast<int>(m_mode), std::memory_order_release);
    Notice n;
    n.kind = Notice::ModeChanged;
    m_notices.push(n);
    if (m_log) {
        m_log->add({'M', static_cast<int64_t>(m_mode), ::b2b::toMs(m_lead), -1,
                static_cast<double>(m_owned), trueStart, static_cast<double>(contentStart)});
    }
}

void EngineAdapter::sendDeckPosition(int d, Nanos deckContent, bool sendAll, Nanos trueStart) {
    DeckState& st = m_state[d];
    if (!positionKnown(d)) {
        st.havePrev = false; // loading: start afresh once it's there
        st.startPending = false;
        return;
    }
    const double pos = positionSec(d);
    const double spd = speed(d);
    const bool playing = deckPlaying(d);
    const double rateIfPlaying = playing ? m_decks[d].buffer->getRateRatio() : 0.0;
    bool jumped = false;
    if (st.havePrev) {
        const double elapsed = static_cast<double>(deckContent - st.prevShared) / 1e9;
        const double expected = st.prevPosSec + spd * elapsed;
        jumped = std::fabs(pos - expected) > kTogetherJumpSec;
    }
    const bool playChanged = st.havePrev && playing != st.prevPlaying;
    st.startStamp = 0; // only for this callback's play
    if (st.startPending) {
        // Started last callback: say where it started, as of then (after a
        // quantize jump the engine made), and the play goes with that stamp.
        st.startPending = false;
        if (playing) {
            const double elapsed = static_cast<double>(deckContent - st.prevShared) / 1e9;
            const double startPos = pos - spd * elapsed;
            char rateText[32];
            std::snprintf(rateText, sizeof(rateText), "%.9f", rateIfPlaying);
            sendEvent(st.prevShared, m_posKeys[d], startPos, rateText, trueStart);
            st.startStamp = st.prevShared;
            jumped = false;
        }
    } else if (playChanged && playing && !sendAll) {
        st.startPending = true;
    } else if (playChanged && !playing && !jumped && !m_decks[d].buffer->b2bSeekQueued()) {
        // Paused: you last heard it `lead` back (what was still on its way
        // to you stops at once), so that's where it waits, and where a cue
        // you set now goes. (Not when the stop comes with a jump of its
        // own: CUE back to the cue point.)
        const double heard = pos - static_cast<double>(m_lead) / 1e9 * std::fabs(st.prevSpeed);
        seekTo(d, heard);
        char rateText[32];
        std::snprintf(rateText, sizeof(rateText), "%.9f", rateIfPlaying);
        sendEvent(deckContent, m_posKeys[d], heard, rateText, trueStart);
        st.prevSpeed = 0;
        st.prevPosSec = heard;
        st.prevShared = deckContent;
        st.prevPlaying = playing;
        return; // no snapshot from before the jump this callback
    } else if (sendAll || jumped || playChanged) {
        char rateText[32];
        std::snprintf(rateText, sizeof(rateText), "%.9f", rateIfPlaying);
        sendEvent(deckContent, m_posKeys[d], pos, rateText, trueStart);
    }
    st.prevPosSec = pos;
    st.prevShared = deckContent;
    st.prevPlaying = playing;
    st.prevSpeed = spd;
    st.havePrev = true;
    if (!st.startPending && deckContent - st.lastSnapshot >= kSnapshotInterval &&
            !(playing && spd == 0.0)) {
        st.lastSnapshot = deckContent;
        DeckSnapshot snap;
        snap.time = deckContent;
        snap.deck = static_cast<uint8_t>(d + 1);
        snap.playing = playing;
        snap.positionSec = pos;
        snap.rate = playing ? spd : 0.0;
        snap.trackId = m_deckTrackId[d].load(std::memory_order_acquire);
        m_session.sendSnapshot(snap);
    }
}

void EngineAdapter::processTogether(Nanos contentStart, Nanos trueStart, double bufferSec) {
    const int numDecks = static_cast<int>(m_decks.size());

    // A planned change of hands, at its moment.
    const int64_t planned = m_switchMask.load(std::memory_order_acquire);
    if (planned >= 0 && contentStart >= m_switchTime.load(std::memory_order_acquire)) {
        m_switchMask.store(-1, std::memory_order_release);
        m_ownedMask.store(static_cast<uint32_t>(planned), std::memory_order_release);
    }
    // Decks changing hands: a playing one moves by the lead so the music
    // stays where it is on the shared timeline.
    const uint32_t mask = m_ownedMask.load(std::memory_order_acquire);
    if (mask != m_owned) {
        for (int d = 0; d < numDecks; ++d) {
            const bool was = owns(d);
            const bool now = (mask >> d) & 1u;
            if (was == now) {
                continue;
            }
            if (deckPlaying(d)) {
                seekTo(d, positionSec(d) + (now ? 1.0 : -1.0) * static_cast<double>(m_lead) / 1e9 * speed(d));
            }
            if (m_decks[d].wheel) {
                m_decks[d].wheel->set(0.0, nullptr);
            }
            m_volume[d].reset(m_decks[d].volume ? m_decks[d].volume->get() : 1.0);
            const uint64_t liveTrack = m_deckTrackId[d].load(std::memory_order_acquire);
            m_state[d] = DeckState{};
            m_state[d].liveTrackId = liveTrack;
            for (Watched& w : m_watched) {
                if (w.deck == d) {
                    w.fromLive = !now;
                    w.last = w.control->get();
                }
            }
        }
        m_owned = mask;
    }

    // A new lead only while none of our decks plays (it restarts the delay).
    bool ownPlaying = false;
    for (int d = 0; d < numDecks; ++d) {
        ownPlaying = ownPlaying || (owns(d) && deckPlaying(d));
    }
    m_ownPlaying.store(ownPlaying, std::memory_order_release);
    const Nanos wantedLead = m_requestedLead.load(std::memory_order_acquire);
    if (wantedLead >= 0 && wantedLead != m_lead && !ownPlaying) {
        m_lead = wantedLead;
        m_leadShared.store(m_lead, std::memory_order_release);
        for (int d = 0; d < numDecks; ++d) {
            if (owns(d)) {
                m_state[d].havePrev = false; // its timeline moved
            }
        }
    }
    const Nanos ownContent = contentStart + m_lead; // where our own decks are
    m_contentShared.store(contentStart, std::memory_order_release);
    mixtiming::g_contentNow.store(contentStart, std::memory_order_release);

    // 1. What the friend sent.
    ControlEvent incoming;
    while (m_session.popReceivedEvent(incoming)) {
        if (incoming.text == "mix") {
            // The friend's move on one of our decks' mixer row: due when it's
            // heard, and our decks play `lead` ahead of that.
            const int d = deckForKey(incoming.key.view());
            if (d >= 0 && owns(d)) {
                incoming.time -= m_lead;
            }
        }
        auto pos = std::upper_bound(m_pending.begin(), m_pending.end(), incoming,
                [](const ControlEvent& a, const ControlEvent& b) { return a.time < b.time; });
        m_pending.insert(pos, incoming);
    }
    DeckSnapshot snap;
    while (m_session.popReceivedSnapshot(snap)) {
        if (snap.deck >= 1 && owns(snap.deck - 1)) {
            continue;
        }
        if (m_snapshots.size() >= m_snapshots.capacity()) {
            m_snapshots.erase(m_snapshots.begin());
        }
        m_snapshots.push_back(snap);
    }
    for (DeckState& st : m_state) {
        st.seekedThisCallback = false;
    }
    // Effect knobs (EQ, filter, effect units) reach the audio one buffer
    // after they're set, so they go one buffer early; the rest on time.
    const Nanos due = contentStart + static_cast<Nanos>(bufferSec * 0.5e9);
    const Nanos dueEffects = due + static_cast<Nanos>(bufferSec * 1e9);
    for (size_t i = 0; i < m_pending.size();) {
        const ControlEvent& next = m_pending[i];
        if (next.time >= dueEffects) {
            break;
        }
        if (next.time < due || isEffectKey(next.key.view())) {
            const ControlEvent ev = next;
            m_pending.erase(m_pending.begin() + static_cast<long>(i));
            applyTogether(ev, contentStart, trueStart);
        } else {
            ++i;
        }
    }

    const uint32_t generation = m_session.peerGeneration();
    const bool sendAll = generation != m_seenPeerGeneration;
    m_seenPeerGeneration = generation;

    // 2. Our decks: jumps, play/pause and snapshots, at their own moment.
    for (int d = 0; d < numDecks; ++d) {
        if (owns(d)) {
            sendDeckPosition(d, ownContent, sendAll, trueStart);
        }
    }

    // 3. Controls.
    for (Watched& w : m_watched) {
        const double v = w.control->get();
        if (w.deck >= 0 && !owns(w.deck)) {
            // The friend's deck. A change here goes to them as deck prep if
            // their deck is stopped (they accept it if it still is), and is
            // put back otherwise. Play/pause is left to the snapshot check.
            if (v == w.last || w.isPlay) {
                continue;
            }
            if (w.isMixer) {
                // Its mixer row is anyone's: heard `lead` from now, here
                // (the fader and trim timed here, the EQ and filter by the
                // effects engine: mixDueHook) and on the friend's side.
                const int d = w.deck;
                if (w.key.view().find(",volume") != std::string_view::npos) {
                    if (m_volume[d].count == 0) {
                        m_volume[d].current = w.last;
                    }
                    m_volume[d].set(ownContent, v, contentStart);
                } else if (w.key.view().find(",pregain") != std::string_view::npos) {
                    if (m_pregain[d].count == 0) {
                        m_pregain[d].current = w.last;
                    }
                    m_pregain[d].set(ownContent, v, contentStart);
                }
                sendEvent(ownContent, w.key, toWire(w, v), "mix", trueStart);
                w.last = v;
                w.lastTouch = contentStart;
                continue;
            }
            if (w.displayOnly) {
                setSyncDisplay(w, w.last);
                m_overrides.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!livePlaying(w.deck)) {
                sendEvent(contentStart, w.key, toWire(w, v), "prep", trueStart);
                w.last = v;
                w.lastPrep = contentStart;
                m_prepChanges.fetch_add(1, std::memory_order_relaxed);
            } else {
                w.control->set(w.last, nullptr);
                m_overrides.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (v != w.last || (sendAll && w.isPlay)) {
            Nanos started = 0;
            if (w.isPlay && w.deck >= 0) {
                DeckState& st = m_state[w.deck];
                if (st.startPending || (v > 0.5 && !st.prevPlaying)) {
                    // Goes out with where it started, once the engine has
                    // started it (also if it was pressed just now).
                    continue;
                }
                started = st.startStamp;
                st.startStamp = 0;
            }
            w.last = v;
            // Our decks and the crossfader are heard `lead` from now: say so,
            // and the friend hears the change at that same moment.
            const bool timed = w.deck >= 0 || w.isCrossfader;
            const Nanos stamp = started > 0 ? started : timed ? ownContent : contentStart;
            sendEvent(stamp, w.key, toWire(w, v), nullptr, trueStart);
            w.localStamp = stamp;
            if (w.isCrossfader) {
                m_crossfader.set(stamp, v, contentStart);
            }
        }
    }

    // 4. Now and then everything we own again (the host: shared controls too).
    if (sendAll || contentStart - m_lastRefresh >= kRefreshInterval) {
        m_lastRefresh = contentStart;
        m_refreshCursor = 0;
    }
    const bool host = m_session.role() == ::b2b::Role::Leader;
    for (int budget = kRefreshPerCallback; budget > 0 && m_refreshCursor < m_watched.size(); ++m_refreshCursor) {
        Watched& w = m_watched[m_refreshCursor];
        const bool mine = w.deck >= 0 ? owns(w.deck) : host;
        if (mine && !w.isPlay) {
            sendEvent(w.deck >= 0 ? ownContent : contentStart, w.key, toWire(w, w.last), "state", trueStart);
            --budget;
        }
    }

    // 5. The friend's decks: where they should be.
    correctFromSnapshots(contentStart, trueStart);

    // 6. Our decks' faders as heard, and the delay itself.
    for (int d = 0; d < numDecks; ++d) {
        if (owns(d) && m_decks[d].volume) {
            const double v = m_decks[d].volume->get();
            if (v != m_volume[d].lastSet) {
                m_volume[d].lastSet = v;
                m_volume[d].set(ownContent, v, contentStart);
            }
        }
    }
    m_lastTrueStart = trueStart;
    setTiming(contentStart, bufferSec);
}

void EngineAdapter::applyTogether(const ControlEvent& ev, Nanos contentStart, Nanos trueStart) {
    const std::string_view key = ev.key.view();
    if (key == kHandoverKey.view()) {
        return;
    }
    const Nanos late = contentStart - ev.time;
    // (Track loads and messages from the main thread aren't timed to the beat.)
    if (late > 5 * ::b2b::kNanosPerMs && ev.text != "state" && ev.text != "prep" && ev.seq < 0x80000000u) {
        m_lateEvents.fetch_add(1, std::memory_order_relaxed);
        m_lastLateMs.store(::b2b::toMs(late), std::memory_order_relaxed);
    }
    const int d = deckForKey(key);
    if (d >= 0 && owns(d)) {
        // Our deck: only deck prep from the friend (they touched it while
        // it was stopped), and their moves on its mixer row (anyone's): set
        // as if we'd moved it (so it goes back to them as our own move, at
        // the moment it's heard).
        if (ev.text == "mix") {
            if (Watched* w = findWatched(key); w && w->isMixer) {
                w->control->set(fromWire(*w, ev.value), nullptr);
            }
        } else if (ev.text == "prep" || endsWith(key, ",b2b_prep_load")) {
            applyPrep(ev);
        }
        return;
    }
    if (d >= 0) {
        // The friend's deck. Our own recent moves on its mixer row win
        // over what comes back (it's our move echoed, or theirs at the same
        // time).
        Watched* w = findWatched(key);
        if (w && w->isMixer) {
            if (w->lastTouch != std::numeric_limits<Nanos>::min() &&
                    contentStart - w->lastTouch < 2 * m_lead + kMixTouchHold) {
                return;
            }
            m_remoteMixAt[d].store(m_clock.now(), std::memory_order_release);
        }
        applyEvent(ev, contentStart, trueStart);
        return;
    }
    Watched* w = findWatched(key);
    if (!w) {
        return;
    }
    // Shared controls: the later change wins.
    if (ev.time < w->localStamp || (ev.text == "state" && contentStart - w->localStamp < kPrepGrace) ||
            (ev.text == "state" && ev.time < w->appliedStamp)) {
        return;
    }
    if (ev.time > w->appliedStamp) {
        w->appliedStamp = ev.time;
    }
    const double value = fromWire(*w, ev.value);
    if (w->control->get() != value) {
        w->control->set(value, nullptr);
    }
    w->last = w->control->get();
    if (w->isCrossfader) {
        m_crossfader.set(ev.time, w->last, contentStart);
    }
    m_appliedEvents.fetch_add(1, std::memory_order_relaxed);
    if (m_log) {
        m_log->add({'E', static_cast<int64_t>(ev.seq), ev.text == "state" ? 11.0 : 10.0, -1, ev.value, trueStart,
                ::b2b::toMs(late)});
    }
}

void EngineAdapter::setTiming(Nanos contentStart, double bufferSec) {
    if (!m_pMixer) {
        return;
    }
    // Changes due within this buffer take effect in it (the friend's side
    // rounds the same way).
    const Nanos heardBy = contentStart + static_cast<Nanos>(bufferSec * 0.5e9);
    EngineMixer::B2BTiming& timing = m_pMixer->b2bTiming();
    const double sampleRate = m_engineSampleRate ? m_engineSampleRate->get() : 0.0;
    const int leadFrames = static_cast<int>(std::lround(static_cast<double>(m_lead) / 1e9 * sampleRate));
    for (size_t d = 0; d < m_decks.size() && d < static_cast<size_t>(EngineMixer::B2BTiming::kDecks); ++d) {
        const bool mine = owns(static_cast<int>(d));
        // Our deck, while it plays (stopped, it's silent at once: see
        // EngineMixer::applyB2BTiming).
        timing.delayFrames[d] = mine && deckPlaying(static_cast<int>(d)) ? leadFrames : 0;
        if (mine) {
            timing.volume[d] = static_cast<CSAMPLE_GAIN>(m_volume[d].at(heardBy));
        } else {
            // The friend's deck: as it is, unless we moved its fader a
            // moment ago (heard when it's due).
            const double v = m_volume[d].count > 0 ? m_volume[d].at(contentStart) : -1.0;
            timing.volume[d] = m_volume[d].count > 0 ? static_cast<CSAMPLE_GAIN>(v) : -1.0f;
        }
        if (m_decks[d].buffer) {
            m_decks[d].buffer->setB2BLeadSeconds(mine ? 0.0 : static_cast<double>(m_lead) / 1e9);
            m_decks[d].buffer->setB2BFollowed(!mine);
            m_decks[d].buffer->setB2BHeardDelaySeconds(
                    mine && deckPlaying(static_cast<int>(d)) ? static_cast<double>(m_lead) / 1e9 : 0.0);
            const double trim = !mine && m_pregain[d].count > 0 ? m_pregain[d].at(contentStart) : -1.0;
            m_decks[d].buffer->setB2BPregain(!mine && m_pregain[d].count > 0 ? trim : -1.0);
        }
    }
    const double xf = m_crossfaderWatched >= 0 ? m_crossfader.at(heardBy)
                                               : std::numeric_limits<double>::quiet_NaN();
    if (m_log && xf != timing.crossfader && !(std::isnan(xf) && std::isnan(timing.crossfader))) {
        m_log->add({'X', 0, 0.0, -1, xf, m_lastTrueStart, static_cast<double>(contentStart)});
    }
    timing.crossfader = xf;
    m_timingSet = true;
}

void EngineAdapter::clearTiming() {
    // Main thread, the hook is gone: the mixer resets itself next callback.
    mixtiming::g_contentNow.store(0, std::memory_order_release);
    for (const DeckRefs& refs : m_decks) {
        if (refs.buffer) {
            refs.buffer->setB2BPregain(-1.0);
        }
    }
    if (m_pMixer && m_timingSet) {
        m_pMixer->resetB2BTiming();
    }
    for (DeckRefs& deck : m_decks) {
        if (deck.buffer) {
            deck.buffer->setB2BLeadSeconds(0);
        }
    }
    m_timingSet = false;
}

// ---------------------------------------------------------------------------
// Helpers

bool EngineAdapter::positionKnown(int deck) const {
    // While a track loads, its length (and so its position) isn't known yet.
    return m_decks[deck].trackSampleRate->get() > 0 && m_decks[deck].buffer->getTrackEndPosition().isValid();
}

double EngineAdapter::positionSec(int deck) const {
    // One reading of the track's length (it changes while a track loads).
    const double sr = m_decks[deck].trackSampleRate->get();
    const mixxx::audio::FramePos end = m_decks[deck].buffer->getTrackEndPosition();
    if (sr <= 0 || !end.isValid()) {
        return 0.0;
    }
    return end.value() * m_decks[deck].buffer->getVisualPlayPos() / sr;
}

double EngineAdapter::speed(int deck) const {
    return m_decks[deck].buffer->getSpeed();
}

bool EngineAdapter::deckPlaying(int deck) const {
    return m_decks[deck].play->get() > 0.5;
}

bool EngineAdapter::livePlaying(int deck) const {
    const int i = deck >= 0 && deck < kMaxDecks ? m_playWatched[deck] : -1;
    if (i >= 0 && m_watched[i].fromLive) {
        return m_watched[i].last > 0.5;
    }
    return deckPlaying(deck);
}

void EngineAdapter::seekTo(int deck, double positionSec) {
    const double sr = m_decks[deck].trackSampleRate->get();
    if (sr <= 0 || !positionKnown(deck)) {
        return;
    }
    // Before the start is fine: a deck started a moment before the track's
    // first beat (quantize does that) plays silence up to it.
    m_decks[deck].buffer->seekExact(mixxx::audio::FramePos(positionSec * sr));
    m_state[deck].seekedThisCallback = true;
}

void EngineAdapter::logPositions(Nanos trueStart) {
    const int numDecks = static_cast<int>(m_decks.size());
    // Role and offset, so positions can be compared across handovers.
    m_log->add({'F', m_live ? 1 : 0, m_offset / 1e6, -1, m_catchUpProgress.load(std::memory_order_relaxed),
            trueStart, 0.0});
    for (int d = 0; d < numDecks; ++d) {
        if (m_deckTrackId[d].load(std::memory_order_relaxed) == 0) {
            continue;
        }
        const bool playing = deckPlaying(d);
        // What is heard now: our own decks in "a deck each" play `lead` ahead.
        double heard = positionSec(d);
        if (m_mode == Mode::DeckEach && owns(d) && playing) {
            heard -= static_cast<double>(m_lead) / 1e9 * speed(d);
        }
        // extra: speed; value: playing
        m_log->add({'P', d, heard, d, playing ? 1.0 : 0.0, trueStart, speed(d)});
    }
}

namespace {
std::atomic<EngineAdapter*> s_activeAdapter{nullptr};
} // namespace

void EngineAdapter::setActive(EngineAdapter* pAdapter) {
    if (pAdapter) {
        s_activeAdapter.store(pAdapter, std::memory_order_release);
        mixtiming::g_due.store(&EngineAdapter::mixDueHook, std::memory_order_release);
    } else {
        mixtiming::g_due.store(nullptr, std::memory_order_release);
        s_activeAdapter.store(nullptr, std::memory_order_release);
    }
}

int64_t EngineAdapter::mixDueHook(const QString& group, const QString& item) {
    Q_UNUSED(item);
    EngineAdapter* pAdapter = s_activeAdapter.load(std::memory_order_acquire);
    if (!pAdapter || pAdapter->mode() != Mode::DeckEach) {
        return 0;
    }
    // Only a deck's mixer row: its EQ and its quick effect (the filter).
    if (!group.startsWith(QLatin1String("[EqualizerRack1_")) &&
            !group.startsWith(QLatin1String("[QuickEffectRack1_"))) {
        return 0;
    }
    const QByteArray utf8 = group.toUtf8();
    const int d = pAdapter->deckForKey(std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
    if (d < 0 || pAdapter->ownsDeck(d)) {
        return 0; // our own deck: heard `lead` later anyway (its delay comes after the EQ)
    }
    // The friend's move, just applied here: at once (it's on time already).
    const int64_t remoteAt = pAdapter->m_remoteMixAt[d].load(std::memory_order_acquire);
    if (remoteAt != 0 && pAdapter->m_clock.now() - remoteAt < kRemoteMixWindow) {
        return 0;
    }
    const Nanos content = pAdapter->m_contentShared.load(std::memory_order_acquire);
    if (content == 0) {
        return 0;
    }
    return content + pAdapter->m_leadShared.load(std::memory_order_acquire);
}

int EngineAdapter::deckForKey(std::string_view key) const {
    const int numDecks = static_cast<int>(m_decks.size());
    for (int d = 0; d < numDecks; ++d) {
        if (key.find(m_groupKeys[d].view()) != std::string_view::npos) {
            return d;
        }
    }
    return -1;
}

EngineAdapter::Watched* EngineAdapter::findWatched(std::string_view key) {
    for (Watched& w : m_watched) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

double EngineAdapter::toWire(const Watched& w, double value) const {
    if (!w.isSamplePos || w.deck < 0 || value < 0) {
        return value;
    }
    const double rate = m_decks[w.deck].trackSampleRate ? m_decks[w.deck].trackSampleRate->get() : 0.0;
    return rate > 0 ? value / (2.0 * rate) : -1.0; // samples are stereo: two per frame
}

double EngineAdapter::fromWire(const Watched& w, double value) const {
    if (!w.isSamplePos || w.deck < 0 || value < 0) {
        return value;
    }
    const double rate = m_decks[w.deck].trackSampleRate ? m_decks[w.deck].trackSampleRate->get() : 0.0;
    return rate > 0 ? 2.0 * std::round(value * rate) : -1.0; // whole frames
}

} // namespace mixxx::b2b
