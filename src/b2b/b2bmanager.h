#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "b2b/b2bengineadapter.h"
#include "b2b/qt/session_ui.hpp"
#include "engine/enginemixer.h"
#include "b2b/event_log.hpp"
#include "b2b/file_share.hpp"
#include "b2b/session.hpp"
#include "b2b/time.hpp"
#include "track/track_decl.h"

class EffectsManager;
class ControlObject;
class PlayerManager;
class TrackCollectionManager;

namespace mixxx::b2b {

class Autopilot;

/// Online back-to-back DJing: connects this Mixxx to another one so both play
/// the same mix, and lets the two DJs take turns being live. Lives on the
/// main thread; the time-critical work happens in EngineAdapter on the audio
/// thread.
///
/// Configured with environment variables (see README-B2B.md):
///   B2B_ROLE=host|guest (or leader|follower)   enables the feature
///   B2B_PORT, B2B_PEER=host:port, B2B_MUSIC_DIR, B2B_DELAY_MS, B2B_LOG, ...
class Manager : public QObject, public EnginePreProcessHook {
    Q_OBJECT
  public:
    struct Config;

    /// True if B2B_ROLE is set (start a session straight away).
    static bool enabledByEnvironment();
    /// Settings from environment variables (role, peer and testing options).
    static Config configFromEnvironment();

    /// Starts a session: hosting or joining as `config` says. Ends when destroyed.
    Manager(const Config& config,
            PlayerManager* pPlayerManager,
            EngineMixer* pEngineMixer,
            std::shared_ptr<EffectsManager> pEffectsManager,
            const QString& settingsPath,
            ::b2b::qt::SessionUi* pUi,
            QObject* pParent = nullptr);
    ~Manager() override;

    /// Called once the library exists: lets us remove borrowed (cached)
    /// tracks from the library again.
    void setTrackCollectionManager(std::shared_ptr<TrackCollectionManager> pTrackCollectionManager);

    /// False if the network could not be set up (port in use, unknown address).
    bool started() const {
        return m_started;
    }

    /// Audio thread (EnginePreProcessHook).
    void preProcess(int frames) override {
        m_adapter->process(frames);
    }

    /// Your name (the site's), told to the friend; theirs as they told us.
    void setMyName(const QString& name);
    QString friendName() const {
        return m_friendName;
    }
    /// Who's on this deck: a deck each, its owner; taking turns, whoever is
    /// live. Empty: not known.
    QString deckPlayerName(int deck) const;
    /// Whether that's you (a deck each: yours; taking turns: you're live).
    bool deckIsMine(int deck) const;

    /// A deck each: what to show next to each deck's title.
    struct DeckOwner {
        bool shown = false;      // playing a deck each, with a friend
        bool mine = false;       // this deck is ours
        bool friendAsks = false; // ours, and the friend asked for it
        bool asking = false;     // theirs, and we asked for it
    };
    DeckOwner deckOwner(int deck) const;
    /// Ask the friend for one of their decks.
    void askForDeck(int deck);
    /// You tried to play the friend's deck (it was stopped again): since the
    /// last call.
    bool takeBlockedPlay(int deck) {
        return m_adapter && m_adapter->takeBlockedPlay(deck);
    }
    /// Answer the friend's request for one of ours.
    void answerDeckAsk(int deck, bool give);
    /// Hand one of ours to the friend.
    void giveDeck(int deck);
    /// A deck each: the decks that are ours (bit d = deck d); 0 when taking
    /// turns. (Who owns what, not whether the friend is reachable right
    /// now: a moment's silence on the network changes nothing.)
    uint32_t ownedDeckMask() const {
        return deckEach() ? myDecks() : 0u;
    }
    /// Playing "a deck each" (not taking turns).
    bool playingDeckEach() const {
        return deckEach();
    }
    /// The friend has linked up at least once in this session.
    bool friendJoined() const;
    /// The host reset the room: tell the friend's app, which resets its
    /// copy of the room too (roomReset there).
    void sendRoomReset() {
        sendSessionMessage("reset", 1.0);
    }
    /// The host: decks 1 and 2 yours, 3 and 4 the friend's (split), or
    /// every deck yours (the friend asks for one with +). Both apps switch
    /// together, as when a deck is handed over.
    void setSplitDecks(bool split);

