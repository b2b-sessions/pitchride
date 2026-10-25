#pragma once

#include <QSharedPointer>
#include <QString>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include "b2b/event_log.hpp"
#include "b2b/session.hpp"
#include "b2b/spsc_queue.hpp"
#include "b2b/time.hpp"

class ControlDoublePrivate;
class ControlObject;
class EngineBuffer;
class EngineMixer;

namespace mixxx::b2b {

/// The part of the B2B adapter that runs on Mixxx's audio thread, once per
/// audio callback, before any deck is processed.
///
/// One of the two peers is *live* at a time:
///
/// Live:     watches mixer/deck controls and deck positions, and sends every
///           change stamped with the moment it is heard ("content time").
///           Accepts deck prep (loads, settings on stopped decks) from the
///           other side.
/// Listener: plays `offset` behind its live peer and applies each change in
///           the callback where it belongs, then checks deck positions
///           against the live side's snapshots and corrects any drift.
///
/// Content time = shared clock - our offset. The live side keeps the offset it
/// had while listening, so its music carries on seamlessly when it takes
/// over. The side that hands over must fall behind instead: it plays slightly
/// slower (key lock on) for a few bars until it is far enough behind, or, if
/// configured, jumps back a whole number of beats on a beat.
///
/// "A deck each" (Mode::DeckEach) works differently: both peers play the same
/// content at the same moment (one offset), each owns some decks, and each
/// hears its *own* decks `lead` after it plays them (a delay line in the
/// mixer, after the deck's EQ and filter; see EngineMixer::B2BTiming). A move
/// on an own deck is stamped with the moment it will be heard, so it reaches
/// the friend before then and lands at the same moment on both. The friend's
/// decks are followed like a listener follows, without a listening delay.
///
/// Everything here is lock-free and allocation-free (except optional
/// test logging).
class EngineAdapter {
  public:
    static constexpr int kMaxDecks = 4;
    static constexpr uint32_t kErrHistory = 8;

    struct DeckRefs {
        QString group;                         // "[Channel1]"
        EngineBuffer* buffer = nullptr;        // position, speed, exact seeks
        QSharedPointer<ControlDoublePrivate> play;
        QSharedPointer<ControlDoublePrivate> wheel;     // catch-up trim (additive rate)
        QSharedPointer<ControlDoublePrivate> keylock;
        ControlObject* trackSampleRate = nullptr;
        ControlObject* rateRatio = nullptr;
        ControlObject* bpm = nullptr;          // current (tempo-adjusted) BPM
        ControlObject* beatDistance = nullptr; // 0..1 through the current beat
        ControlObject* volume = nullptr;
        ControlObject* pregain = nullptr;
    };

    /// Mirrored controls are written through the shared control value with no
    /// sender, exactly like a skin widget or controller would. (Writing via the
    /// owning ControlObject would skip its own change handlers: e.g. the tempo
    /// slider would move but the tempo would not be recalculated.)
    ///
    /// Exception: `displayOnly` controls (the sync button) are shown on the
    /// listener without engaging them, so Mixxx's own sync cannot move decks
    /// that the live side drives. They engage for real when we go live.
    struct Watched {
        QSharedPointer<ControlDoublePrivate> control;
        ControlObject* displayOnly = nullptr;
        ::b2b::FixedString<64> key; // "[Channel1],volume"
        int deck = -1;              // index into decks, or -1 for global controls
        double last = 0;
        bool fromLive = false;      // listener: value has been set by the live side
        bool isPlay = false;
        bool isKeylock = false;
        bool isSamplePos = false;   // a track position in samples (loop points): sent in seconds
        bool isCrossfader = false;  // DeckEach: heard `lead` after it moves, like our own decks
        bool isMixer = false;       // DeckEach: a deck's mixer row (trim, EQ, filter, fader): anyone's
        ::b2b::Nanos lastTouch = std::numeric_limits<::b2b::Nanos>::min(); // DeckEach: we last moved it (friend's deck)
        ::b2b::Nanos lastPrep = 0; // listener: when we last prepared this control
        ::b2b::Nanos localStamp = 0; // DeckEach, shared controls: our last change (later ones win)
        ::b2b::Nanos appliedStamp = std::numeric_limits<::b2b::Nanos>::min(); // newest change applied from the peer
    };

