#include "b2b/b2bmanager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

#include "b2b/b2bautopilot.h"
#include "b2b/b2bfingerprint.h"
#include "control/control.h"
#include "control/controlobject.h"
#include "effects/chains/quickeffectchain.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "effects/effectsmanager.h"
#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/presets/effectchainpresetmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginemixer.h"
#include "library/trackcollectionmanager.h"
#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "mixer/previewdeck.h"
#include "track/beats.h"
#include "track/track.h"

namespace mixxx::b2b {

namespace {
// "A deck each" with your own decks a little delayed, so your moves reach
// the friend on the dot (B2B_TIGHT_TIMING=1). Off: they play the moment you
// touch them.
bool tightTiming() {
    static const bool tight = qEnvironmentVariableIntValue("B2B_TIGHT_TIMING") == 1;
    return tight;
}
// The most the lead grows when moves reach the friend late (ms).
constexpr double kMaxLeadBoostMs = 25.0;
} // namespace

namespace {

constexpr int kTickMs = 20;

double envDouble(const char* name, double fallback) {
    const QByteArray v = qgetenv(name);
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : fallback;
}

QString envString(const char* name) {
    return QString::fromLocal8Bit(qgetenv(name));
}

// Controls mirrored from leader to follower, per deck. Headphone cue (pfl) is
// deliberately missing: what you preview in your headphones stays private.
const char* const kDeckControls[] = {
        "play",
        "volume",
        "pregain",
        "rate",
        "rateRange",
        "rate_dir",
        "keylock",
        "pitch_adjust",
        "mute",
        "orientation",
        "quantize",
        // The speed the hand or a VINYL SPEED ADJUST start / stop gives the
        // deck: friends hear the record slow down and spin up, not a jump.
        "scratch2_indicates_scratching", // (a nudge keeps keylock)
        "scratch2_enable",
        "scratch2",
};


bool isGuestRole(const QString& role) {
    return role == QStringLiteral("follower") || role == QStringLiteral("guest");
}

// What we tell the friend about one of our tracks.
::b2b::TrackMeta trackMeta(const TrackPointer& pTrack) {
    ::b2b::TrackMeta meta;
    meta.title = pTrack->getTitle().toStdString();
    meta.artist = pTrack->getArtist().toStdString();
    meta.key = pTrack->getKeyText().toStdString();
    meta.bpm = pTrack->getBpm();
    if (const mixxx::BeatsPointer pBeats = pTrack->getBeats()) {
        const QByteArray bytes = pBeats->toByteArray();
        meta.beats.assign(bytes.constBegin(), bytes.constEnd());
        meta.beatsVersion = pBeats->getVersion().toStdString();
        meta.beatsSubVersion = pBeats->getSubVersion().toStdString();
    }
    return meta;
}

} // namespace

bool Manager::enabledByEnvironment() {
    const QString role = envString("B2B_ROLE").toLower();
    return role == QStringLiteral("leader") || role == QStringLiteral("host") || isGuestRole(role);
}

Manager::Config Manager::configFromEnvironment() {
    Config c;
    c.role = isGuestRole(envString("B2B_ROLE").toLower()) ? ::b2b::Role::Follower : ::b2b::Role::Leader;
    c.port = static_cast<uint16_t>(envDouble("B2B_PORT", c.role == ::b2b::Role::Leader ? 9000 : 0));
    const QString peer = envString("B2B_PEER");
    const int colon = peer.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        c.peerHost = peer.left(colon);
        c.peerPort = static_cast<uint16_t>(peer.mid(colon + 1).toUInt());
    } else if (!peer.isEmpty()) {
        c.peerHost = peer;
    }
    c.server = envString("B2B_SERVER");
    if (c.server.isEmpty()) {
        c.server = QString::fromLatin1(::b2b::defaultRendezvousServer());
    }
    c.useCode = envDouble("B2B_USE_CODE", 0) > 0;
    c.relayOnly = envDouble("B2B_RELAY_ONLY", 0) > 0;
    c.password = envString("B2B_PASSWORD");
    c.joinCode = QString::fromStdString(::b2b::normaliseJoinCode(envString("B2B_CODE").toStdString()));
    c.delayMs = envDouble("B2B_DELAY_MS", 0);
    c.autopilotHandoverSec = envDouble("B2B_AUTOPILOT_HANDOVER_S", 0);
    c.autopilotRequestSec = envDouble("B2B_AUTOPILOT_REQUEST_S", 0);
    c.musicDir = envString("B2B_MUSIC_DIR");
    c.logPath = envString("B2B_LOG");
    c.autopilot = envString("B2B_AUTOPILOT");
    c.fakeOffsetMs = envDouble("B2B_FAKE_OFFSET_MS", 0);
    c.fakeDriftPpm = envDouble("B2B_FAKE_DRIFT_PPM", 0);
    c.correctMs = envDouble("B2B_CORRECT_MS", 5);
    c.logAfterSec = envDouble("B2B_LOG_AFTER_S", 0);
    c.shareKBps = envDouble("B2B_SHARE_KBPS", c.shareKBps);
    c.shareMaxMB = envDouble("B2B_SHARE_MAX_MB", c.shareMaxMB);
    c.sim.delayMs = envDouble("B2B_SIM_DELAY_MS", 0);
    c.sim.jitterMs = envDouble("B2B_SIM_JITTER_MS", 0);
    c.sim.lossPct = envDouble("B2B_SIM_LOSS", 0);
    c.sim.bandwidthKBps = envDouble("B2B_SIM_KBPS", 0);
    return c;
}