    struct Config {
        ::b2b::Role role = ::b2b::Role::Leader;
        uint16_t port = 9000;
        QString peerHost;
        uint16_t peerPort = 9000;
        // Join codes: the server ("host:port"), whether the host asks for a
        // code, and the code a guest joins with.
        QString server;
        bool useCode = false; // host
        QString joinCode;     // guest
        bool relayOnly = false; // testing: always use the server's relay
        QString password;       // by address: the session password (both sides)
        bool splitDecks = false; // the host, a deck each: decks 1-2 mine, 3-4 the friend's from the start
        double delayMs = 0; // > 0: fixed listening delay; 0: measure it
        QString musicDir;
        QString logPath;
        QString autopilot;
        double fakeOffsetMs = 0;
        double fakeDriftPpm = 0;
        double correctMs = 5;
        double logAfterSec = 0;  // testing: write the log this long after start
        double shareKBps = 12288; // upload cap for track sharing (through the relay: b2b-core's lower one)
        double shareMaxMB = 300; // biggest file we send or accept
        // Testing: hand over this long after going live / ask for control
        // this long after becoming the listener (0 = never).
        double autopilotHandoverSec = 0;
        double autopilotRequestSec = 0;
        ::b2b::NetSimConfig sim;
    };

    /// Our pointer on the skin (PointerShare packs it; -1: not on the
    /// skin), with our skin and deck view.
    void sendPointer(double packed, const QString& view) {
        sendSessionMessage("ptr", packed, view);
    }
    bool peerConnected() const;
    /// How we reach the friend now: "direct", "relay" (through the
    /// connection server), or "" (not linked). For the site's admins.
    QString linkKind() const;
    /// Our music browser's screen, for the friend to see (PeerBrowser).
    void sendBrowserView(double kind, const QString& text) {
        sendSessionMessage("bv", kind, text);
    }
    /// A setlist entry (JSON) for the friend's app: the host's app tells
    /// side b what's been played in the room (SetList).
    void sendSetEntry(const QByteArray& json);

  signals:
    /// The friend's pointer moved (see PointerShare).
    void pointerReceived(double packed, const QString& view);
    /// A piece of the friend's music browser screen (see PeerBrowser).
    void browserViewReceived(double kind, const QString& text);
    /// The session can't go on (e.g. a wrong join code). Connect queued:
    /// the receiver usually destroys this Manager.
    void failed(const QString& message);
    /// Who has which deck (or the play mode) changed: see deckOwner().
    void decksChanged();
    /// A setlist entry from the friend's app (JSON).
    void setEntryReceived(const QByteArray& json);
    /// The host reset the room (the guest's app: reset yours too).
    void roomReset();

  private slots:
    void slotTick();

  private:

    bool isHost() const {
        return m_config.role == ::b2b::Role::Leader;
    }
    bool codeMode() const {
        return isHost() ? m_config.useCode : !m_config.joinCode.isEmpty();
    }
    void tickRendezvous();
    bool isLive() const;
    ::b2b::Nanos contentNow() const;
    // A deck each
    bool deckEach() const {
        return m_adapter && m_adapter->mode() == EngineAdapter::Mode::DeckEach;
    }
    /// Whether we decide this deck: ours in "a deck each", all of them when live.
    bool controlsDeck(int deck) const;
    /// Whether we announce this effect chain ("U1", "Q2"...).
    bool announcesEffects(const QString& code) const;
    void setPlayMode(bool deckEach, ::b2b::Nanos offset);
    void setHostDecks(uint32_t hostMask, ::b2b::Nanos at, bool announce);
    uint32_t myDecks() const {
        return isHost() ? m_hostMask : (~m_hostMask & 0xFu);
    }
    void sendPlayMode();
    void updateLead();
    QString myDecksText() const;