    /// A track to load (sent to the main thread). Listener: the live side
    /// loaded it. Live: the listener prepared it on a stopped deck.
    struct LoadRequest {
        int deck = 0;
        uint64_t trackId = 0;
    };

    enum class HandoverStyle { Smooth = 0, Snap = 1 };
    enum class Mode { TakeTurns = 0, DeckEach = 1 };

    /// Value as sent to the peer / as stored locally. Loop points are sample
    /// positions, which differ between copies of a track at different sample
    /// rates, so they travel in seconds (-1 = no loop point).
    double toWire(const Watched& w, double value) const;
    double fromWire(const Watched& w, double value) const;

    /// Audio thread -> main thread.
    struct Notice {
        enum Kind { BecameLive, BecameListener, CatchUpDone, ModeChanged } kind = BecameLive;
        double shiftMs = 0;    // BecameListener: how far we must fall back
        double durationSec = 0; // BecameListener: how long the catch-up takes (0 = instant)
    };

    EngineAdapter(::b2b::Session& session,
            const ::b2b::LocalClock& clock,
            ::b2b::EventLog* log,
            std::vector<DeckRefs> decks,
            std::vector<Watched> watched,
            ControlObject* engineSampleRate,
            ControlObject* outputLatencyMs,
            double correctThresholdMs,
            bool startLive,
            ::b2b::Nanos startOffset);

    /// Audio thread: called at the start of every callback.
    void process(int frames);

    // ---- Main thread ----

    /// The fingerprint of the track now loaded on a deck (0 = none).
    void setDeckTrackId(int deck, uint64_t id) {
        m_deckTrackId[deck].store(id, std::memory_order_release);
    }
    bool popLoadRequest(LoadRequest* request) {
        return m_loadRequests.pop(*request);
    }
    bool popNotice(Notice* notice) {
        return m_notices.pop(*notice);
    }

    /// Live: pass control to the peer (smooth: now; snap: on the next beat).
    void requestHandover() {
        m_handoverRequested.store(true, std::memory_order_release);
    }
    /// Peer reconnected: the host is live again, everything starts afresh.
    void requestReset(bool live) {
        m_resetLive.store(live ? 1 : 0, std::memory_order_release);
    }
    bool isLive() const {
        return m_liveShared.load(std::memory_order_acquire);
    }
    /// How far behind the live side we want to listen (measured from the network).
    void setListenDelay(::b2b::Nanos delay) {
        m_listenDelay.store(delay, std::memory_order_release);
    }
    /// The peer's content offset (it tells us whenever it changes), and
    /// whether it was live when it said so. A listener only follows the
    /// offset of a *live* peer; the live side needs the listener's offset
    /// when it hands over.
    void setPeerOffset(::b2b::Nanos offset, bool peerLive) {
        m_peerOffset.store(offset, std::memory_order_release);
        m_peerOffsetFromLive.store(peerLive, std::memory_order_release);
    }
    ::b2b::Nanos offset() const {
        return m_offsetShared.load(std::memory_order_acquire);
    }
    void setCatchUpBars(double bars) {
        m_catchUpBars.store(bars, std::memory_order_release);
    }
    void setHandoverStyle(HandoverStyle style) {
        m_style.store(static_cast<int>(style), std::memory_order_release);
    }
    // ---- A deck each ----