Manager::Manager(const Config& config,
        PlayerManager* pPlayerManager,
        EngineMixer* pEngineMixer,
        std::shared_ptr<EffectsManager> pEffectsManager,
        const QString& settingsPath,
        ::b2b::qt::SessionUi* pUi,
        QObject* pParent)
        : QObject(pParent),
          m_config(config),
          m_pUi(pUi),
          m_settingsPath(settingsPath),
          m_pPlayerManager(pPlayerManager),
          m_pEffectsManager(std::move(pEffectsManager)),
          m_pEngineMixer(pEngineMixer) {
    const bool leader = m_config.role == ::b2b::Role::Leader;
    m_clock = std::make_unique<::b2b::LocalClock>(
            ::b2b::fromMs(m_config.fakeOffsetMs), m_config.fakeDriftPpm);

    ::b2b::SessionConfig sc;
    sc.role = m_config.role;
    sc.localPort = m_config.port;
    sc.peerHost = m_config.peerHost.toStdString();
    sc.peerPort = m_config.peerPort;
    sc.password = m_config.password.toStdString();
    if (codeMode()) {
        const int colon = m_config.server.lastIndexOf(QLatin1Char(':'));
        sc.rendezvousHost = (colon > 0 ? m_config.server.left(colon) : m_config.server).toStdString();
        sc.rendezvousPort = colon > 0 ? static_cast<uint16_t>(m_config.server.mid(colon + 1).toUInt()) : 7700;
        sc.joinCode = m_config.joinCode.toStdString();
        sc.relayOnly = m_config.relayOnly;
    }
    // How far behind the live side we listen: fixed if B2B_DELAY_MS is set,
    // otherwise measured from the connection (starting safely at 150 ms).
    m_listenDelay = ::b2b::fromMs(m_config.delayMs > 0 ? m_config.delayMs : 150.0);
    sc.delayNs = m_listenDelay;
    sc.startInNs = 0;
    sc.sim = m_config.sim;
    m_session = std::make_unique<::b2b::Session>(sc, *m_clock);

    if (!m_config.logPath.isEmpty()) {
        m_log = std::make_unique<::b2b::EventLog>();
        m_log->setHeader("role", leader ? "host" : "guest");
        m_log->setHeader("fake_offset_ns", std::to_string(m_clock->fakeOffset()));
        m_log->setHeader("fake_drift_ppm", std::to_string(m_clock->fakeDriftPpm()));
        m_log->setHeader("origin_ns", std::to_string(m_clock->origin()));
        m_log->setHeader("follower_delay_ns", std::to_string(sc.delayNs));
        m_log->setHeader("sim_delay_ms", std::to_string(m_config.sim.delayMs));
        m_log->setHeader("sim_jitter_ms", std::to_string(m_config.sim.jitterMs));
        m_log->setHeader("sim_loss_pct", std::to_string(m_config.sim.lossPct));
    }

    // Decks and the controls we mirror. Always all four, on both sides: a
    // two-deck skin creates decks 3 and 4 only when you switch to four decks,
    // which would be too late (and the two apps would disagree about them).
    while (m_pPlayerManager->numberOfDecks() < EngineAdapter::kMaxDecks) {
        m_pPlayerManager->addDeck();
    }
    m_numDecks =std::min(static_cast<int>(m_pPlayerManager->numberOfDecks()), EngineAdapter::kMaxDecks);
    std::vector<EngineAdapter::DeckRefs> decks;
    std::vector<EngineAdapter::Watched> watched;
    auto watch = [&watched](const QString& group, const QString& item, int deck) {
        QSharedPointer<ControlDoublePrivate> pControl = ControlDoublePrivate::getControl(
                ConfigKey(group, item), ControlFlag::NoWarnIfMissing);
        if (!pControl) {
            return;
        }
        const std::string key = QString(group + QLatin1Char(',') + item).toStdString();
        for (const auto& existing : watched) {
            if (existing.key == key) {
                return;
            }
        }
        EngineAdapter::Watched w;
        w.control = pControl;
        w.key.assign(QString(group + QLatin1Char(',') + item).toStdString());
        w.deck = deck;
        w.isPlay = item == QStringLiteral("play");
        w.isKeylock = item == QStringLiteral("keylock");
        w.isSamplePos = item == QStringLiteral("loop_start_position") ||
                item == QStringLiteral("loop_end_position");
        if (item == QStringLiteral("sync_enabled")) {
            w.displayOnly = ControlObject::getControl(ConfigKey(group, item));
        }
        // A deck's mixer row (a deck each: anyone's): its trim, fader, EQ
        // and quick effect (the filter).
        w.isMixer = deck >= 0 &&
                (item == QStringLiteral("volume") || item == QStringLiteral("pregain") ||
                        group.startsWith(QStringLiteral("[EqualizerRack1_")) ||
                        group.startsWith(QStringLiteral("[QuickEffectRack1_")));
        watched.push_back(w);
    };
    for (int d = 0; d < m_numDecks; ++d) {
        Deck* pDeck = m_pPlayerManager->getDeck(d);
        const QString group = pDeck->getGroup();
        m_deckGroups.push_back(group);

        EngineAdapter::DeckRefs refs;
        refs.group = group;
        refs.buffer = pDeck->getEngineDeck()->getEngineBuffer();
        refs.play = ControlDoublePrivate::getControl(ConfigKey(group, QStringLiteral("play")));
        refs.wheel = ControlDoublePrivate::getControl(ConfigKey(group, QStringLiteral("wheel")));
        refs.keylock = ControlDoublePrivate::getControl(ConfigKey(group, QStringLiteral("keylock")));
        refs.trackSampleRate = ControlObject::getControl(ConfigKey(group, QStringLiteral("track_samplerate")));
        refs.rateRatio = ControlObject::getControl(ConfigKey(group, QStringLiteral("rate_ratio")));
        refs.bpm = ControlObject::getControl(ConfigKey(group, QStringLiteral("bpm")));
        refs.beatDistance = ControlObject::getControl(ConfigKey(group, QStringLiteral("beat_distance")));
        refs.volume = ControlObject::getControl(ConfigKey(group, QStringLiteral("volume")));
        refs.pregain = ControlObject::getControl(ConfigKey(group, QStringLiteral("pregain")));
        decks.push_back(refs);

        for (const char* item : kDeckControls) {
            watch(group, QString::fromLatin1(item), d);
        }
        watch(group, QStringLiteral("sync_enabled"), d);
        // Loops: the points first, then on/off, so the listener's deck loops
        // at the same places (and shows the loop) instead of only following jumps.
        watch(group, QStringLiteral("loop_start_position"), d);
        watch(group, QStringLiteral("loop_end_position"), d);
        watch(group, QStringLiteral("loop_enabled"), d);
        const QString eq = QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(group);
        for (int p = 1; p <= 3; ++p) {
            watch(eq, QStringLiteral("parameter%1").arg(p), d);
            watch(eq, QStringLiteral("button_parameter%1").arg(p), d);
        }

        if (!leader) {
            // The guest starts as the listener: its decks are driven by the
            // host, so no local sync that could move them on its own.
            ControlObject::set(ConfigKey(group, QStringLiteral("sync_enabled")), 0.0);
        }

        connect(pDeck, &BaseTrackPlayer::newTrackLoaded, this, [this, d](TrackPointer pTrack) {
            trackLoaded(d, pTrack);
        });
        for (const char* item : {"beats_translate_curpos",
                     "beats_translate_earlier",
                     "beats_translate_later",
                     "beats_adjust_faster",
                     "beats_adjust_slower",
                     "beats_set_halve",
                     "beats_set_double",
                     "beats_set_twothirds",
                     "beats_set_threefourths",
                     "beats_set_fourthirds",
                     "beats_set_threehalves",
                     "beats_translate_match_alignment",
                     "beats_undo_adjustment"}) {
            ControlObject* pControl = ControlObject::getControl(
                    ConfigKey(group, QString::fromLatin1(item)), ControlFlag::NoWarnIfMissing);
            if (pControl) {
                connect(pControl, &ControlObject::valueChanged, this, [this, d, pControl](double v) {
                    gridEditPressed(d, pControl, v);
                });
            }
        }
        connect(pDeck, &BaseTrackPlayer::playerEmpty, this, [this, d] { trackUnloaded(d); });
    }
    watch(QStringLiteral("[Master]"), QStringLiteral("crossfader"), -1);
    // The crossfader's curve and direction shape what the crowd hears too.
    // So does the DJ EQ's EQ / ISOLATOR mode and its crossovers.
    for (const char* item : {"xFaderCurve",
                 "xFaderMode",
                 "xFaderCalibration",
                 "xFaderReverse",
                 "LoEQFrequency",
                 "HiEQFrequency",
                 "B2BEqIsolator"}) {
        watch(QStringLiteral("[Mixer Profile]"), QString::fromLatin1(item), -1);
    }

    // Effects: the four effect units and each deck's quick effect, with
    // every effect's knobs and buttons. (Which effect is loaded in each slot
    // is synced on the main thread, see syncEffects().)
    auto watchChain = [&](const EffectChainPointer& pChain, int deck) {
        if (!pChain) {
            return;
        }
        const QString chain = pChain->getGroup();
        for (const char* item : {"enabled", "mix", "super1", "mix_mode"}) {
            watch(chain, QString::fromLatin1(item), deck);
        }
        for (const QString& deckGroup : m_deckGroups) {
            watch(chain, QStringLiteral("group_%1_enable").arg(deckGroup), deck);
        }
        watch(chain, QStringLiteral("group_[Master]_enable"), deck);
        for (const EffectSlotPointer& pSlot : pChain->getEffectSlots()) {
            const QString slot = pSlot->getGroup();
            watch(slot, QStringLiteral("enabled"), deck);
            watch(slot, QStringLiteral("meta"), deck);
            for (int p = 1; p <= 16; ++p) {
                watch(slot, QStringLiteral("parameter%1").arg(p), deck);
                watch(slot, QStringLiteral("button_parameter%1").arg(p), deck);
            }
        }
    };
    if (m_pEffectsManager) {
        for (int u = 0; u < 4; ++u) {
            watchChain(m_pEffectsManager->getStandardEffectChain(u), -1);
        }
        for (int d = 0; d < m_numDecks; ++d) {
            watchChain(m_pEffectsManager->getQuickEffectChain(m_deckGroups[d]), d);
        }
    }
    qInfo() << "B2B: mirroring" << watched.size() << "controls";

    m_adapter = std::make_unique<EngineAdapter>(*m_session,
            *m_clock,
            m_log.get(),
            std::move(decks),
            std::move(watched),
            ControlObject::getControl(ConfigKey(QStringLiteral("[App]"), QStringLiteral("samplerate"))),
            ControlObject::getControl(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")),
                    ControlFlag::NoWarnIfMissing),
            m_config.correctMs,
            leader,              // the host is live first
            leader ? 0 : m_listenDelay);
    m_adapter->setListenDelay(m_listenDelay);
    // A deck each: the host starts with every deck (the guest asks for one
    // with +), or, split, with decks 1 and 2 (the guest has 3 and 4).
    // (B2B_GUEST_DECKS=2,4 gives the guest decks from the start: testing.)
    if (leader && m_config.splitDecks) {
        m_hostMask = 0x3u;
    }
    for (const QString& n : envString("B2B_GUEST_DECKS").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int d = n.trimmed().toInt() - 1;
        if (d >= 0 && d < EngineAdapter::kMaxDecks) {
            m_hostMask &= ~(1u << d);
        }
    }
    m_adapter->setOwnedDecks(leader ? m_hostMask : (~m_hostMask & 0xFu));
    m_adapter->setMixer(m_pEngineMixer);
    EngineAdapter::setActive(m_adapter.get()); // the mixer row's timing (EQ, filter)
    m_lead = ::b2b::fromMs(envDouble("B2B_LEAD_MS", tightTiming() ? 30 : 0));
    m_adapter->setLead(m_lead);
    m_lastLive = leader;
    m_sinceRoleChange.start();

    if (m_pUi) {
        connect(m_pUi, &::b2b::qt::SessionUi::takeOrRequestClicked, this, &Manager::takeOrRequest);
        connect(m_pUi, &::b2b::qt::SessionUi::handOverClicked, this, &Manager::handOver);
        connect(m_pUi, &::b2b::qt::SessionUi::requestAnswered, this, &Manager::answerRequest);
        connect(m_pUi, &::b2b::qt::SessionUi::prefsChanged, this, &Manager::applyPrefs);
        applyPrefs(m_pUi->prefs());
    } else {
        applyPrefs(::b2b::qt::HandoverPrefs{});
    }

    // Tracks that were already loaded before we started.
    for (int d = 0; d < m_numDecks; ++d) {
        if (TrackPointer pTrack = m_pPlayerManager->getDeck(d)->getLoadedTrack()) {
            trackLoaded(d, pTrack);
        }
    }
    m_starting = false;

    watchPreviewDecks();

    m_pEngineMixer->setPreProcessHook(this);

    m_started = m_session->start();
    if (!m_started) {
        qWarning() << "B2B: could not start session (port in use or bad peer address?)";
    } else if (leader) {
        qInfo() << "B2B: host listening on UDP port" << m_config.port
                << (codeMode() ? "with a join code from" : "") << (codeMode() ? m_config.server : QString());
    } else if (codeMode()) {
        // (Not the code: logs go out in bug reports.)
        qInfo() << "B2B: guest joining with a join code via" << m_config.server;
    } else {
        qInfo() << "B2B: guest connecting to" << m_config.peerHost << m_config.peerPort;
    }
    startFileShare();
    startMusicIndex();

    if (!m_config.autopilot.isEmpty()) {
        m_autopilot = std::make_unique<Autopilot>(m_pPlayerManager,
                m_config.autopilot,
                m_numDecks,
                envDouble("B2B_AUTOPILOT_DELAY_S", 10));
    }

    connect(&m_timer, &QTimer::timeout, this, &Manager::slotTick);
    m_timer.start(kTickMs);
    m_sinceStart.start();
}

Manager::~Manager() {
    m_timer.stop();
    m_pEngineMixer->setPreProcessHook(nullptr); // the audio thread is done with us after this
    m_adapter->finish();
    if (m_pUi) {
        m_pUi->closeRequest();
        m_pUi->setState(::b2b::qt::UiState{});
    }
    m_stopIndex = true;
    if (m_indexThread.joinable()) {
        m_indexThread.join();
    }
    if (m_share) {
        m_share->stop();
    }
    if (m_session) {
        m_session->stop();
    }
    // Borrowed tracks are for this session only.
    if (m_pTrackCollectionManager && !m_cacheDir.isEmpty()) {
        m_pTrackCollectionManager->purgeAllTracks(QDir(m_cacheDir));
    }
    clearCache();
    writeLog();
}

void Manager::setTrackCollectionManager(std::shared_ptr<TrackCollectionManager> pTrackCollectionManager) {
    m_pTrackCollectionManager = std::move(pTrackCollectionManager);
    // Leftovers from a session that did not end cleanly (crash, or files that
    // Windows kept locked at exit): the files were wiped at start-up, now drop
    // them from the library too.
    if (m_pTrackCollectionManager && !m_cacheDir.isEmpty()) {
        m_pTrackCollectionManager->purgeAllTracks(QDir(m_cacheDir));
    }
}

bool Manager::isInCache(const QString& path) const {
    if (m_cacheDir.isEmpty()) {
        return false;
    }
    const QString file = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    return file.startsWith(m_cacheDir + QLatin1Char('/'), Qt::CaseInsensitive);
}