    void trackLoaded(int deck, TrackPointer pTrack);
    void trackUnloaded(int deck);
    void sendLoad(int deck);
    void startMusicIndex();
    void tryPendingLoads();
    void startFileShare();
    void sendTrackList();
    void watchPreviewDecks();
    /// A track we have locally (shared folder, loaded by us, or received this session).
    QString localPathFor(uint64_t id);
    void shareTrack(const TrackPointer& pTrack, uint64_t id); // make one of ours shareable, with its details
    void applyPeerMeta(const TrackPointer& pTrack, uint64_t id);
    // Beat grids fixed by hand during a session reach the friend's copy too
    // ("[B2B],grid": the BPM, and the track and its first beat as text).
    void deckBeatsUpdated(int deck);
    void applyPeerGrid(double bpm, const QString& text);
    // A borrowed track's grid is locked (so our analyzer leaves the friend's
    // grid alone); the grid buttons still work on it.
    void gridEditPressed(int deck, ControlObject* pControl, double value);
    void onPeerHint(uint64_t id, uint8_t priority);
    void onReceived(uint64_t id, const QString& path);
    void onTransferFailed(uint64_t id, const QString& reason);
    void clearCache();
    void requestFromPeer(uint64_t id, uint8_t priority, const char* why);
    bool isInCache(const QString& path) const;

    // Taking turns
    void sendSessionMessage(const char* name, double value, const QString& text = QString());
    // Setlist entries, in pieces (a message's text is short): the pieces
    // of each, by its number, until its last.
    QHash<int, QByteArray> m_setParts;
    int m_setSerial = 0;
    void handleSessionMessages();
    void handleNotices();
    void updateListenDelay();
    void takeOrRequest();
    void handOver();
    void answerRequest(bool allow);
    void updateUi();
    void applyPrefs(const ::b2b::qt::HandoverPrefs& prefs);
    void runAutopilotTurns();
    // Which effects are loaded: the live side announces it, the listener loads the same.
    void syncEffects();
    void applyEffectMessage(bool preset, const QString& text);

    void printStatus();
    void writeLog();

    Config m_config;
    QString m_settingsPath;
    QString m_myName;
    QString m_friendName;
    QElapsedTimer m_sinceNameSent;
    PlayerManager* m_pPlayerManager;
    std::shared_ptr<EffectsManager> m_pEffectsManager;
    QHash<QString, QString> m_fxSent; // "U1/0" -> effect id we last announced
    QElapsedTimer m_sinceFxResend;
    EngineMixer* m_pEngineMixer;

    std::unique_ptr<::b2b::LocalClock> m_clock;
    std::unique_ptr<::b2b::Session> m_session;
    std::unique_ptr<::b2b::EventLog> m_log;
    std::unique_ptr<EngineAdapter> m_adapter;
    std::unique_ptr<Autopilot> m_autopilot;
    QPointer<::b2b::qt::SessionUi> m_pUi;
    bool m_started = false;
    bool m_starting = true; // constructor: decks already loaded aren't deck prep

    int m_numDecks = 0;
    std::vector<QString> m_deckGroups;
    std::array<uint64_t, EngineAdapter::kMaxDecks> m_deckTrackId{};
    std::array<uint64_t, EngineAdapter::kMaxDecks> m_pendingLoad{}; // wanted, not yet loaded
    uint32_t m_seenPeerGeneration = 0;