    /// The mixer whose decks we delay (before the hook is installed).
    void setMixer(EngineMixer* pMixer) {
        m_pMixer = pMixer;
    }
    /// Switch modes (applied on the next callback). DeckEach: both peers use
    /// `offset` (the host's); TakeTurns: back to "the host is live", like a
    /// reconnect.
    void requestMode(Mode mode, ::b2b::Nanos offset) {
        m_requestedOffset.store(offset, std::memory_order_release);
        m_requestedMode.store(static_cast<int>(mode), std::memory_order_release);
    }
    Mode mode() const {
        return static_cast<Mode>(m_modeShared.load(std::memory_order_acquire));
    }
    /// Which decks are ours in DeckEach (bit d = deck d).
    void setOwnedDecks(uint32_t mask) {
        m_ownedMask.store(mask, std::memory_order_release);
    }
    /// The same, from a moment on the content timeline (both apps switch
    /// together when a deck changes hands).
    void setOwnedDecksAt(uint32_t mask, ::b2b::Nanos contentTime) {
        m_switchTime.store(contentTime, std::memory_order_release);
        m_switchMask.store(static_cast<int64_t>(mask), std::memory_order_release);
    }
    uint32_t ownedDecks() const {
        return m_ownedMask.load(std::memory_order_acquire);
    }
    /// Main thread: a play on the friend's deck (while theirs is stopped)
    /// was stopped since the last call.
    bool takeBlockedPlay(int deck) {
        return deck >= 0 && deck < kMaxDecks && m_blockedPlay[deck].exchange(false, std::memory_order_acq_rel);
    }
    bool ownsDeck(int deck) const {
        return deck >= 0 && (m_ownedMask.load(std::memory_order_acquire) >> deck) & 1u;
    }
    /// How long after playing them we hear our own decks in DeckEach.
    /// Changes wait until none of our decks is playing.
    void setLead(::b2b::Nanos lead) {
        m_requestedLead.store(lead, std::memory_order_release);
    }
    ::b2b::Nanos lead() const {
        return m_leadShared.load(std::memory_order_acquire);
    }
    /// Our own decks playing (main thread: whether the lead can change now).
    bool anyOwnDeckPlaying() const {
        return m_ownPlaying.load(std::memory_order_acquire);
    }
    /// Events from the friend that arrived after their moment (DeckEach).
    uint64_t lateEvents() const {
        return m_lateEvents.load(std::memory_order_relaxed);
    }
    /// The latest of them, in ms (DeckEach).
    double lastLateMs() const {
        return m_lastLateMs.load(std::memory_order_relaxed);
    }

    /// Main thread, after the adapter has been unhooked from the engine:
    /// put back anything a catch-up changed (deck trim, key lock).
    void finish() {
        if (m_slew.active) {
            endCatchUp();
        }
        clearTiming();
        setActive(nullptr);
    }

    /// "A deck each": the mixer row is anyone's. The session's adapter (for
    /// mixDueHook); nullptr: none.
    static void setActive(EngineAdapter* pAdapter);
    /// Main thread (mixtiming::g_due): when a change to this effect control
    /// is due, or 0 for at once. A move we make on the friend's channel is
    /// due `lead` from now, when the friend hears it too.
    static int64_t mixDueHook(const QString& group, const QString& item);

    /// 0..1 while catching up after a handover, -1 otherwise.
    double catchUpProgress() const {
        return m_catchUpProgress.load(std::memory_order_relaxed);
    }

    uint64_t corrections() const {
        return m_corrections.load(std::memory_order_relaxed);
    }
    uint64_t appliedEvents() const {
        return m_appliedEvents.load(std::memory_order_relaxed);
    }
    uint64_t outsidePlayChanges() const {
        return m_outsidePlayChanges.load(std::memory_order_relaxed);
    }
    uint64_t overrides() const {
        return m_overrides.load(std::memory_order_relaxed);
    }
    /// Listener: differences healed by the live side's periodic state refresh.
    uint64_t resyncs() const {
        return m_resyncs.load(std::memory_order_relaxed);
    }
    uint64_t prepChanges() const {
        return m_prepChanges.load(std::memory_order_relaxed);
    }

  private:
    struct DeckState {
        // Live: jump detection.
        bool havePrev = false;
        double prevPosSec = 0;
        ::b2b::Nanos prevShared = 0;
        bool prevPlaying = false;
        double prevSpeed = 0; // our deck's speed last callback
        ::b2b::Nanos lastSnapshot = 0;
        // "A deck each", our deck just started: where it really starts is only
        // known once the engine has started it (quantize may move it), so its
        // start goes out one callback later, stamped with the moment it began.
        bool startPending = false;
        ::b2b::Nanos startStamp = 0; // > 0: send play with this stamp
        // Listener: snapshot correction.
        uint64_t liveTrackId = 0;
        ::b2b::Nanos lastPositionEvent = 0; // time of last applied play/seek event
        ::b2b::Nanos lastSnapshotUsed = 0;
        bool seekedThisCallback = false;
        std::array<double, 8> errHistory{};
        double lastPlay = -1; // listener: play value we left at the end of last callback
        uint32_t errCount = 0;
    };