void Manager::clearCache() {
    if (m_cacheDir.isEmpty()) {
        return;
    }
    QDir dir(m_cacheDir);
    if (dir.exists() && !dir.removeRecursively()) {
        qWarning() << "B2B: could not fully remove the session cache" << m_cacheDir
                   << "(files still in use?) - it is wiped again at next start";
    }
    m_cache.clear();
    m_borrowedFiles = 0;
    m_borrowedBytes = 0;
}

void Manager::startFileShare() {
    m_cacheDir = QDir::cleanPath(QDir(m_settingsPath).absoluteFilePath(QStringLiteral("b2b-cache")));
    clearCache();
    QDir().mkpath(m_cacheDir);

    ::b2b::FileShareConfig fc;
    fc.session = m_session.get(); // inside the session's own connection: works wherever it does
    fc.role = m_config.role;
    fc.cacheDir = m_cacheDir.toStdString(); // UTF-8
    fc.maxFileBytes = static_cast<uint64_t>(m_config.shareMaxMB * 1024 * 1024);
    fc.rateBytesPerSec = static_cast<uint64_t>(m_config.shareKBps * 1024);

    // All of these run on the share's network thread. Only `lookup` answers
    // there; the rest hop over to the main thread. Using `this` as the
    // context object means nothing runs once we are gone.
    ::b2b::FileShare::Callbacks cb;
    cb.lookup = [this](uint64_t id, std::string& path) {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        const QString p = m_index.value(id);
        if (p.isEmpty()) {
            return false;
        }
        path = p.toStdString();
        return true;
    };
    cb.describe = [this](uint64_t id, ::b2b::TrackMeta& meta) {
        // Fresh from the track (its analysis may have finished since it was
        // loaded). Track and its beats are safe to read from any thread.
        TrackPointer pTrack;
        {
            std::lock_guard<std::mutex> lock(m_indexMutex);
            pTrack = m_localTracks.value(id);
        }
        if (!pTrack) {
            return false;
        }
        meta = trackMeta(pTrack);
        return true;
    };
    cb.onPeerMeta = [this](uint64_t id, ::b2b::TrackMeta meta) {
        QMetaObject::invokeMethod(this, [this, id, meta] { m_peerMeta.insert(id, meta); }, Qt::QueuedConnection);
    };
    cb.onPeerList = [this](std::vector<::b2b::SharedTrack> list) {
        QHash<uint64_t, QString> names;
        for (const auto& t : list) {
            names.insert(t.id, QString::fromStdString(t.name));
        }
        QMetaObject::invokeMethod(this, [this, names] {
            m_peerTrackCount = names.size();
            m_peerNames = names;
            qInfo() << "B2B: peer shares" << names.size() << "tracks";
        }, Qt::QueuedConnection);
    };
    cb.onPeerHint = [this](uint64_t id, uint8_t priority) {
        QMetaObject::invokeMethod(this, [this, id, priority] { onPeerHint(id, priority); }, Qt::QueuedConnection);
    };
    cb.onProgress = [this](uint64_t id, uint64_t got, uint64_t total) {
        const int pct = total ? static_cast<int>(got * 100 / total) : 0;
        QMetaObject::invokeMethod(this, [this, id, pct] { m_progress[id] = pct; }, Qt::QueuedConnection);
    };
    cb.onReceived = [this](uint64_t id, std::string path) {
        const QString p = QString::fromStdString(path);
        QMetaObject::invokeMethod(this, [this, id, p] { onReceived(id, p); }, Qt::QueuedConnection);
    };
    cb.onFailed = [this](uint64_t id, std::string reason) {
        const QString r = QString::fromStdString(reason);
        QMetaObject::invokeMethod(this, [this, id, r] { onTransferFailed(id, r); }, Qt::QueuedConnection);
    };
    cb.onConnection = [this](bool up) {
        QMetaObject::invokeMethod(this, [this, up] {
            m_shareConnected = up;
            qInfo() << "B2B: track sharing" << (up ? "connected" : "disconnected");
        }, Qt::QueuedConnection);
    };

    m_share = std::make_unique<::b2b::FileShare>(fc, std::move(cb));
    if (!m_share->start()) {
        qWarning() << "B2B: could not start track sharing";
        m_share.reset();
        return;
    }
    qInfo().noquote() << QStringLiteral("B2B: track sharing over the session, upload cap %1 KB/s, max %2 MB, cache %3")
                                 .arg(m_config.shareKBps)
                                 .arg(m_config.shareMaxMB)
                                 .arg(m_cacheDir);
}

void Manager::sendTrackList() {
    if (!m_share) {
        return;
    }
    std::vector<::b2b::SharedTrack> list;
    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        list.reserve(m_index.size());
        for (auto it = m_index.constBegin(); it != m_index.constEnd(); ++it) {
            const QFileInfo info(it.value());
            ::b2b::SharedTrack t;
            t.id = it.key();
            t.size = static_cast<uint64_t>(info.size());
            const auto meta = m_localMeta.constFind(it.key());
            if (meta != m_localMeta.constEnd() && !meta->title.empty()) {
                t.name = meta->artist.empty() ? meta->title : meta->artist + " - " + meta->title;
            } else {
                t.name = info.completeBaseName().toStdString();
            }
            list.push_back(std::move(t));
        }
    }
    m_share->setLocalTracks(std::move(list));
}

void Manager::watchPreviewDecks() {
    for (int i = 0; i < m_pPlayerManager->numberOfPreviewDecks(); ++i) {
        PreviewDeck* pPreview = m_pPlayerManager->getPreviewDeck(i);
        if (!pPreview) {
            continue;
        }
        connect(pPreview, &BaseTrackPlayer::newTrackLoaded, this, [this](TrackPointer pTrack) {
            if (!pTrack || !m_share) {
                return;
            }
            const QString path = pTrack->getLocation();
            const uint64_t id = fingerprintFile(path);
            if (id == 0 || isInCache(path)) {
                return;
            }
            shareTrack(pTrack, id);
            // Previewing is a strong sign it is coming up next: let the peer
            // fetch it in the background.
            m_share->hint(id, 2);
        });
    }
}

QString Manager::localPathFor(uint64_t id) {
    const QString cached = m_cache.value(id);
    if (!cached.isEmpty()) {
        return cached;
    }
    std::lock_guard<std::mutex> lock(m_indexMutex);
    return m_index.value(id);
}

namespace {
// What the friend's app can make us fetch in one session (each file is also
// capped at shareMaxMB): a few at a time, and so much in all.
constexpr int kMaxFetchesAtOnce = 6;      // decks loading plus a preview or two
constexpr int kMaxBorrowedFiles = 200;
constexpr qint64 kMaxBorrowedBytes = 4LL * 1024 * 1024 * 1024;
constexpr int kMaxSetParts = 4;           // set-list entries being pieced together at once
} // namespace

void Manager::requestFromPeer(uint64_t id, uint8_t priority, const char* why) {
    if (!m_share || id == 0 || m_requested.contains(id) || m_failed.contains(id)) {
        return;
    }
    if (m_requested.size() >= kMaxFetchesAtOnce) {
        return; // previews are skipped; a deck's track is asked for again on the next pass
    }
    if (m_borrowedFiles >= kMaxBorrowedFiles || m_borrowedBytes >= kMaxBorrowedBytes) {
        if (priority == 0) {
            qWarning() << "B2B: not fetching" << fingerprintToHex(id) << "- this session's borrowed tracks are at the limit";
            m_failed.insert(id, QStringLiteral("too much from your friend this session"));
            m_failCount[id] = 99; // not retried
            m_failedAt[id] = QDateTime::currentMSecsSinceEpoch();
        }
        return;
    }
    m_requested.insert(id, true);
    qInfo() << "B2B: requesting" << fingerprintToHex(id) << "from peer (" << why << ")";
    m_share->request(id, priority);
}

void Manager::onPeerHint(uint64_t id, uint8_t priority) {
    if (!localPathFor(id).isEmpty()) {
        return;
    }
    if (!m_indexDone) {
        // We may have it and just not know yet.
        m_wantLater.insert(id, priority);
        return;
    }
    requestFromPeer(id, priority, priority == 0 ? "peer loaded it" : "peer is previewing it");
}

void Manager::onReceived(uint64_t id, const QString& path) {
    m_requested.remove(id);
    m_progress.remove(id);
    // Only keep what we asked for, byte for byte.
    if (fingerprintFile(path) != id) {
        qWarning() << "B2B: received" << path << "but its fingerprint does not match - discarded";
        QFile::remove(path);
        m_failed.insert(id, QStringLiteral("fingerprint mismatch"));
        return;
    }
    qInfo() << "B2B: received" << fingerprintToHex(id) << "->" << path;
    m_borrowedFiles += 1;
    m_borrowedBytes += QFileInfo(path).size();
    // Read-only: nothing (e.g. writing tags back to files) may change the
    // borrowed copy, or it would no longer match the friend's track.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser);
    m_cache.insert(id, path);
    tryPendingLoads();
}

void Manager::onTransferFailed(uint64_t id, const QString& reason) {
    m_requested.remove(id);
    m_progress.remove(id);
    m_failed.insert(id, reason);
    m_failCount[id] += 1;
    m_failedAt[id] = QDateTime::currentMSecsSinceEpoch();
    qWarning() << "B2B: could not get" << fingerprintToHex(id) << "from peer:" << reason;
}

void Manager::writeLog() {
    if (m_log && !m_config.logPath.isEmpty()) {
        if (m_log->writeCsv(m_config.logPath.toStdString())) {
            qInfo() << "B2B: log written to" << m_config.logPath;
        }
    }
    m_logWritten = true;
}

void Manager::trackLoaded(int deck, TrackPointer pTrack) {
    if (!pTrack) {
        trackUnloaded(deck);
        return;
    }
    const QString location = pTrack->getLocation();
    const uint64_t id = fingerprintFile(location);
    m_deckTrackId[deck] = id;
    QObject::disconnect(m_beatsConnection[deck]);
    // Queued: analysis finishes (and updates the grid) on another thread.
    m_beatsConnection[deck] = connect(pTrack.get(), &Track::beatsUpdated, this, [this, deck] {
        deckBeatsUpdated(deck);
    }, Qt::QueuedConnection);
    // Anything you load (from your own library) becomes shareable.
    if (id != 0 && isInCache(location)) {
        applyPeerMeta(pTrack, id);
    }
    if (id != 0 && !isInCache(location)) {
        shareTrack(pTrack, id);
        if (m_share) {
            m_share->hint(id, 0);
        }
    }
    m_adapter->setDeckTrackId(deck, id);
    qInfo() << "B2B:" << m_deckGroups[deck] << "loaded" << pTrack->getLocation() << "fingerprint"
            << fingerprintToHex(id);
    const bool mirrored = m_pendingLoad[deck] == id; // we loaded it because the peer asked
    if (mirrored) {
        m_pendingLoad[deck] = 0;
    }
    if (controlsDeck(deck)) {
        sendLoad(deck);
    } else if (!mirrored && id != 0 && !m_starting) {
        // Deck prep while the friend is live: they load it too.
        ::b2b::ControlEvent ev;
        ev.time = contentNow();
        ev.key.assign(QString(m_deckGroups[deck] + QStringLiteral(",b2b_prep_load")).toStdString());
        ev.text.assign(fingerprintToHex(id).toStdString());
        m_session->sendEventFromMainThread(ev);
        qInfo() << "B2B: prepared" << m_deckGroups[deck] << "- asking the live side to load it too";
    }
}

