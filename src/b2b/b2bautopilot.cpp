#include "b2b/b2bautopilot.h"

#include <QDebug>
#include <QStringList>
#include <algorithm>
#include <random>

#include "control/controlobject.h"
#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "track/beats.h"
#include "track/track.h"

namespace mixxx::b2b {

Autopilot::Autopilot(PlayerManager* pPlayerManager, const QString& spec, int numDecks, double startDelaySec)
        : m_pPlayerManager(pPlayerManager),
          m_startDelaySec(startDelaySec) {
    QStringList tracks = spec.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    m_guest = !tracks.isEmpty() && tracks.first() == QStringLiteral("moves");
    // "A deck each" test: together-host|A plays deck 1, together-guest|B
    // deck 2 (synced to deck 1), and both make moves at set times, whoever
    // is "live". Both record what they hear.
    if (!tracks.isEmpty() && tracks.first().startsWith(QStringLiteral("together"))) {
        m_together = true;
        const bool host = tracks.first() == QStringLiteral("together-host");
        const QString deck = host ? QStringLiteral("[Channel1]") : QStringLiteral("[Channel2]");
        const QString eq = QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deck);
        const QString filter = QStringLiteral("[QuickEffectRack1_%1]").arg(deck);
        const QString track = tracks.value(1);
        auto move = [this](double t, const QString& group, const QString& item, double value) {
            at(t, [=] {
                qInfo().noquote() << QStringLiteral("B2B autopilot move: %1,%2 = %3").arg(group, item).arg(value);
                set(group, item, value);
            });
        };
        at(1.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(track, deck, false); });
        move(2.0, deck, QStringLiteral("quantize"), 1.0);
        for (double t : {7.0, 10.0, 12.0, 20.0, 30.0, 40.0}) {
            at(t, [=] {
                auto get = [deck](const char* item) {
                    return ControlObject::get(ConfigKey(deck, QString::fromLatin1(item)));
                };
                qInfo().noquote() << QStringLiteral("B2B autopilot deck check: %1 play %2 rate %3 rate_ratio %4 "
                                                    "bpm %5 sync_enabled %6 sync_mode %7 sync_leader %8 "
                                                    "scratch2_enable %9 wheel %10 keylock %11")
                                             .arg(deck)
                                             .arg(get("play"))
                                             .arg(get("rate"))
                                             .arg(get("rate_ratio"))
                                             .arg(get("bpm"))
                                             .arg(get("sync_enabled"))
                                             .arg(get("sync_mode"))
                                             .arg(get("sync_leader"))
                                             .arg(get("scratch2_enable"))
                                             .arg(get("wheel"))
                                             .arg(get("keylock"));
            });
        }
        if (qEnvironmentVariableIsSet("B2B_LOG_VU")) {
            // The decks' meters as the skin sees them, ~30 times a second.
            for (double t = 20.0; t < 23.0; t += 0.03) {
                at(t, [] {
                    qInfo().noquote() << QStringLiteral("B2B autopilot vu: %1 %2 %3 %4")
                                                 .arg(ControlObject::get(ConfigKey(QStringLiteral("[Channel1]"),
                                                         QStringLiteral("vu_meter"))))
                                                 .arg(ControlObject::get(ConfigKey(QStringLiteral("[Channel2]"),
                                                         QStringLiteral("vu_meter"))))
                                                 .arg(ControlObject::get(ConfigKey(QStringLiteral("[Channel1]"),
                                                         QStringLiteral("play"))))
                                                 .arg(ControlObject::get(ConfigKey(QStringLiteral("[Channel2]"),
                                                         QStringLiteral("play"))));
                });
            }
        }
        move(4.0, QStringLiteral("[Recording]"), QStringLiteral("toggle_recording"), 1.0);
        // Stopped before the end, so the recordings are whole files.
        move(45.5, QStringLiteral("[Recording]"), QStringLiteral("toggle_recording"), 0.0);
        if (host) {
            move(6.0, deck, QStringLiteral("play"), 1.0);
            move(14.0, eq, QStringLiteral("parameter2"), 0.0);  // mid kill
            move(16.0, eq, QStringLiteral("parameter2"), 1.0);
            move(20.0, deck, QStringLiteral("volume"), 0.3);
            move(22.0, deck, QStringLiteral("volume"), 1.0);
            move(26.0, QStringLiteral("[Master]"), QStringLiteral("crossfader"), -1.0);
            move(30.0, QStringLiteral("[Master]"), QStringLiteral("crossfader"), 0.0);
            move(34.0, eq, QStringLiteral("parameter2"), 0.0);
            move(36.0, eq, QStringLiteral("parameter2"), 1.0);
        } else {
            if (qEnvironmentVariableIsSet("B2B_TOGETHER_QUANTIZE")) {
                // Started by hand, off the beat: quantize lines it up.
                move(9.37, deck, QStringLiteral("play"), 1.0);
            } else {
                move(8.0, deck, QStringLiteral("sync_enabled"), 1.0);
                move(9.0, deck, QStringLiteral("play"), 1.0);
            }
            if (qEnvironmentVariableIsSet("B2B_TOGETHER_FILTER")) {
                // A filter knob turned slowly (the host's deck, then ours),
                // read back before each step: it must stay where it's put.
                for (const QString& g : {QStringLiteral("[QuickEffectRack1_[Channel1]]"), filter}) {
                    const double t0 = g == filter ? 37.0 : 30.0;
                    for (int i = 0; i <= 75; ++i) {
                        const double v = 0.5 - 0.4 * i / 75.0;
                        at(t0 + i * 0.02, [g, v, i] {
                            const double was = ControlObject::get(ConfigKey(g, QStringLiteral("super1")));
                            qInfo().noquote() << QStringLiteral("B2B autopilot filter %1 step %2 was %3 set %4")
                                                         .arg(g).arg(i).arg(was, 0, 'f', 4).arg(v, 0, 'f', 4);
                            set(g, QStringLiteral("super1"), v);
                        });
                    }
                    for (int i = 0; i < 40; ++i) {
                        at(t0 + 1.6 + i * 0.05, [g] {
                            qInfo().noquote() << QStringLiteral("B2B autopilot filter %1 after %2")
                                                         .arg(g).arg(ControlObject::get(ConfigKey(g, QStringLiteral("super1"))), 0, 'f', 4);
                        });
                    }
                }
            }
            if (qEnvironmentVariableIsSet("B2B_TOGETHER_CUE")) {
                // CDJ CUE: back to the cue point, then tapped in time.
                const double beat = 60.0 / 107.167;
                for (int i = 0; i < 6; ++i) {
                    const double t = 36.0 + i * 2 * beat;
                    move(t, deck, QStringLiteral("cue_default"), 1.0);
                    move(t + (i == 0 ? 0.05 : 0.2), deck, QStringLiteral("cue_default"), 0.0);
                    at(t + 0.5, [deck] {
                        qInfo().noquote() << QStringLiteral("B2B autopilot cue check: cue_point %1 position %2 play %3")
                                                     .arg(ControlObject::get(ConfigKey(deck, QStringLiteral("cue_point"))))
                                                     .arg(ControlObject::get(ConfigKey(deck, QStringLiteral("playposition"))) *
                                                             ControlObject::get(ConfigKey(deck, QStringLiteral("track_samples"))))
                                                     .arg(ControlObject::get(ConfigKey(deck, QStringLiteral("play"))));
                    });
                }
            }
            if (qEnvironmentVariableIsSet("B2B_TOGETHER_PAUSE")) {
                // Paused, and in again by hand (quantize lines it up).
                move(33.0, deck, QStringLiteral("play"), 0.0);
                move(34.6, deck, QStringLiteral("play"), 1.0);
            }
            move(18.0, eq, QStringLiteral("parameter3"), 0.0); // high kill
            move(19.5, eq, QStringLiteral("parameter3"), 1.0);
            move(24.0, filter, QStringLiteral("super1"), 0.15);
            move(25.0, filter, QStringLiteral("super1"), 0.5);
            move(28.0, QStringLiteral("[Master]"), QStringLiteral("crossfader"), 1.0);
            move(29.0, QStringLiteral("[Master]"), QStringLiteral("crossfader"), 0.0);
            // The host's channel: its mixer row is anyone's.
            const QString hostEq = QStringLiteral("[EqualizerRack1_[Channel1]_Effect1]");
            move(31.0, hostEq, QStringLiteral("parameter1"), 0.0); // low kill on deck 1
            move(32.0, hostEq, QStringLiteral("parameter1"), 1.0);
            move(41.0, QStringLiteral("[Channel1]"), QStringLiteral("volume"), 0.2);
            move(42.0, QStringLiteral("[Channel1]"), QStringLiteral("volume"), 1.0);
            move(38.0, eq, QStringLiteral("parameter3"), 0.0); // high kill
            move(40.0, eq, QStringLiteral("parameter3"), 1.0);
        }
        move(44.0, QStringLiteral("[Recording]"), QStringLiteral("toggle_recording"), 0.0);
        move(44.2, QStringLiteral("[Recording]"), QStringLiteral("toggle_recording"), 1.0); // pressed again: stop
        std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) {
            return a.atSec < b.atSec;
        });
        m_elapsed.start();
        return;
    }
    const QString d1 = QStringLiteral("[Channel1]");
    const QString d2 = QStringLiteral("[Channel2]");
    const QString eq1 = QStringLiteral("[EqualizerRack1_[Channel1]_Effect1]");
    const QString eq2 = QStringLiteral("[EqualizerRack1_[Channel2]_Effect1]");
    const QString qfx1 = QStringLiteral("[QuickEffectRack1_[Channel1]]");
    const QString master = QStringLiteral("[Master]");

    m_sinceCreated.start();
    m_listenStartDelaySec = startDelaySec;
    auto report = [d2, master](const QString& what) {
        auto get = [](const QString& g, const QString& i) { return ControlObject::get(ConfigKey(g, i)); };
        qInfo().noquote() << QStringLiteral("B2B autopilot check (%1): deck2 sync_enabled %2 sync_mode %3 "
                                            "volume %4 eq-low %5 playing %6, crossfader %7")
                                     .arg(what)
                                     .arg(get(d2, QStringLiteral("sync_enabled")))
                                     .arg(get(d2, QStringLiteral("sync_mode")))
                                     .arg(get(d2, QStringLiteral("volume")))
                                     .arg(get(QStringLiteral("[EqualizerRack1_[Channel2]_Effect1]"),
                                             QStringLiteral("parameter1")))
                                     .arg(get(d2, QStringLiteral("play")))
                                     .arg(get(master, QStringLiteral("crossfader")));
    };
    auto fxReport = [](const QString& what) {
        auto get = [](const QString& g, const QString& i) { return ControlObject::get(ConfigKey(g, i)); };
        const QString u1 = QStringLiteral("[EffectRack1_EffectUnit1]");
        const QString s1 = QStringLiteral("[EffectRack1_EffectUnit1_Effect1]");
        const QString q2 = QStringLiteral("[QuickEffectRack1_[Channel2]]");
        qInfo().noquote() << QStringLiteral("B2B autopilot fx check (%1): unit1 deck1 %2 mix %3, slot1 effect %4 "
                                            "meta %5 p1 %6, deck2 quick preset %7 super %8")
                                     .arg(what)
                                     .arg(get(u1, QStringLiteral("group_[Channel1]_enable")))
                                     .arg(get(u1, QStringLiteral("mix")))
                                     .arg(get(s1, QStringLiteral("loaded_effect")))
                                     .arg(get(s1, QStringLiteral("meta")))
                                     .arg(get(s1, QStringLiteral("parameter1")))
                                     .arg(get(q2, QStringLiteral("loaded_chain_preset")))
                                     .arg(get(q2, QStringLiteral("super1")));
    };
    auto loopReport = [d1](const QString& what) {
        auto get = [d1](const char* item) { return ControlObject::get(ConfigKey(d1, QString::fromLatin1(item))); };
        qInfo().noquote() << QStringLiteral("B2B autopilot loop check (%1): deck1 loop_enabled %2 start %3 end %4")
                                     .arg(what)
                                     .arg(get("loop_enabled"))
                                     .arg(get("loop_start_position"), 0, 'f', 0)
                                     .arg(get("loop_end_position"), 0, 'f', 0);
    };
    // CUE test (B2B_CUE_TEST=1): presses CUE repeatedly on a stopped deck 3
    // holding track A, first on the live side, then on the listening side.
    // The cue point must never move, and each release must return to it.
    const bool cueTest = !qgetenv("B2B_CUE_TEST").isEmpty();
    auto cueReport = [](const QString& deck, const QString& what) {
        auto get = [deck](const char* item) { return ControlObject::get(ConfigKey(deck, QString::fromLatin1(item))); };
        const double rate = get("track_samplerate");
        const double cueSec = rate > 0 && get("cue_point") >= 0 ? get("cue_point") / (2.0 * rate) : -1.0;
        qInfo().noquote() << QStringLiteral("B2B autopilot cue check (%1): %5 cue %2 s, position %3 s, play %4")
                                     .arg(what)
                                     .arg(cueSec, 0, 'f', 3)
                                     .arg(get("playposition") * get("duration"), 0, 'f', 3)
                                     .arg(get("play"))
                                     .arg(deck);
    };
    auto pressCue = [this, cueReport](const QString& deck, bool listening, double sec, int times, const QString& who) {
        for (int i = 0; i < times; ++i) {
            const double t = sec + i * 0.6;
            auto step = [this, listening](double when, std::function<void()> f) {
                if (listening) {
                    whileListening(when, std::move(f));
                } else {
                    at(when, std::move(f));
                }
            };
            step(t, [=] { set(deck, QStringLiteral("cue_default"), 1.0); });
            step(t + 0.3, [=] { set(deck, QStringLiteral("cue_default"), 0.0); });
            step(t + 0.5, [=] { cueReport(deck, who + QStringLiteral(", after press %1").arg(i + 1)); });
        }
    };
    // Decks 3 and 4 are mirrored too: the host plays deck 3 for a while.
    const QString d3 = QStringLiteral("[Channel3]");
    auto deck3Report = [d3](const QString& what) {
        auto get = [d3](const char* item) { return ControlObject::get(ConfigKey(d3, QString::fromLatin1(item))); };
        qInfo().noquote() << QStringLiteral("B2B autopilot deck3 check (%1): play %2 volume %3 position %4 s")
                                     .arg(what)
                                     .arg(get("play"))
                                     .arg(get("volume"), 0, 'f', 2)
                                     .arg(get("playposition") * get("duration"), 0, 'f', 2);
    };
    // Grid test (B2B_GRID_TEST=1): the host fixes deck 1's beat grid by hand
    // while live, then the guest (listening) adjusts the same track, which
    // it borrowed from the host. Each side logs its grid; they must agree.
    const bool gridTest = !qgetenv("B2B_GRID_TEST").isEmpty();
    auto gridReport = [this](const QString& what) {
        const TrackPointer pTrack = m_pPlayerManager->getDeck(0)->getLoadedTrack();
        const mixxx::BeatsPointer pBeats = pTrack ? pTrack->getBeats() : mixxx::BeatsPointer();
        qInfo().noquote() << QStringLiteral("B2B autopilot grid check (%1): deck1 bpm %2 first beat %3 locked %4")
                                     .arg(what)
                                     .arg(pTrack ? pTrack->getBpm() : 0.0, 0, 'f', 4)
                                     .arg(pBeats ? pBeats->firstBeat().value() : -1.0, 0, 'f', 2)
                                     .arg(pTrack && pTrack->isBpmLocked());
    };
    auto press = [this](double t, bool listening, const QString& group, const QString& item) {
        auto step = [this, listening](double when, std::function<void()> f) {
            listening ? whileListening(when, std::move(f)) : at(when, std::move(f));
        };
        step(t, [=] { set(group, item, 1.0); });
        step(t + 0.1, [=] { set(group, item, 0.0); });
    };
    if (gridTest) {
        if (m_guest) {
            whileListening(24.0, [=] { gridReport(QStringLiteral("guest, after the host's fix")); });
            for (int i = 0; i < 3; ++i) {
                press(25.0 + i * 0.6, true, d1, QStringLiteral("beats_adjust_slower"));
            }
            whileListening(28.0, [=] { gridReport(QStringLiteral("guest, after its own edit")); });
        } else {
            at(1.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(0), d1, false); });
            at(6.0, [=] { gridReport(QStringLiteral("host, analysed")); });
            for (int i = 0; i < 4; ++i) {
                press(7.0 + i * 0.6, false, d1, QStringLiteral("beats_translate_later"));
            }
            press(10.0, false, d1, QStringLiteral("beats_adjust_faster"));
            at(11.0, [=] { gridReport(QStringLiteral("host, after its fix")); });
            at(32.0, [=] { gridReport(QStringLiteral("host, after the guest's edit")); });
        }
        std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) {
            return a.atSec < b.atSec;
        });
        std::stable_sort(m_listenSteps.begin(), m_listenSteps.end(), [](const Step& a, const Step& b) {
            return a.atSec < b.atSec;
        });
        m_elapsed.start();
        return;
    }
    if (m_guest && !cueTest) {
        whileListening(43.0, [=] { deck3Report(QStringLiteral("guest listening")); });
    }
    if (m_guest) {
        if (cueTest) {
            // While the host has both decks stopped: press CUE on deck 2.
            whileListening(19.5, [=] { cueReport(d2, QStringLiteral("guest listening, before")); });
            pressCue(d2, true, 20.0, 6, QStringLiteral("guest listening"));
            // After the host hands over (~30 s): live, quantize on, deck 1
            // playing, cue deck 2 over and over.
            at(1.0, [=] {
                set(d2, QStringLiteral("cue_gotoandstop"), 1.0); // back to its cue point first
                set(d1, QStringLiteral("quantize"), 1.0);
                set(d2, QStringLiteral("quantize"), 1.0);
                set(d1, QStringLiteral("play"), 1.0);
            });
            at(2.5, [=] { cueReport(d2, QStringLiteral("guest live, before")); });
            pressCue(d2, false, 3.0, 10, QStringLiteral("guest live"));
            std::stable_sort(m_listenSteps.begin(), m_listenSteps.end(), [](const Step& a, const Step& b) {
                return a.atSec < b.atSec;
            });
            std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) {
                return a.atSec < b.atSec;
            });
            return;
        }
        whileListening(27.0, [=] { fxReport(QStringLiteral("guest listening")); });
        // The host loops deck 1 from 20 s to 25 s (we start ~2 s after it).
        whileListening(20.5, [=] { loopReport(QStringLiteral("guest listening, loop on")); });
        whileListening(24.5, [=] { loopReport(QStringLiteral("guest listening, loop off")); });
        // Guest, while still listening: prepare deck 2 before the host
        // starts it (load a track only we have, set volume and EQ), and try
        // to move the crossfader (must be undone).
        if (tracks.size() > 1) {
            whileListening(3.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(1), d2, false); });
            whileListening(4.0, [=] { set(d2, QStringLiteral("volume"), 0.42); });
            whileListening(6.0, [=] { set(d2, QStringLiteral("pregain"), 1.3); }); // races the host pressing play
            whileListening(4.5, [=] { set(eq2, QStringLiteral("parameter1"), 0.7); });
            whileListening(7.0, [=] { set(master, QStringLiteral("crossfader"), 0.9); });
            whileListening(5.5, [=] { report(QStringLiteral("guest listening")); });
        }
        at(2.5, [=] { report(QStringLiteral("guest live")); });
        // Guest, once live: swap deck 1 for a track the host may not have.
        if (tracks.size() > 1) {
            at(1.0, [=] { set(d1, QStringLiteral("play"), 0.0); });
            at(2.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(1), d1, false); });
            at(8.0, [=] { set(d1, QStringLiteral("play"), 1.0); });
        }
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> gap(0.6, 2.2);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (double t = 3.0; t < 600.0; t += gap(rng)) {
            const int which = static_cast<int>(unit(rng) * 4);
            const double v = unit(rng);
            switch (which) {
            case 0:
                at(t, [=] { set(master, QStringLiteral("crossfader"), v * 2.0 - 1.0); });
                break;
            case 1:
                at(t, [=] { set(d2, QStringLiteral("volume"), 0.3 + 0.7 * v); });
                break;
            case 2:
                at(t, [=] { set(eq2, QStringLiteral("parameter3"), v * 2.0); });
                break;
            default:
                at(t, [=] { set(d2, QStringLiteral("rate"), (v - 0.5) * 0.2); });
                break;
            }
        }
        std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) {
            return a.atSec < b.atSec;
        });
        std::stable_sort(m_listenSteps.begin(), m_listenSteps.end(), [](const Step& a, const Step& b) {
            return a.atSec < b.atSec;
        });
        qInfo() << "B2B autopilot (guest): " << m_steps.size() << "moves once live";
        return;
    }

    if (cueTest) {
        // Both decks loaded and left stopped; the host presses CUE on deck 1.
        at(1.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(0), d1, false); });
        at(1.5, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(1), d2, false); });
        at(5.5, [=] { cueReport(d1, QStringLiteral("host live, before")); });
        pressCue(d1, false, 6.0, 6, QStringLiteral("host live"));
        // Live, the way DJs play: quantize on, deck 1 playing, cueing deck 2.
        at(10.0, [=] {
            set(d1, QStringLiteral("quantize"), 1.0);
            set(d2, QStringLiteral("quantize"), 1.0);
            set(d1, QStringLiteral("play"), 1.0);
        });
        at(10.8, [=] { cueReport(d2, QStringLiteral("host live, quantize, deck 1 playing, before")); });
        pressCue(d2, false, 11.0, 8, QStringLiteral("host live, quantize, deck 1 playing"));
        // Then the host's deck 2 sits paused away from its cue point (as after
        // scrubbing), so the two copies differ for the guest's test.
        at(17.0, [=] {
            set(d1, QStringLiteral("play"), 0.0);
            set(d2, QStringLiteral("playposition"), 0.03);
        });
        at(27.0, [=] { cueReport(d2, QStringLiteral("host live, deck 2 after the guest's presses")); });
        std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) {
            return a.atSec < b.atSec;
        });
        m_elapsed.start();
        return;
    }

    // Setup
    at(0.0, [=] {
        set(master, QStringLiteral("crossfader"), -1.0);
        for (int d = 1; d <= numDecks; ++d) {
            const QString g = PlayerManager::groupForDeck(d - 1);
            set(g, QStringLiteral("quantize"), 0.0);
        }
    });
    if (!tracks.isEmpty()) {
        at(0.5, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(0), d1, false); });
    }
    if (tracks.size() > 1) {
        at(1.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(1), d2, false); });
    }

    // A short "set"
    at(4.0, [=] { set(d1, QStringLiteral("play"), 1.0); });
    at(8.0, [=] { set(d2, QStringLiteral("play"), 1.0); });
    for (int i = 0; i <= 150; ++i) { // crossfader sweep over 6 s
        const double x = -1.0 + 2.0 * i / 150.0;
        at(10.0 + i * 0.04, [=] { set(master, QStringLiteral("crossfader"), x); });
    }
    at(12.0, [=] { set(eq1, QStringLiteral("button_parameter1"), 1.0); }); // bass kill
    at(14.0, [=] { set(eq1, QStringLiteral("button_parameter1"), 0.0); });
    at(16.0, [=] { set(d1, QStringLiteral("volume"), 0.5); });
    at(17.0, [=] { set(d1, QStringLiteral("volume"), 1.0); });
    at(18.0, [=] { set(d2, QStringLiteral("playposition"), 0.5); }); // seek, like clicking the waveform
    at(22.0, [=] { set(d2, QStringLiteral("rate"), 0.3); });         // tempo up
    at(26.0, [=] { set(d1, QStringLiteral("play"), 0.0); });
    at(28.0, [=] { set(d1, QStringLiteral("play"), 1.0); });
    at(30.0, [=] { set(eq2, QStringLiteral("parameter2"), 0.3); });
    at(32.0, [=] { set(master, QStringLiteral("crossfader"), 0.0); });
    at(34.0, [=] { set(qfx1, QStringLiteral("super1"), 0.2); });
    at(36.0, [=] { set(qfx1, QStringLiteral("super1"), 0.5); });
    at(12.0, [=] { set(d2, QStringLiteral("sync_enabled"), 1.0); }); // engage sync on deck 2
    // Effects: a different effect in unit 1, assigned to deck 1, knobs moved;
    // the next quick effect on deck 2.
    at(19.0, [=] { set(QStringLiteral("[EffectRack1_EffectUnit1_Effect1]"), QStringLiteral("effect_selector"), 1.0); });
    at(19.2, [=] { set(QStringLiteral("[EffectRack1_EffectUnit1_Effect1]"), QStringLiteral("effect_selector"), 1.0); });
    at(19.5, [=] { set(QStringLiteral("[EffectRack1_EffectUnit1]"), QStringLiteral("group_[Channel1]_enable"), 1.0); });
    at(19.7, [=] { set(QStringLiteral("[EffectRack1_EffectUnit1]"), QStringLiteral("mix"), 0.7); });
    at(20.0, [=] { set(QStringLiteral("[EffectRack1_EffectUnit1_Effect1]"), QStringLiteral("meta"), 0.3); });
    at(20.5, [=] { set(QStringLiteral("[QuickEffectRack1_[Channel2]]"), QStringLiteral("next_chain_preset"), 1.0); });
    at(21.0, [=] { set(QStringLiteral("[QuickEffectRack1_[Channel2]]"), QStringLiteral("super1"), 0.8); });
    at(25.0, [=] { fxReport(QStringLiteral("host live")); });
    at(20.0, [=] { set(d1, QStringLiteral("beatloop_4_activate"), 1.0); }); // a 4-beat loop...
    at(21.0, [=] { loopReport(QStringLiteral("host live, loop on")); });
    at(25.0, [=] { set(d1, QStringLiteral("reloop_toggle"), 1.0); }); // ...and out of it
    at(25.5, [=] { loopReport(QStringLiteral("host live, loop off")); });
    at(24.0, [=] { report(QStringLiteral("host live")); });
    at(7.5, [=] { report(QStringLiteral("host live, after guest prep")); });
    at(29.0, [=] { report(QStringLiteral("host live")); });
    if (numDecks >= 3 && !tracks.isEmpty()) {
        at(30.0, [=, this] { m_pPlayerManager->slotLoadLocationToPlayer(tracks.value(0), d3, false); });
        at(33.0, [=] { set(d3, QStringLiteral("volume"), 0.6); });
        at(34.0, [=] { set(d3, QStringLiteral("play"), 1.0); });
        at(38.0, [=] { set(d3, QStringLiteral("playposition"), 0.4); });
        at(45.0, [=] { deck3Report(QStringLiteral("host live")); });
    }
    whileListening(38.0, [=] { report(QStringLiteral("host listening")); });

    // Then random moves until the end.
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> gap(0.6, 2.2);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (double t = 38.0; t < 600.0; t += gap(rng)) {
        const int which = static_cast<int>(unit(rng) * 6);
        const double v = unit(rng);
        switch (which) {
        case 0:
            at(t, [=] { set(master, QStringLiteral("crossfader"), v * 2.0 - 1.0); });
            break;
        case 1:
            at(t, [=] { set(d1, QStringLiteral("volume"), 0.3 + 0.7 * v); });
            break;
        case 2:
            at(t, [=] { set(d2, QStringLiteral("volume"), 0.3 + 0.7 * v); });
            break;
        case 3:
            at(t, [=] { set(eq1, QStringLiteral("parameter3"), v * 2.0); });
            break;
        case 4:
            at(t, [=] { set(eq2, QStringLiteral("parameter1"), v * 2.0); });
            break;
        default:
            at(t, [=] { set(d1, QStringLiteral("rate"), (v - 0.5) * 0.4); });
            break;
        }
    }
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const Step& a, const Step& b) {
        return a.atSec < b.atSec;
    });
    std::stable_sort(m_listenSteps.begin(), m_listenSteps.end(), [](const Step& a, const Step& b) {
        return a.atSec < b.atSec;
    });
    m_elapsed.start();
    qInfo() << "B2B autopilot: scripted set with" << m_steps.size() << "moves";
}