    // Catch-up: our offset moves smoothly from `from` to `to`.
    struct Slew {
        bool active = false;
        ::b2b::Nanos startLocal = 0;
        double durationNs = 0;
        double from = 0;
        double to = 0;
    };

    ::b2b::Nanos bufferLocalTime(int frames, double sampleRate);
    void processLive(::b2b::Nanos contentStart, ::b2b::Nanos trueStart, double bufferSec);
    void processListener(::b2b::Nanos contentStart, ::b2b::Nanos trueStart, double bufferSec);
    void sendEvent(::b2b::Nanos time, const ::b2b::FixedString<64>& key,
            double value, const char* text, ::b2b::Nanos trueStart);
    void applyEvent(const ::b2b::ControlEvent& ev, ::b2b::Nanos contentStart, ::b2b::Nanos trueStart);
    void applyPrep(const ::b2b::ControlEvent& ev);
    void correctFromSnapshots(::b2b::Nanos contentStart, ::b2b::Nanos trueStart);

    // A value that changes at given moments on the content timeline.
    struct Scheduled {
        static constexpr int kSize = 64;
        double current = 0;
        double lastSet = std::numeric_limits<double>::quiet_NaN(); // last value given (our own changes)
        int count = 0;
        std::array<::b2b::Nanos, kSize> times{};
        std::array<double, kSize> values{};
        void reset(double value) {
            current = value;
            lastSet = value;
            count = 0;
        }
        void set(::b2b::Nanos time, double value, ::b2b::Nanos now);
        double at(::b2b::Nanos now);
    };

    void switchMode(Mode mode, ::b2b::Nanos contentStart, ::b2b::Nanos local, ::b2b::Nanos trueStart);
    void processTogether(::b2b::Nanos contentStart, ::b2b::Nanos trueStart, double bufferSec);
    void applyTogether(const ::b2b::ControlEvent& ev, ::b2b::Nanos contentStart, ::b2b::Nanos trueStart);
    void sendDeckPosition(int deck, ::b2b::Nanos deckContent, bool sendAll, ::b2b::Nanos trueStart);
    void setTiming(::b2b::Nanos contentStart, double bufferSec);
    void clearTiming();
    void shiftOwnDecks(double seconds);
    bool owns(int deck) const {
        return deck >= 0 && (m_owned >> deck) & 1u;
    }

    void becomeLive(::b2b::Nanos contentStart, ::b2b::Nanos trueStart);
    void becomeListener(::b2b::Nanos contentStart, ::b2b::Nanos local, ::b2b::Nanos trueStart);
    void retarget(::b2b::Nanos local, double target, bool forceInstant);
    void startCatchUp(::b2b::Nanos local, double target, bool instant);
    void stepCatchUp(::b2b::Nanos local);
    void endCatchUp();
    void jumpOffset(double newOffset);
    int referenceDeck() const;
    double beatLengthSec(int deck) const;
    void setSyncDisplay(Watched& w, double v);
    void resetDeckStates();

    double positionSec(int deck) const;
    bool positionKnown(int deck) const;
    double speed(int deck) const;
    bool deckPlaying(int deck) const;
    /// Listener: whether the live side has this deck playing (its play
    /// control as last received), falling back to our own deck.
    bool livePlaying(int deck) const;
    void seekTo(int deck, double positionSec);
    void logPositions(::b2b::Nanos trueStart);
    int deckForKey(std::string_view key) const;
    Watched* findWatched(std::string_view key);

    ::b2b::Session& m_session;
    const ::b2b::LocalClock& m_clock;
    ::b2b::EventLog* m_log;
    std::vector<DeckRefs> m_decks;
    std::vector<Watched> m_watched;
    ControlObject* m_engineSampleRate;
    ControlObject* m_outputLatencyMs; // [App],output_latency_ms: callback -> speaker
    double m_correctThresholdSec;

    // Audio clock: sample counting anchored loosely to the system clock.
    bool m_timingStarted = false;
    ::b2b::Nanos m_anchor = 0;
    int64_t m_framesRendered = 0;
    double m_lastSampleRate = 0;