void Manager::shareTrack(const TrackPointer& pTrack, uint64_t id) {
    ::b2b::TrackMeta meta = trackMeta(pTrack);
    bool added = false;
    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (!m_index.contains(id)) {
            m_index.insert(id, pTrack->getLocation());
            added = true;
        }
        const auto old = m_localMeta.constFind(id);
        if (old == m_localMeta.constEnd() || old->title != meta.title || old->artist != meta.artist) {
            added = true; // the name in the list changes
        }
        m_localMeta.insert(id, std::move(meta));
        m_localTracks.insert(id, pTrack);
    }
    if (added) {
        sendTrackList();
    }
}

void Manager::applyPeerMeta(const TrackPointer& pTrack, uint64_t id) {
    const auto it = m_peerMeta.constFind(id);
    if (it == m_peerMeta.constEnd()) {
        return;
    }
    const ::b2b::TrackMeta& meta = it.value();
    if (!meta.title.empty()) {
        pTrack->setTitle(QString::fromStdString(meta.title));
    }
    if (!meta.artist.empty()) {
        pTrack->setArtist(QString::fromStdString(meta.artist));
    }
    if (!meta.key.empty()) {
        pTrack->setKeyText(QString::fromStdString(meta.key), mixxx::track::io::key::USER);
    }
    // The friend's beat grid, so quantize and sync line up exactly as on
    // their side. Locked, so our own analysis doesn't replace it.
    bool gridApplied = false;
    if (!meta.beats.empty() && pTrack->getSampleRate().isValid()) {
        const QByteArray bytes(reinterpret_cast<const char*>(meta.beats.data()),
                static_cast<int>(meta.beats.size()));
        if (const mixxx::BeatsPointer pBeats = mixxx::Beats::fromByteArray(pTrack->getSampleRate(),
                    QString::fromStdString(meta.beatsVersion),
                    QString::fromStdString(meta.beatsSubVersion),
                    bytes)) {
            gridApplied = pTrack->trySetAndLockBeats(pBeats);
        }
    }
    qInfo() << "B2B: borrowed track" << fingerprintToHex(id) << "is" << pTrack->getArtist() << "-"
            << pTrack->getTitle() << (gridApplied ? "(friend's beat grid)" : "(own analysis)")
            << "grid bytes" << meta.beats.size() << "sample rate" << pTrack->getSampleRate().value();
}

namespace {
// A constant grid as (bpm, first beat frame); false for a beat map (tempo
// changes), which doesn't fit in a session message.
bool constantGrid(const TrackPointer& pTrack, double* pBpm, double* pFirst) {
    const mixxx::BeatsPointer pBeats = pTrack ? pTrack->getBeats() : mixxx::BeatsPointer();
    if (!pBeats || !pBeats->hasConstantTempo()) {
        return false;
    }
    const mixxx::Bpm bpm = pBeats->getBpmInRange(mixxx::audio::kStartFramePos,
            mixxx::audio::FramePos{pTrack->getDuration() * pBeats->getSampleRate()});
    if (!bpm.isValid() || !pBeats->firstBeat().isValid()) {
        return false;
    }
    *pBpm = bpm.value();
    *pFirst = pBeats->firstBeat().value();
    return true;
}

bool sameGrid(const QPair<double, double>& a, double bpm, double first) {
    return std::fabs(a.first - bpm) < 1e-6 && std::fabs(a.second - first) < 0.5;
}
} // namespace

void Manager::deckBeatsUpdated(int deck) {
    const uint64_t id = m_deckTrackId[deck];
    if (!m_session || id == 0) {
        return;
    }
    const TrackPointer pTrack = m_pPlayerManager->getDeck(deck)->getLoadedTrack();
    double bpm = 0;
    double first = 0;
    if (!constantGrid(pTrack, &bpm, &first)) {
        return;
    }
    const auto last = m_lastGrid.constFind(id);
    if (last != m_lastGrid.constEnd() && sameGrid(*last, bpm, first)) {
        return; // nothing new, or the friend's grid we just applied
    }
    m_lastGrid.insert(id, qMakePair(bpm, first));
    // A borrowed track's grid comes from its owner. Only a hand edit here is
    // news for them, not our analyzer or the owner's grid arriving.
    if (isInCache(pTrack->getLocation()) &&
            (!m_gridEditTimer[deck].isValid() || m_gridEditTimer[deck].elapsed() > 3000)) {
        return;
    }
    sendSessionMessage("grid", bpm, fingerprintToHex(id) + QLatin1Char(' ') + QString::number(first, 'f', 2));
    qInfo() << "B2B: beat grid of" << fingerprintToHex(id) << "changed:" << bpm << "BPM, first beat" << first
            << "- sent to the friend";
}

void Manager::applyPeerGrid(double bpm, const QString& text) {
    const QStringList parts = text.split(QLatin1Char(' '));
    if (parts.size() != 2 || bpm <= 0) {
        return;
    }
    bool ok = false;
    const uint64_t id = parts[0].toULongLong(&ok, 16);
    const double first = parts[1].toDouble(&ok);
    if (!ok || id == 0) {
        return;
    }
    m_lastGrid.insert(id, qMakePair(bpm, first)); // so it isn't sent back
    // Our copies: our own library track, and whatever deck has it loaded.
    QList<TrackPointer> tracks;
    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (TrackPointer pTrack = m_localTracks.value(id)) {
            tracks.append(pTrack);
        }
    }
    for (int d = 0; d < m_numDecks; ++d) {
        if (m_deckTrackId[d] == id) {
            TrackPointer pTrack = m_pPlayerManager->getDeck(d)->getLoadedTrack();
            if (pTrack && !tracks.contains(pTrack)) {
                tracks.append(pTrack);
            }
        }
    }
    for (const TrackPointer& pTrack : std::as_const(tracks)) {
        if (!pTrack->getSampleRate().isValid()) {
            continue;
        }
        const mixxx::BeatsPointer pOld = pTrack->getBeats();
        const mixxx::BeatsPointer pBeats = mixxx::Beats::fromConstTempo(pTrack->getSampleRate(),
                mixxx::audio::FramePos(first),
                mixxx::Bpm(bpm),
                pOld ? pOld->getSubVersion() : QString());
        if (!pBeats) {
            continue;
        }
        const bool wasLocked = pTrack->isBpmLocked();
        if (wasLocked) {
            pTrack->setBpmLocked(false);
        }
        pTrack->trySetBeats(pBeats);
        // A borrowed copy keeps the owner's grid: locked, so our analyzer
        // (perhaps still running) can't replace it.
        if (wasLocked || isInCache(pTrack->getLocation())) {
            pTrack->setBpmLocked(true);
        }
        // A borrowed track loaded again later gets this grid too.
        auto meta = m_peerMeta.find(id);
        if (meta != m_peerMeta.end()) {
            const QByteArray bytes = pBeats->toByteArray();
            meta->beats.assign(bytes.constBegin(), bytes.constEnd());
            meta->beatsVersion = pBeats->getVersion().toStdString();
            meta->beatsSubVersion = pBeats->getSubVersion().toStdString();
        }
    }
    qInfo() << "B2B: friend changed the beat grid of" << fingerprintToHex(id) << "to" << bpm << "BPM, first beat"
            << first << "- applied to" << tracks.size() << "copies";
}

void Manager::gridEditPressed(int deck, ControlObject* pControl, double value) {
    if (value <= 0 || m_retriggeringGridEdit) {
        return;
    }
    m_gridEditTimer[deck].start();
    const TrackPointer pTrack = m_pPlayerManager->getDeck(deck)->getLoadedTrack();
    if (!pTrack || !pTrack->isBpmLocked() || !isInCache(pTrack->getLocation())) {
        return; // Mixxx has already done it (or the user locked their own track)
    }
    // Mixxx ignored the press because the borrowed grid is locked: unlock,
    // press again, lock again.
    m_retriggeringGridEdit = true;
    pTrack->setBpmLocked(false);
    ControlObject::set(pControl->getKey(), value); // (set() from the control itself wouldn't reach Mixxx)
    pTrack->setBpmLocked(true);
    m_retriggeringGridEdit = false;
}

void Manager::trackUnloaded(int deck) {
    QObject::disconnect(m_beatsConnection[deck]);
    m_deckTrackId[deck] = 0;
    m_adapter->setDeckTrackId(deck, 0);
}

void Manager::sendLoad(int deck) {
    if (m_deckTrackId[deck] == 0) {
        return;
    }
    ::b2b::ControlEvent ev;
    ev.time = contentNow();
    ev.key.assign(QString(m_deckGroups[deck] + QStringLiteral(",b2b_load")).toStdString());
    ev.value = 0;
    ev.text.assign(fingerprintToHex(m_deckTrackId[deck]).toStdString());
    m_session->sendEventFromMainThread(ev);
}

void Manager::startMusicIndex() {
    if (m_config.musicDir.isEmpty()) {
        qInfo() << "B2B: B2B_MUSIC_DIR is not set; sharing only the tracks you load";
        m_indexDone = true;
        return;
    }
    const QString dir = m_config.musicDir;
    m_indexThread = std::thread([this, dir] {
        const QStringList filters = {QStringLiteral("*.mp3"),
                QStringLiteral("*.flac"),
                QStringLiteral("*.wav"),
                QStringLiteral("*.aiff"),
                QStringLiteral("*.aif"),
                QStringLiteral("*.ogg"),
                QStringLiteral("*.opus"),
                QStringLiteral("*.m4a"),
                QStringLiteral("*.mp4")};
        QDirIterator it(dir, filters, QDir::Files, QDirIterator::Subdirectories);
        int count = 0;
        while (it.hasNext() && !m_stopIndex) {
            const QString path = it.next();
            const uint64_t id = fingerprintFile(path);
            if (id != 0) {
                std::lock_guard<std::mutex> lock(m_indexMutex);
                m_index.insert(id, path);
                ++count;
            }
        }
        qInfo() << "B2B: indexed" << count << "tracks in" << dir;
        m_indexDone = true;
        QMetaObject::invokeMethod(this, [this] { sendTrackList(); }, Qt::QueuedConnection);
    });
}