    // Fingerprint -> file path of tracks we may share, built in the background.
    std::mutex m_indexMutex;
    QHash<uint64_t, QString> m_index;
    QHash<uint64_t, ::b2b::TrackMeta> m_localMeta; // our tracks' library details (m_indexMutex)
    QHash<uint64_t, TrackPointer> m_localTracks;    // the tracks themselves, for fresh details (m_indexMutex)
    QHash<uint64_t, ::b2b::TrackMeta> m_peerMeta;  // main thread: details of tracks we are borrowing
    std::array<QMetaObject::Connection, EngineAdapter::kMaxDecks> m_beatsConnection;
    QHash<uint64_t, QPair<double, double>> m_lastGrid; // id -> (bpm, first beat frame) sent or received
    bool m_retriggeringGridEdit = false;
    std::array<QElapsedTimer, EngineAdapter::kMaxDecks> m_gridEditTimer; // since the last grid button press
    std::atomic<bool> m_indexDone{false};
    std::atomic<bool> m_stopIndex{false};
    std::thread m_indexThread;

    // Track sharing (both roles).
    std::unique_ptr<::b2b::FileShare> m_share;
    std::shared_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    QString m_cacheDir;                 // borrowed tracks for this session only
    QHash<uint64_t, QString> m_cache;   // main thread: received this session
    QHash<uint64_t, int> m_progress;    // main thread: id -> percent while receiving
    QHash<uint64_t, bool> m_requested;  // main thread: asked the peer for these
    QHash<uint64_t, QString> m_failed;  // main thread: id -> why the transfer failed
    QHash<uint64_t, int> m_failCount;   // main thread: id -> failed transfers so far
    QHash<uint64_t, qint64> m_failedAt; // main thread: id -> when it last failed (ms since epoch)
    int m_borrowedFiles = 0;            // main thread: received from the friend this session
    qint64 m_borrowedBytes = 0;         // ...and their size
    QHash<uint64_t, uint8_t> m_wantLater; // peer hints that arrived before our index was ready
    QHash<uint64_t, QString> m_peerNames; // the peer's tracks, for display
    bool m_shareConnected = false;
    bool m_failReported = false;
    bool m_codeWritten = false;
    int m_connectedTicks = 0;
    int m_peerTrackCount = 0;

    // Taking turns (main thread).
    bool m_allowTake = false;        // our setting: the friend may take control without asking
    bool m_peerAllowsTake = false;   // theirs
    int m_catchUpBars = 4;
    bool m_snapStyle = false;
    bool m_requestPending = false;   // we asked for control
    QElapsedTimer m_requestTimer;
    bool m_peerRequestOpen = false;  // the friend asked us
    ::b2b::Nanos m_listenDelay = 0;
    ::b2b::Nanos m_peerOffset = 0;
    bool m_autoHandoverDone = false;
    bool m_autoRequestDone = false;
    ::b2b::Nanos m_sentOffset = -1;
    QElapsedTimer m_sinceOffsetSent;
    bool m_lastLive = false;
    QElapsedTimer m_sinceRoleChange;
    QString m_catchUpText;

    // A deck each (main thread).
    bool m_wantDeckEach = true;         // our menu choice (the latest of either side's wins)
    ::b2b::Nanos m_lead = 0;            // how long after a move both hear it
    double m_peerOutputMs = -1;         // the friend's output latency (-1: not heard yet)
    double m_sentOutputMs = -1;
    uint64_t m_lateSeen = 0;
    QElapsedTimer m_sinceLateWarning;
    QElapsedTimer m_sinceLateSent;
    double m_leadBoostMs = 0;           // added because the friend saw our moves arrive late
    QElapsedTimer m_sinceLateReceived;  // ...and shrinks again while they don't
    uint32_t m_hostMask = 0xFu;         // the host's decks (the host keeps the list; the guest has the rest)
    uint32_t m_friendAsks = 0;          // our decks the friend asked for
    uint32_t m_asking = 0;              // their decks we asked for
    std::array<QElapsedTimer, EngineAdapter::kMaxDecks> m_askTimer;

    QTimer m_timer;
    int m_tickCount = 0;
    bool m_logWritten = false;
    QElapsedTimer m_sinceStart;
};

} // namespace mixxx::b2b