    // Roles
    bool m_live = false;
    double m_offset = 0; // ns: content time = shared time - m_offset
    Slew m_slew;
    bool m_waitingForBeat = false; // snap handover: waiting for the next beat
    double m_lastBeatDistance = -1;
    ::b2b::Nanos m_beatWaitStart = 0;
    uint32_t m_epoch = 0;
    std::atomic<bool> m_liveShared{false};
    std::atomic<::b2b::Nanos> m_offsetShared{0};
    std::atomic<bool> m_handoverRequested{false};
    std::atomic<int> m_resetLive{-1};
    std::atomic<::b2b::Nanos> m_listenDelay{0};
    std::atomic<::b2b::Nanos> m_peerOffset{0};
    std::atomic<bool> m_peerOffsetFromLive{true};
    ::b2b::Nanos m_appliedPeerOffset = -1;
    ::b2b::Nanos m_appliedListenDelay = -1;
    std::atomic<double> m_catchUpBars{4.0};
    std::atomic<int> m_style{0};
    std::atomic<double> m_catchUpProgress{-1.0};

    // Live
    uint32_t m_nextSeq = 1;
    uint32_t m_seenPeerGeneration = 0;
    ::b2b::Nanos m_lastRefresh = 0;
    size_t m_refreshCursor = SIZE_MAX; // next control to repeat in the current refresh pass

    // A deck each
    EngineMixer* m_pMixer = nullptr;
    Mode m_mode = Mode::TakeTurns;
    std::atomic<int> m_modeShared{0};
    std::atomic<int> m_requestedMode{-1};
    std::atomic<::b2b::Nanos> m_requestedOffset{0};
    std::atomic<uint32_t> m_ownedMask{0};
    std::atomic<int64_t> m_switchMask{-1}; // -1: no change planned
    std::atomic<::b2b::Nanos> m_switchTime{0};
    uint32_t m_owned = 0;
    std::atomic<::b2b::Nanos> m_requestedLead{-1}; // < 0: not set yet
    ::b2b::Nanos m_lead = 0;
    std::atomic<::b2b::Nanos> m_leadShared{0};
    std::atomic<bool> m_ownPlaying{false};
    std::array<Scheduled, kMaxDecks> m_volume{};   // our decks' faders as heard (and our moves on the friend's)
    std::array<Scheduled, kMaxDecks> m_pregain{};  // our moves on the friend's decks' trim, as heard
    std::array<std::atomic<int64_t>, kMaxDecks> m_remoteMixAt{}; // the friend's mixer move on one of their decks was applied (local clock)
    std::atomic<::b2b::Nanos> m_contentShared{0};
    Scheduled m_crossfader;
    int m_crossfaderWatched = -1;
    std::atomic<uint64_t> m_lateEvents{0};
    std::atomic<double> m_lastLateMs{0};
    bool m_timingSet = false;
    ::b2b::Nanos m_lastTrueStart = 0;

    // Listener
    std::vector<::b2b::ControlEvent> m_pending;  // sorted by time
    std::vector<::b2b::DeckSnapshot> m_snapshots; // received, newest last

    std::array<DeckState, kMaxDecks> m_state{};
    std::array<::b2b::FixedString<32>, kMaxDecks> m_groupKeys; // "[Channel1]"
    std::array<::b2b::FixedString<64>, kMaxDecks> m_posKeys;   // "[Channel1],b2b_pos"
    std::array<int, kMaxDecks> m_playWatched{};                 // index into m_watched, -1 = none
    std::array<std::atomic<uint64_t>, kMaxDecks> m_deckTrackId{};
    std::array<std::atomic<bool>, kMaxDecks> m_blockedPlay{};
    ::b2b::SpscQueue<LoadRequest, 64> m_loadRequests;
    ::b2b::SpscQueue<Notice, 16> m_notices;

    std::atomic<uint64_t> m_corrections{0};
    std::atomic<uint64_t> m_appliedEvents{0};
    std::atomic<uint64_t> m_overrides{0};
    std::atomic<uint64_t> m_outsidePlayChanges{0};
    std::atomic<uint64_t> m_prepChanges{0};
    std::atomic<uint64_t> m_resyncs{0};
    uint64_t m_callbacks = 0;
};

} // namespace mixxx::b2b