namespace {
constexpr int kMaxFetchTries = 3;
constexpr qint64 kFetchRetryMs = 10000;
} // namespace

void Manager::tryPendingLoads() {
    for (int d = 0; d < m_numDecks; ++d) {
        if (m_pendingLoad[d] == 0 || m_pendingLoad[d] == m_deckTrackId[d]) {
            continue;
        }
        const uint64_t id = m_pendingLoad[d];
        const QString path = localPathFor(id);
        if (controlsDeck(d) && ControlObject::get(ConfigKey(m_deckGroups[d], QStringLiteral("play"))) > 0.5) {
            m_pendingLoad[d] = 0; // the friend prepared a deck that we have started playing since
            continue;
        }
        if (!path.isEmpty()) {
            qInfo() << "B2B: peer loaded" << fingerprintToHex(id) << "on" << m_deckGroups[d]
                    << "- loading" << (isInCache(path) ? "borrowed copy" : "local copy") << path;
            m_pPlayerManager->slotLoadLocationToPlayer(path, m_deckGroups[d], false);
            m_deckTrackId[d] = id; // don't load again while it loads
        } else if (m_failed.contains(id)) {
            // Try again a few times (a dropped connection, for instance),
            // unless it can never work.
            const QString reason = m_failed.value(id);
            const bool permanent = reason == QStringLiteral("too large") ||
                    reason == QStringLiteral("fingerprint mismatch");
            const int tries = m_failCount.value(id, 0);
            if (!permanent && tries < kMaxFetchTries) {
                if (QDateTime::currentMSecsSinceEpoch() - m_failedAt.value(id, 0) > kFetchRetryMs) {
                    m_failed.remove(id);
                    qInfo() << "B2B: trying" << fingerprintToHex(id) << "again (" << tries << "failed so far)";
                    requestFromPeer(id, 0, "retry");
                }
                continue;
            }
            qWarning() << "B2B: peer loaded track" << fingerprintToHex(id) << "on" << m_deckGroups[d]
                       << "but it could not be fetched:" << reason;
            if (m_pUi) {
                m_pUi->showMessage(tr("deck %1: couldn't get the track from your friend (%2).")
                                           .arg(d + 1)
                                           .arg(reason));
            }
            m_pendingLoad[d] = 0;
        } else if (m_indexDone) {
            requestFromPeer(id, 0, "peer loaded it");
        }
    }
}

bool Manager::isLive() const {
    return m_adapter && m_adapter->isLive();
}

bool Manager::controlsDeck(int deck) const {
    return deckEach() ? m_adapter->ownsDeck(deck) : isLive();
}

bool Manager::announcesEffects(const QString& code) const {
    if (!deckEach()) {
        return isLive();
    }
    if (code.startsWith(QLatin1Char('Q'))) {
        return m_adapter->ownsDeck(code.mid(1).toInt() - 1); // each deck's own filter
    }
    return isHost(); // the effect units: the host's
}

QString Manager::myDecksText() const {
    QStringList decks;
    for (int d = 0; d < m_numDecks; ++d) {
        if (myDecks() & (1u << d)) {
            decks << QString::number(d + 1);
        }
    }
    if (decks.isEmpty()) {
        return tr("no decks yet (press + on one to ask)");
    }
    if (decks.size() == 1) {
        return tr("deck %1").arg(decks.first());
    }
    const QString last = decks.takeLast();
    return tr("decks %1 and %2").arg(decks.join(QStringLiteral(", ")), last);
}

bool Manager::peerConnected() const {
    const ::b2b::Nanos heard = m_session->lastHeard();
    return m_session->peerGeneration() > 0 && m_session->ready() && heard > 0 &&
            m_clock->now() - heard < 3 * ::b2b::kNanosPerSec;
}

QString Manager::linkKind() const {
    if (!m_session || !peerConnected()) {
        return QString();
    }
    const bool relayed = codeMode() && m_session->rendezvousStatus().relayed;
    return relayed ? QStringLiteral("relay") : QStringLiteral("direct");
}

bool Manager::friendJoined() const {
    return m_session && m_session->peerGeneration() > 0;
}

void Manager::setSplitDecks(bool split) {
    if (!isHost() || !m_adapter) {
        return;
    }
    const uint32_t mask = split ? 0x3u : 0xFu;
    if (mask == m_hostMask) {
        return;
    }
    // A little from now, so the friend's app hears of it first.
    setHostDecks(mask, contentNow() + ::b2b::fromMs(300), true);
}

Manager::DeckOwner Manager::deckOwner(int deck) const {
    DeckOwner o;
    if (deck < 0 || deck >= m_numDecks || !deckEach() || !peerConnected()) {
        return o;
    }
    const uint32_t bit = 1u << deck;
    o.shown = true;
    o.mine = myDecks() & bit;
    o.friendAsks = o.mine && (m_friendAsks & bit);
    o.asking = !o.mine && (m_asking & bit);
    return o;
}

void Manager::setHostDecks(uint32_t hostMask, ::b2b::Nanos at, bool announce) {
    // Both apps switch at the same moment on the shared timeline, a little
    // from now so the message gets there first.
    m_hostMask = hostMask & 0xFu;
    m_adapter->setOwnedDecksAt(myDecks(), at);
    m_friendAsks &= myDecks();
    m_asking &= ~myDecks();
    if (announce) {
        sendSessionMessage("decks", static_cast<double>(m_hostMask), QString::number(at));
    }
    qInfo() << "B2B: decks now" << myDecksText() << "(host's:" << m_hostMask << ")";
    emit decksChanged();
}

void Manager::askForDeck(int deck) {
    const uint32_t bit = 1u << deck;
    if (!deckEach() || (myDecks() & bit) || (m_asking & bit)) {
        return;
    }
    m_asking |= bit;
    m_askTimer[deck].start();
    sendSessionMessage("deck_ask", deck);
    qInfo() << "B2B: asking the friend for deck" << deck + 1;
    emit decksChanged();
}

void Manager::answerDeckAsk(int deck, bool give) {
    const uint32_t bit = 1u << deck;
    if (!(m_friendAsks & bit)) {
        return;
    }
    m_friendAsks &= ~bit;
    if (give) {
        giveDeck(deck);
    } else {
        sendSessionMessage("deck_no", deck);
        emit decksChanged();
    }
}

void Manager::giveDeck(int deck) {
    const uint32_t bit = 1u << deck;
    if (!deckEach() || !(myDecks() & bit)) {
        return;
    }
    m_friendAsks &= ~bit;
    if (isHost()) {
        setHostDecks(m_hostMask & ~bit, contentNow() + ::b2b::fromMs(300), true);
    } else {
        sendSessionMessage("deck_release", deck); // the host keeps the list
    }
}

void Manager::setPlayMode(bool on, ::b2b::Nanos offset) {
    m_wantDeckEach = on;
    if (m_pUi) {
        m_pUi->showDeckEach(on);
    }
    if (on == deckEach()) {
        return;
    }
    qInfo() << "B2B:" << (on ? "playing a deck each" : "taking turns");
    m_adapter->requestMode(on ? EngineAdapter::Mode::DeckEach : EngineAdapter::Mode::TakeTurns, offset);
}

void Manager::sendPlayMode() {
    // With the host's timeline, which both use in "a deck each".
    const ::b2b::Nanos hostOffset = isHost() ? m_adapter->offset() : m_peerOffset;
    sendSessionMessage("mode", m_wantDeckEach ? 1.0 : 0.0, QString::number(hostOffset));
}

void Manager::updateLead() {
    // How long after a move both of you hear it, in "a deck each": long
    // enough for the move to reach the friend before then. One way across
    // the network (half a slow-ish round trip), plus however much sooner the
    // friend's sound card plays than ours, plus a little spare.
    const double outputMs = ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")));
    if (m_session && (m_sentOutputMs < 0 || std::fabs(outputMs - m_sentOutputMs) > 1)) {
        m_sentOutputMs = outputMs;
        sendSessionMessage("latency", outputMs);
    }
    if (!qgetenv("B2B_LEAD_MS").isEmpty()) {
        return; // fixed (testing)
    }
    if (!tightTiming()) {
        // Your decks play the moment you touch them, as on your own: none of
        // the delay below. Your friend hears your moves one trip across the
        // network later (a start or a jump lands where it should be, a knob
        // turn that much late). B2B_TIGHT_TIMING=1: the delay, so your moves
        // reach them on the dot.
        if (m_lead != 0) {
            m_lead = 0;
            m_adapter->setLead(0);
        }
        return;
    }
    const ::b2b::Nanos rtt = m_session->rttP90();
    if (rtt <= 0 || m_session->stats().pongsReceived.load() < 20 || m_peerOutputMs < 0) {
        return;
    }
    if (m_leadBoostMs > 0 && m_sinceLateReceived.isValid() && m_sinceLateReceived.elapsed() > 15000) {
        m_leadBoostMs = std::max(0.0, m_leadBoostMs - 2); // on time for a while: a little less
        m_sinceLateReceived.restart();
    }
    const double spareMs = envDouble("B2B_LEAD_SPARE_MS", 8) + m_leadBoostMs;
    const ::b2b::Nanos wanted = std::clamp<::b2b::Nanos>(
            rtt / 2 + ::b2b::fromMs(std::max(0.0, m_peerOutputMs - outputMs)) + ::b2b::fromMs(spareMs),
            ::b2b::fromMs(10),
            ::b2b::fromMs(200));
    // Up at once (moves arriving late are worse), down when it's worth it.
    if (wanted > m_lead + ::b2b::fromMs(2) || wanted < m_lead - ::b2b::fromMs(5)) {
        m_lead = wanted;
        m_adapter->setLead(wanted);
    }
}

::b2b::Nanos Manager::contentNow() const {
    // Events are stamped on the content timeline (shared clock minus our offset).
    return m_session->sharedNow() - (m_adapter ? m_adapter->offset() : 0);
}

void Manager::sendSessionMessage(const char* name, double value, const QString& text) {
    ::b2b::ControlEvent ev;
    ev.time = m_session->sharedNow();
    ev.key.assign(std::string("[B2B],") + name);
    ev.value = value;
    ev.text.assign(text.toStdString());
    m_session->sendEventFromMainThread(ev);
}