void Autopilot::at(double sec, std::function<void()> action) {
    m_steps.push_back({sec, std::move(action)});
}

void Autopilot::set(const QString& group, const QString& item, double value) {
    // Static set: no sender, so the control's own change handlers run, the
    // same as when a skin widget or controller moves it.
    ControlObject::set(ConfigKey(group, item), value);
}

void Autopilot::whileListening(double sec, std::function<void()> action) {
    m_listenSteps.push_back({sec, std::move(action)});
}

void Autopilot::tick(bool live) {
    const double sinceCreated = m_sinceCreated.elapsed() / 1000.0 - m_listenStartDelaySec;
    while (m_nextListen < m_listenSteps.size() && m_listenSteps[m_nextListen].atSec <= sinceCreated) {
        if (!live) {
            m_listenSteps[m_nextListen].action();
        }
        ++m_nextListen;
    }
    if (m_together) {
        const double now = m_elapsed.elapsed() / 1000.0 - m_startDelaySec;
        while (m_next < m_steps.size() && m_steps[m_next].atSec <= now) {
            m_steps[m_next].action();
            ++m_next;
        }
        return;
    }
    if (m_guest && !m_started) {
        if (!live) {
            return;
        }
        m_started = true;
        m_startDelaySec = 0;
        m_elapsed.restart();
    }
    const double now = m_elapsed.elapsed() / 1000.0 - m_startDelaySec;
    while (m_next < m_steps.size() && m_steps[m_next].atSec <= now) {
        // Moves made while not live would only be undone again.
        if (live) {
            m_steps[m_next].action();
        }
        ++m_next;
    }
}

} // namespace mixxx::b2b