void Manager::sendSetEntry(const QByteArray& json) {
    // "+" and a piece, ..., "." and the last piece; all with the entry's
    // number. Bytes, so a piece may end inside a character.
    constexpr int kPiece = 48;
    const int serial = ++m_setSerial;
    for (int at = 0; at < json.size() || at == 0; at += kPiece) {
        const bool last = at + kPiece >= json.size();
        ::b2b::ControlEvent ev;
        ev.time = m_session->sharedNow();
        ev.key.assign(std::string("[B2B],set"));
        ev.value = serial;
        ev.text.assign((last ? std::string(".") : std::string("+")) + json.mid(at, kPiece).toStdString());
        m_session->sendEventFromMainThread(ev);
        if (last) {
            break;
        }
    }
}

void Manager::applyPrefs(const ::b2b::qt::HandoverPrefs& prefs) {
    const bool allowChanged = prefs.allowTake != m_allowTake;
    m_allowTake = prefs.allowTake;
    m_catchUpBars = std::clamp(prefs.catchUpBars, 1, 32);
    m_snapStyle = prefs.snap;
    // Environment overrides (for testing).
    if (!qgetenv("B2B_ALLOW_TAKE").isEmpty()) {
        m_allowTake = envDouble("B2B_ALLOW_TAKE", 0) > 0;
    }
    if (!qgetenv("B2B_CATCHUP_BARS").isEmpty()) {
        m_catchUpBars = std::clamp(static_cast<int>(envDouble("B2B_CATCHUP_BARS", 4)), 1, 32);
    }
    if (!qgetenv("B2B_HANDOVER_STYLE").isEmpty()) {
        m_snapStyle = envString("B2B_HANDOVER_STYLE").toLower() == QStringLiteral("snap");
    }
    bool wantDeckEach = prefs.deckEach;
    if (!qgetenv("B2B_PLAY_MODE").isEmpty()) {
        wantDeckEach = envString("B2B_PLAY_MODE") == QStringLiteral("deck_each"); // testing
    }
    if (wantDeckEach != m_wantDeckEach || m_starting) {
        m_wantDeckEach = wantDeckEach;
        if (!m_starting && m_session) {
            // Our choice: both switch (the latest choice of either side wins).
            setPlayMode(m_wantDeckEach, isHost() ? m_adapter->offset() : m_peerOffset);
            sendPlayMode();
        }
    }
    m_adapter->setCatchUpBars(m_catchUpBars);
    m_adapter->setHandoverStyle(m_snapStyle ? EngineAdapter::HandoverStyle::Snap : EngineAdapter::HandoverStyle::Smooth);
    if (allowChanged && m_session) {
        sendSessionMessage("allow", m_allowTake ? 1.0 : 0.0);
    }
}

void Manager::takeOrRequest() {
    if (isLive() || m_requestPending) {
        return;
    }
    m_requestPending = true;
    m_requestTimer.start();
    sendSessionMessage("request", 0);
    qInfo() << "B2B: asking to take control";
    updateUi();
}

void Manager::handOver() {
    if (!isLive()) {
        return;
    }
    qInfo() << "B2B: handing over to the friend";
    m_adapter->requestHandover();
}

void Manager::answerRequest(bool allow) {
    m_peerRequestOpen = false;
    if (allow && isLive()) {
        handOver();
    } else {
        sendSessionMessage("deny", 0);
    }
}

void Manager::handleSessionMessages() {
    ::b2b::ControlEvent ev;
    while (m_session->popMainEvent(ev)) {
        const std::string_view name = ev.key.view().substr(6); // after "[B2B],"
        if (name == "request") {
            if (!isLive() || deckEach()) {
                continue; // they asked us, but control has already moved
            }
            if (m_allowTake) {
                qInfo() << "B2B: friend is taking control";
                handOver();
            } else {
                qInfo() << "B2B: friend asks to take control";
                m_peerRequestOpen = true;
                if (m_pUi) {
                    m_pUi->showRequest();
                }
            }
        } else if (name == "deny") {
            if (m_requestPending) {
                m_requestPending = false;
                if (m_pUi) {
                    m_pUi->showMessage(tr("your friend said not yet."));
                }
            }
        } else if (name == "ptr") {
            emit pointerReceived(ev.value, QString::fromStdString(ev.text.str()));
        } else if (name == "bv") {
            emit browserViewReceived(ev.value, QString::fromStdString(ev.text.str()));
        } else if (name == "set") {
            const std::string piece = ev.text.str();
            const int serial = static_cast<int>(ev.value);
            if (!piece.empty()) {
                if (!m_setParts.contains(serial) && m_setParts.size() >= kMaxSetParts) {
                    m_setParts.clear(); // pieces that never ended: drop them, don't keep collecting
                }
                QByteArray& json = m_setParts[serial];
                json += QByteArray::fromStdString(piece.substr(1));
                if (piece[0] == '.' || json.size() > 4096) {
                    if (piece[0] == '.') {
                        emit setEntryReceived(json);
                    }
                    m_setParts.remove(serial);
                }
            }
        } else if (name == "allow") {
            m_peerAllowsTake = ev.value > 0.5;
        } else if (name == "fxslot" || name == "fxpreset") {
            applyEffectMessage(name == "fxpreset", QString::fromStdString(ev.text.str()));
        } else if (name == "grid") {
            applyPeerGrid(ev.value, QString::fromStdString(ev.text.str()));
        } else if (name == "mode") {
            setPlayMode(ev.value > 0.5, static_cast<::b2b::Nanos>(QString::fromStdString(ev.text.str()).toLongLong()));
        } else if (name == "reset" && !isHost()) {
            // (Only the host resets a room.)
            emit roomReset();
        } else if (name == "decks" && !isHost()) {
            setHostDecks(static_cast<uint32_t>(ev.value),
                    static_cast<::b2b::Nanos>(QString::fromStdString(ev.text.str()).toLongLong()), false);
        } else if (name == "name") {
            const QString friendName = QString::fromStdString(ev.text.str()).trimmed();
            if (friendName != m_friendName) {
                m_friendName = friendName;
                emit decksChanged(); // the players' titles
            }
        } else if (name == "deck_ask") {
            const int d = static_cast<int>(ev.value);
            if (d >= 0 && d < m_numDecks && (myDecks() & (1u << d))) {
                m_friendAsks |= 1u << d;
                if (m_pUi) {
                    m_pUi->showMessage(tr("your friend asks for deck %1. answer next to its title.").arg(d + 1));
                }
                emit decksChanged();
            }
        } else if (name == "deck_no") {
            const int d = static_cast<int>(ev.value);
            if (d >= 0 && d < m_numDecks && (m_asking & (1u << d))) {
                m_asking &= ~(1u << d);
                if (m_pUi) {
                    m_pUi->showMessage(tr("your friend is keeping deck %1 for now.").arg(d + 1));
                }
                emit decksChanged();
            }
        } else if (name == "deck_release" && isHost()) {
            const int d = static_cast<int>(ev.value);
            if (d >= 0 && d < m_numDecks) {
                setHostDecks(m_hostMask | (1u << d), contentNow() + ::b2b::fromMs(300), true);
            }
        } else if (name == "latency") {
            m_peerOutputMs = ev.value;
        } else if (name == "late") {
            // Our moves reached the friend after their moment: give them more time.
            // A little at a time, and not much in all: what's heard stays
            // in step anyway (a late start or jump is put where it should
            // be); only a knob or fader turn lands a few ms late. Every ms
            // here is a ms later you hear your own decks.
            // (No delay to grow when your decks play at once: then they're
            // a trip late by design.)
            const bool fixed = !qgetenv("B2B_LEAD_MS").isEmpty();
            if (tightTiming() || fixed) {
                m_leadBoostMs = std::min(m_leadBoostMs + std::min(ev.value, 10.0) + 1, kMaxLeadBoostMs);
                m_sinceLateReceived.restart();
                qInfo() << "B2B: the friend got moves" << ev.value << "ms late; timing now has"
                        << m_leadBoostMs << "ms extra";
            }
            if (fixed) {
                m_lead = ::b2b::fromMs(envDouble("B2B_LEAD_MS", 30) + m_leadBoostMs);
                m_adapter->setLead(m_lead);
            }
        } else if (name == "offset") {
            m_peerOffset = static_cast<::b2b::Nanos>(ev.value);
            m_adapter->setPeerOffset(m_peerOffset, ev.text == "live");
        }
    }
}

void Manager::handleNotices() {
    EngineAdapter::Notice n;
    while (m_adapter->popNotice(&n)) {
        switch (n.kind) {
        case EngineAdapter::Notice::BecameLive:
            qInfo() << "B2B: you are live";
            m_requestPending = false;
            if (m_pUi) {
                m_pUi->closeRequest();
            }
            if (m_pUi) {
                m_pUi->showMessage(tr("you have control."));
            }
            m_sentOffset = -1; // tell the friend our offset right away
            m_fxSent.clear();  // and which effects are loaded
            m_catchUpText.clear();
            // Whatever is loaded now is ours to announce.
            for (int d = 0; d < m_numDecks; ++d) {
                m_pendingLoad[d] = 0;
            }
            break;
        case EngineAdapter::Notice::BecameListener:
            if (n.durationSec > 0) {
                qInfo().noquote() << QStringLiteral("B2B: friend is live; easing %1 ms behind over %2 s")
                                             .arg(n.shiftMs, 0, 'f', 0)
                                             .arg(n.durationSec, 0, 'f', 1);
            } else {
                qInfo().noquote() << QStringLiteral("B2B: friend is live; moved %1 ms behind").arg(n.shiftMs, 0, 'f', 0);
            }
            m_peerRequestOpen = false;
            if (m_pUi) {
                m_pUi->closeRequest();
            }
            if (m_pUi) {
                m_pUi->showMessage(tr("your friend has control."));
            }
            m_sentOffset = -1;
            break;
        case EngineAdapter::Notice::ModeChanged:
            emit decksChanged();
            if (deckEach()) {
                if (m_pUi) {
                    m_pUi->showMessage(tr("playing a deck each. you have %1.").arg(myDecksText()));
                }
            } else if (m_pUi) {
                m_pUi->showMessage(isHost() ? tr("taking turns. you're live.") : tr("taking turns. your friend is live."));
            }
            m_sentOffset = -1;
            m_fxSent.clear();
            for (int d = 0; d < m_numDecks; ++d) {
                if (controlsDeck(d)) {
                    m_pendingLoad[d] = 0;
                    sendLoad(d); // what's on our decks
                }
            }
            break;
        case EngineAdapter::Notice::CatchUpDone:
            qInfo() << "B2B: caught up with the friend's timing";
            m_catchUpText.clear();
            break;
        }
        if (n.kind != EngineAdapter::Notice::CatchUpDone) {
            m_sinceRoleChange.restart();
            m_autoHandoverDone = false;
            m_autoRequestDone = false;
        }
    }
}

void Manager::updateListenDelay() {
    ::b2b::Nanos wanted;
    if (m_config.delayMs > 0) {
        wanted = ::b2b::fromMs(m_config.delayMs);
    } else {
        const ::b2b::Nanos rtt = m_session->rttP90();
        if (rtt <= 0 || m_session->stats().pongsReceived.load() < 20) {
            return; // not measured yet: keep the safe start value
        }
        // One way (half the slow-ish round trip), plus our own output
        // latency, plus room for one lost packet to be resent.
        const double outputMs = ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")));
        wanted = rtt / 2 + ::b2b::fromMs(outputMs) + ::b2b::fromMs(25);
        wanted = std::clamp<::b2b::Nanos>(wanted, ::b2b::fromMs(30), ::b2b::fromMs(500));
    }
    // Follow increases at once (late events are worse), decreases only when they matter.
    if (wanted > m_listenDelay + ::b2b::fromMs(5) || wanted < m_listenDelay - ::b2b::fromMs(15)) {
        m_listenDelay = wanted;
        m_adapter->setListenDelay(wanted);
    }
}

void Manager::updateUi() {
    if (!m_pUi) {
        return;
    }
    ::b2b::qt::UiState st;
    const ::b2b::Nanos heard = m_session->lastHeard();
    const bool lost = heard > 0 && m_clock->now() - heard > 3 * ::b2b::kNanosPerSec;
    const bool connected = m_session->peerGeneration() > 0 && m_session->ready() && !lost;
    st.phase = connected ? ::b2b::qt::UiState::Phase::Connected : ::b2b::qt::UiState::Phase::Waiting;
    st.live = isLive();
    st.deckEach = deckEach();
    if (st.deckEach) {
        st.myDecks = myDecksText();
        st.timingMs = ::b2b::toMs(m_adapter->lead());
    }
    st.peerAllowsTake = m_peerAllowsTake;
    st.requestPending = m_requestPending;
    st.listenDelayMs = ::b2b::toMs(m_adapter->offset() - m_peerOffset);
    st.rttMs = ::b2b::toMs(m_session->rttP90());
    st.where = isHost() ? tr("hosting on port %1").arg(m_config.port)
                        : tr("joined %1").arg(m_config.peerHost);
    if (codeMode()) {
        using State = ::b2b::RendezvousStatus::State;
        const ::b2b::RendezvousStatus rs = m_session->rendezvousStatus();
        const QString code = QString::fromStdString(rs.code);
        st.where = isHost() ? tr("hosting with code %1").arg(code) : tr("joined with code %1").arg(code);
        if (rs.state == State::Connected && rs.relayed) {
            st.where += tr(" · via relay");
        }
        if (isHost() && rs.state != State::Connected) {
            st.code = code;
            st.codeSecret = QString::fromStdString(rs.secret); // goes to the guest with the code, via the room
        }
        st.checkCode = QString::fromStdString(rs.checkCode);
        if (rs.state == State::Contacting) {
            st.where = isHost() ? tr("getting a join code") : tr("looking up %1").arg(code);
        } else if (rs.state == State::Punching) {
            st.where = tr("connecting to your friend");
            st.code.clear();
            st.codeSecret.clear();
        } else if (rs.state == State::Failed) {
            st.problem = QString::fromStdString(rs.error);
        }
    }
    if (lost) {
        st.where = tr("lost contact with your friend, still trying (%1)").arg(st.where);
    }
    if (!codeMode() && isHost() && !connected && !m_config.password.isEmpty()) {
        st.code = m_config.password; // hosting by address: the friend needs this
        st.codeIsPassword = true;
    }
    if (connected && m_session->encrypted()) {
        st.where += tr(" · encrypted");
    }
    if (!connected) {
        const QString theirs = QString::fromStdString(m_session->peerAppVersion());
        switch (m_session->linkProblem()) {
        case ::b2b::LinkProblem::PeerOutdated:
            st.problem = tr("your friend's app is out of date%1. they need to update to %2, then try again.")
                                 .arg(theirs.isEmpty() ? QString() : tr(" (%1)").arg(theirs),
                                         QString::fromLatin1(::b2b::kVersion));
            break;
        case ::b2b::LinkProblem::PeerNewer:
            st.problem = tr("your friend has a newer version (%1). update your app, then try again.").arg(theirs);
            break;
        case ::b2b::LinkProblem::WrongPassword:
            st.problem = codeMode() ? tr("couldn't set up a secure connection with your friend. try again.")
                                    : tr("the session password doesn't match. check it with your friend.");
            break;
        case ::b2b::LinkProblem::None:
            break;
        }
    }
    if (connected && st.rttMs > 400) {
        st.problem = tr("Your connection is struggling (round trip %1 ms): moves may arrive late. "
                        "A wired or steadier connection helps.")
                             .arg(st.rttMs, 0, 'f', 0);
    }
    QStringList activity;
    const double progress = m_adapter->catchUpProgress();
    if (progress >= 0) {
        activity << tr("easing into your friend's timing, %1%").arg(static_cast<int>(progress * 100));
    }
    for (auto it = m_requested.constBegin(); it != m_requested.constEnd(); ++it) {
        const QString name = m_peerNames.value(it.key(), fingerprintToHex(it.key()));
        activity << tr("receiving %1, %2%").arg(name).arg(m_progress.value(it.key(), 0));
    }
    st.activity = activity.join(QStringLiteral(" · "));
    m_pUi->setState(st);
}

namespace {

// The effect chains we keep in step, with short codes for the messages.
QList<QPair<QString, EffectChainPointer>> effectChains(
        const std::shared_ptr<EffectsManager>& pManager, const std::vector<QString>& deckGroups) {
    QList<QPair<QString, EffectChainPointer>> chains;
    if (!pManager) {
        return chains;
    }
    for (int u = 0; u < 4; ++u) {
        chains.append({QStringLiteral("U%1").arg(u + 1), pManager->getStandardEffectChain(u)});
    }
    for (size_t d = 0; d < deckGroups.size(); ++d) {
        chains.append({QStringLiteral("Q%1").arg(d + 1), pManager->getQuickEffectChain(deckGroups[d])});
    }
    return chains;
}

QString slotEffectId(const EffectSlotPointer& pSlot) {
    const EffectManifestPointer pManifest = pSlot ? pSlot->getManifest() : EffectManifestPointer();
    return pSlot && pSlot->isLoaded() && pManifest ? pManifest->uniqueId() : QString();
}

} // namespace

void Manager::syncEffects() {
    // Live side: announce which effect is in every slot (and each quick
    // effect's preset), when it changes and every few seconds anyway.
    // (A deck each: each deck's quick effect by its owner, the units by the host.)
    if (!isLive() && !deckEach()) {
        return;
    }
    const bool resendAll = !m_sinceFxResend.isValid() || m_sinceFxResend.elapsed() > 3000;
    if (resendAll) {
        m_sinceFxResend.restart();
    }
    for (const auto& [code, pChain] : effectChains(m_pEffectsManager, m_deckGroups)) {
        if (!pChain || !announcesEffects(code)) {
            continue;
        }
        if (code.startsWith(QLatin1Char('Q'))) {
            const QString key = code + QStringLiteral("/preset");
            const QString preset = pChain->presetName();
            if (resendAll || m_fxSent.value(key) != preset) {
                m_fxSent.insert(key, preset);
                sendSessionMessage("fxpreset", 0, code + QLatin1Char('/') + preset);
            }
        }
        const auto& effectSlots = pChain->getEffectSlots();
        for (int i = 0; i < effectSlots.size(); ++i) {
            const QString key = code + QLatin1Char('/') + QString::number(i);
            const QString id = slotEffectId(effectSlots[i]);
            if (resendAll || m_fxSent.value(key, QStringLiteral("?")) != id) {
                m_fxSent.insert(key, id);
                sendSessionMessage("fxslot", 0, key + QLatin1Char('/') + id);
            }
        }
    }
}

void Manager::applyEffectMessage(bool preset, const QString& text) {
    if (!m_pEffectsManager) {
        return;
    }
    // "U1/0/<effect id>" or "Q1/<preset name>"
    const int first = text.indexOf(QLatin1Char('/'));
    if (first < 0) {
        return;
    }
    const QString code = text.left(first);
    if (announcesEffects(code)) {
        return; // ours are what count
    }
    EffectChainPointer pChain;
    for (const auto& [c, p] : effectChains(m_pEffectsManager, m_deckGroups)) {
        if (c == code) {
            pChain = p;
        }
    }
    if (!pChain) {
        return;
    }
    if (preset) {
        const QString name = text.mid(first + 1);
        if (pChain->presetName() != name) {
            if (auto pPreset = m_pEffectsManager->getChainPresetManager()->getPreset(name)) {
                qInfo() << "B2B: friend chose quick effect" << name << "on" << pChain->getGroup();
                pChain->loadChainPreset(pPreset);
            }
        }
        return;
    }
    const int second = text.indexOf(QLatin1Char('/'), first + 1);
    if (second < 0) {
        return;
    }
    const int index = text.mid(first + 1, second - first - 1).toInt();
    const QString id = text.mid(second + 1);
    const EffectSlotPointer pSlot = pChain->getEffectSlots().value(index);
    if (!pSlot || slotEffectId(pSlot) == id) {
        return;
    }
    EffectManifestPointer pManifest;
    if (!id.isEmpty()) {
        pManifest = m_pEffectsManager->getBackendManager()->getManifestFromUniqueId(id);
        if (!pManifest) {
            qWarning() << "B2B: friend loaded effect" << id << "which this Mixxx doesn't have";
            return;
        }
    }
    qInfo() << "B2B: friend loaded" << (id.isEmpty() ? QStringLiteral("no effect") : id) << "in" << pSlot->getGroup();
    pSlot->loadEffectWithDefaults(pManifest);
}

void Manager::runAutopilotTurns() {
    // Testing only: scripted handovers.
    const double startDelay = envDouble("B2B_AUTOPILOT_DELAY_S", 10);
    const double sinceStart = m_sinceStart.elapsed() / 1000.0 - startDelay;
    if (sinceStart < 0) {
        return;
    }
    const double inRole = std::min(m_sinceRoleChange.elapsed() / 1000.0, sinceStart);
    if (isLive() && m_config.autopilotHandoverSec > 0 && !m_autoHandoverDone &&
            inRole >= m_config.autopilotHandoverSec) {
        m_autoHandoverDone = true;
        qInfo() << "B2B autopilot: handing over";
        handOver();
    }
    if (!isLive() && m_config.autopilotRequestSec > 0 && !m_requestPending && !m_autoRequestDone &&
            inRole >= m_config.autopilotRequestSec) {
        m_autoRequestDone = true;
        qInfo() << "B2B autopilot: asking for control";
        takeOrRequest();
    }
}

void Manager::tickRendezvous() {
    if (!codeMode()) {
        return;
    }
    const ::b2b::RendezvousStatus rs = m_session->rendezvousStatus();
    using State = ::b2b::RendezvousStatus::State;
    if (isHost() && !rs.code.empty() && !m_codeWritten) {
        m_codeWritten = true;
        // Neither the code nor our address: logs go out in bug reports.
        qInfo().noquote() << "B2B: got a join code from the server";
        const QString file = envString("B2B_CODE_FILE"); // testing
        if (!file.isEmpty()) {
            QFile f(file);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(rs.code.c_str());
            }
        }
    }
    if (rs.state == State::Connected) {
        if (m_connectedTicks == 0) {
            qInfo().noquote() << "B2B: connected to the peer"
                              << (rs.relayed ? "through the server's relay" : "directly");
        }
        ++m_connectedTicks;
    } else {
        m_connectedTicks = 0;
    }
    if (rs.state == State::Failed && !rs.retrying && !m_failReported) {
        m_failReported = true;
        qWarning().noquote() << "B2B:" << QString::fromStdString(rs.error);
        emit failed(QString::fromStdString(rs.error));
    }
}

void Manager::setMyName(const QString& name) {
    if (name == m_myName) {
        return;
    }
    qInfo() << "B2B: my name for the decks' titles:" << name;
    m_myName = name;
    m_sinceNameSent.invalidate(); // tell the friend at once
}

QString Manager::deckPlayerName(int deck) const {
    return deckIsMine(deck) ? m_myName : m_friendName;
}

bool Manager::deckIsMine(int deck) const {
    return deckEach() ? m_adapter->ownsDeck(deck) : isLive();
}

void Manager::slotTick() {
    ++m_tickCount;
    tickRendezvous();
    // Our name for the friend's titles, now and then (they may reconnect).
    if (!m_myName.isEmpty() && (!m_sinceNameSent.isValid() || m_sinceNameSent.elapsed() > 5000)) {
        m_sinceNameSent.start();
        sendSessionMessage("name", 0, m_myName.left(40));
    }

    handleSessionMessages();
    handleNotices();
    updateListenDelay();
    updateLead();

    const bool live = isLive();
    m_lastLive = live;
    if (m_autopilot) {
        m_autopilot->tick(live);
    }
    runAutopilotTurns();
    syncEffects();

    // The peer (re)connected: roles start afresh with the host live, and
    // each side tells the other its settings.
    const uint32_t generation = m_session->peerGeneration();
    if (generation != m_seenPeerGeneration) {
        m_seenPeerGeneration = generation;
        m_adapter->requestReset(isHost());
        m_requestPending = false;
        m_peerRequestOpen = false;
        if (m_pUi) {
            m_pUi->closeRequest();
        }
        m_sentOffset = -1;
        m_fxSent.clear();
        m_sentOutputMs = -1;
        sendSessionMessage("allow", m_allowTake ? 1.0 : 0.0);
        m_friendAsks = 0;
        m_asking = 0;
        if (isHost()) {
            sendPlayMode(); // the host's choice when you connect
            setPlayMode(m_wantDeckEach, m_adapter->offset());
            setHostDecks(m_hostMask, contentNow(), true);
        }
        if (isHost()) {
            for (int d = 0; d < m_numDecks; ++d) {
                sendLoad(d);
            }
        } else if (deckEach()) {
            for (int d = 0; d < m_numDecks; ++d) {
                if (controlsDeck(d)) {
                    sendLoad(d);
                }
            }
        }
    }

    // A deck each: moves from the friend arriving after their moment mean
    // the timing is too tight for the connection (it grows by itself).
    const uint64_t late = m_adapter->lateEvents();
    if (late > m_lateSeen) {
        // Tell the friend (now and then), so their timing grows to fit.
        if (deckEach() && m_lateSeen > 0 && (!m_sinceLateSent.isValid() || m_sinceLateSent.elapsed() > 5000)) {
            m_sinceLateSent.restart();
            sendSessionMessage("late", std::min(m_adapter->lastLateMs(), 50.0));
        }
        if (m_lateSeen > 0 && (!m_sinceLateWarning.isValid() || m_sinceLateWarning.elapsed() > 10000)) {
            m_sinceLateWarning.restart();
            qInfo() << "B2B:" << (late - m_lateSeen) << "moves from the friend arrived late, latest by"
                    << m_adapter->lastLateMs() << "ms";
        }
        m_lateSeen = late;
    }

    // Unanswered deck requests: the host takes the deck after 30 s (the
    // friend may have stepped away); a guest's request just lapses.
    for (int d = 0; d < m_numDecks; ++d) {
        const uint32_t bit = 1u << d;
        if ((m_asking & bit) && m_askTimer[d].elapsed() > 30000) {
            m_asking &= ~bit;
            if (isHost() && deckEach()) {
                qInfo() << "B2B: no answer about deck" << d + 1 << "- the host takes it";
                setHostDecks(m_hostMask | bit, contentNow() + ::b2b::fromMs(300), true);
            } else {
                if (m_pUi) {
                    m_pUi->showMessage(tr("no answer about deck %1.").arg(d + 1));
                }
                emit decksChanged();
            }
        }
    }

    // Tell the friend our content offset when it changes (they need it to
    // know how far behind us to listen after taking over from... us).
    const ::b2b::Nanos offset = m_adapter->offset();
    if (m_sentOffset < 0 ||
            (std::llabs(offset - m_sentOffset) > ::b2b::fromMs(1) && m_sinceOffsetSent.elapsed() > 250)) {
        m_sentOffset = offset;
        m_sinceOffsetSent.restart();
        sendSessionMessage("offset", static_cast<double>(offset),
                isLive() ? QStringLiteral("live") : QStringLiteral("listening"));
    }

    if (m_requestPending && m_requestTimer.elapsed() > 20000) {
        m_requestPending = false; // no answer
        if (m_pUi) {
            m_pUi->showMessage(tr("no answer from your friend."));
        }
    }

    EngineAdapter::LoadRequest request;
    while (m_adapter->popLoadRequest(&request)) {
        if (request.deck >= 0 && request.deck < m_numDecks && request.trackId != 0 &&
                request.trackId != m_deckTrackId[request.deck]) {
            m_pendingLoad[request.deck] = request.trackId;
        }
    }
    tryPendingLoads();
    if (m_indexDone && !m_wantLater.isEmpty()) {
        const auto later = m_wantLater;
        m_wantLater.clear();
        for (auto it = later.constBegin(); it != later.constEnd(); ++it) {
            onPeerHint(it.key(), it.value());
        }
    }

    if (m_log && !live && m_tickCount % 13 == 0) {
        const ::b2b::ClockSync& cs = m_session->clockSync();
        if (cs.hasEstimate() && !isHost()) {
            m_log->add({'S', cs.sampleCount(), 0, 0, ::b2b::toMs(cs.bestRtt()), ::b2b::trueNow(),
                    static_cast<double>(cs.offset())});
        }
    }

    if (m_tickCount % 10 == 0) {
        updateUi();
    }
    if (m_tickCount % (2000 / kTickMs) == 0) {
        printStatus();
    }
    if (m_config.logAfterSec > 0 && !m_logWritten && m_sinceStart.elapsed() >= m_config.logAfterSec * 1000) {
        writeLog();
    }
}

void Manager::printStatus() {
    if (m_share && (!m_progress.isEmpty() || !m_requested.isEmpty())) {
        QStringList parts;
        for (auto it = m_requested.constBegin(); it != m_requested.constEnd(); ++it) {
            parts << QStringLiteral("%1 %2%").arg(fingerprintToHex(it.key())).arg(m_progress.value(it.key(), 0));
        }
        qInfo().noquote() << QStringLiteral("B2B receiving: ") + parts.join(QStringLiteral(", "));
    }
    if (m_share && m_tickCount % (10000 / kTickMs) == 0) {
        const auto& ss = m_share->stats();
        qInfo().noquote() << QStringLiteral("B2B sharing: %1, peer offers %2 tracks, sent %3 files (%4 MB), received %5 files (%6 MB)")
                                     .arg(m_shareConnected ? QStringLiteral("connected") : QStringLiteral("waiting for peer"))
                                     .arg(m_peerTrackCount)
                                     .arg(ss.filesSent.load())
                                     .arg(ss.bytesSent.load() / 1e6, 0, 'f', 1)
                                     .arg(ss.filesReceived.load())
                                     .arg(ss.bytesReceived.load() / 1e6, 0, 'f', 1);
    }
    const ::b2b::SessionStats& st = m_session->stats();
    const ::b2b::ClockSync& cs = m_session->clockSync();
    qInfo().noquote() << QStringLiteral(
            "B2B %1 (%2): offset %3 ms, listen delay %4 ms, rtt p90 %5 ms, clock %6, "
            "events sent %7 / applied %8, corrections %9, undone %10, prep %11, resynced %12, "
            "resends %13, superseded %14, decrypt failures %15")
                                 .arg(deckEach() ? QStringLiteral("DECK EACH (lead %1 ms, late %2)")
                                                              .arg(::b2b::toMs(m_adapter->lead()), 0, 'f', 1)
                                                              .arg(m_adapter->lateEvents())
                                                : isLive() ? QStringLiteral("LIVE") : QStringLiteral("listening"))
                                 .arg(isHost() ? QStringLiteral("host") : QStringLiteral("guest"))
                                 .arg(::b2b::toMs(m_adapter->offset()), 0, 'f', 1)
                                 .arg(::b2b::toMs(m_listenDelay), 0, 'f', 1)
                                 .arg(::b2b::toMs(m_session->rttP90()), 0, 'f', 1)
                                 .arg(isHost() ? QStringLiteral("reference")
                                               : (cs.hasEstimate() ? QStringLiteral("synced %1 ms").arg(::b2b::toMs(cs.offset()), 0, 'f', 3)
                                                                   : QStringLiteral("syncing")))
                                 .arg(st.eventsSent.load())
                                 .arg(m_adapter->appliedEvents())
                                 .arg(m_adapter->corrections())
                                 .arg(m_adapter->overrides())
                                 .arg(m_adapter->prepChanges())
                                 .arg(m_adapter->resyncs())
                                 .arg(st.eventResends.load())
                                 .arg(st.eventsSuperseded.load())
                                 .arg(st.authFailures.load());
}

} // namespace mixxx::b2b

#include "moc_b2bmanager.cpp"
