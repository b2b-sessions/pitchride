#include "b2b/b2bmixercontrols.h"

#include "b2b/b2bbeatfx.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QRegularExpression>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "effects/effectsmanager.h"

namespace mixxx::b2b {

namespace {
const QString kGroup = QStringLiteral("[B2BBooth]");

double get(const QString& group, const char* item) {
    return ControlObject::get(ConfigKey(group, QLatin1String(item)));
}

void set(const QString& group, const char* item, double value) {
    const ConfigKey key(group, QLatin1String(item));
    if (ControlObject::exists(key)) {
        ControlObject::set(key, value);
    }
}

void setParameter(const QString& group, const QString& item, double parameter) {
    if (ControlObject* pControl = ControlObject::getControl(
                ConfigKey(group, item), ControlFlag::NoAssertIfMissing)) {
        pControl->setParameter(parameter);
    }
}

QString unitGroup(int index) {
    return QStringLiteral("[EffectRack1_EffectUnit%1]").arg(index + 1);
}

QString unitEffect(int index) {
    return QStringLiteral("[EffectRack1_EffectUnit%1_Effect1]").arg(index + 1);
}

QString quickGroup(int deck) {
    return QStringLiteral("[QuickEffectRack1_[Channel%1]]").arg(deck);
}

// Inputs an effect unit can be put on.
const char* const kUnitInputs[] = {"[Channel1]",
        "[Channel2]",
        "[Channel3]",
        "[Channel4]",
        "[Microphone]",
        "[Microphone2]",
        "[BusLeft]",
        "[BusCenter]",
        "[BusRight]",
        "[Master]"};

void routeToMics(int unitIndex) {
    const QString group = unitGroup(unitIndex);
    for (const char* input : kUnitInputs) {
        const QString in = QLatin1String(input);
        const ConfigKey key(group, QStringLiteral("group_%1_enable").arg(in));
        if (ControlObject::exists(key)) {
            ControlObject::set(key, in.startsWith(QLatin1String("[Microphone")) ? 1.0 : 0.0);
        }
    }
}

// ---- layout 1
// The BEAT FX dial's positions (the "Club Mixer Beat FX" effect plays them
// all, b2bbeatfx.h; the names here only count them).
const char* const kBeatFx[] = {
        "org.mixxx.effects.echo",              // DELAY
        "org.mixxx.effects.echo",              // ECHO
        "org.mixxx.effects.echo",              // PING PONG
        "org.mixxx.effects.echo",              // SPIRAL
        "org.mixxx.effects.glitch",            // HELIX
        "org.mixxx.effects.reverb",            // REVERB
        "org.mixxx.effects.flanger",           // FLANGER
        "org.mixxx.effects.phaser",            // PHASER
        "org.mixxx.effects.filter",            // FILTER
        "org.mixxx.effects.moogladder4filter", // TRIPLET FILTER
        "org.mixxx.effects.tremolo",           // TRANS
        "org.mixxx.effects.glitch",            // ROLL
        "org.mixxx.effects.glitch",            // TRIPLET ROLL
        "org.mixxx.effects.pitchshift",        // MOBIUS
};
constexpr int kBeatFxPositions = sizeof(kBeatFx) / sizeof(kBeatFx[0]);
constexpr int kBeatFxDefault = 6; // FLANGER
constexpr int kBeatFxChannelDefault = 8; // master
const char* const kBeatFxInputs[] = {"[Channel1]",
        "[Channel2]",
        "[Channel3]",
        "[Channel4]",
        "[Microphone]",
        "[BusLeft]",
        "[BusRight]",
        "[Master]"};

const char* const kColorFx[] = {
        nullptr,                        // off
        "org.mixxx.effects.reverb",     // SPACE
        "org.mixxx.effects.echo",       // DUB ECHO
        "org.mixxx.effects.bitcrusher", // CRUSH
        "org.mixxx.effects.phaser",     // SWEEP
        "org.mixxx.effects.whitenoise", // NOISE
        "org.mixxx.effects.filter",     // FILTER
};
constexpr int kColorFxFilter = 6;
// The COLOR knobs: gentle near the centre, the full effect only at the ends
// (a straight line was too much too soon).
constexpr double kColorCurve = 2.0;
constexpr double kColorWetMax = 0.8; // the effects other than FILTER, fully up
// CENTER LOCK: inside this of the centre it catches; it takes this much
// more turning to leave.
constexpr double kLockZone = 0.05;
constexpr double kLockEscape = 0.15;
constexpr qint64 kLockSwallowMs = 300; // the rest of a quick turn stops there

double colorCurve(double v) {
    return std::copysign(std::pow(std::fabs(v), kColorCurve), v);
}

// The COLOR knob position that colorCurve() turns into `shaped`.
double colorKnobFor(double shaped) {
    return std::copysign(std::pow(std::fabs(shaped), 1.0 / kColorCurve), shaped);
}

const char* const kMicFx[] = {
        nullptr,                        // off
        "org.mixxx.effects.echo",       // ECHO
        "org.mixxx.effects.pitchshift", // PITCH
        "org.mixxx.effects.distortion", // MEGAPHONE
};
constexpr int kMicFxUnit = 2;     // effect unit 3
constexpr int kMicReverbUnit = 3; // effect unit 4

// ---- layouts 2 and 3
constexpr int kHpf = 1;
constexpr int kBpf = 2;
constexpr int kLpf = 3;
// Mixxx's filter: knob parameters lpf, q, hpf.
const QString kLpfParam = QStringLiteral("parameter1");
const QString kHpfParam = QStringLiteral("parameter3");
// The LFO's period, by the rate knob, in beats.
const double kLfoBeats[] = {16, 8, 4, 2, 1, 0.5, 0.25};
constexpr double kLfoDepth = 0.3;
} // namespace

MixerControls::MixerControls(int decks, QObject* pParent)
        : QObject(pParent),
          m_numDecks(decks) {
    m_layoutControl = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("layout")));
    connect(m_layoutControl.get(), &ControlObject::valueChanged, this, [this](double v) {
        layoutChanged(static_cast<int>(v));
    });

    // Layout 1
    m_beatFxSelect = std::make_unique<ControlPotmeter>(ConfigKey(kGroup, QStringLiteral("beatfx_select")),
            0.0,
            kBeatFxPositions - 1,
            false,
            true,
            false,
            true,
            kBeatFxDefault);
    m_beatFxLoaded = static_cast<int>(std::lround(m_beatFxSelect->get()));
    m_beatFx = std::make_unique<BeatFx>(decks, this);
    m_beatFx->setOnRemoteFx([this](int fx) {
        // The other DJ picked it: the knob shows it.
        m_beatFxLoaded = fx;
        m_beatFxSelect->set(fx);
    });
    connect(m_beatFxSelect.get(), &ControlObject::valueChanged, this, &MixerControls::beatFxTurned);
    m_beatFxSettle.setSingleShot(true);
    m_beatFxSettle.setInterval(350);
    connect(&m_beatFxSettle, &QTimer::timeout, this, [this] {
        // Let go between two positions: settle on the nearest, like a detent.
        m_beatFxSelect->set(std::round(m_beatFxSelect->get()));
    });
    m_beatFxChannel = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("beatfx_ch")), true, false, true, kBeatFxChannelDefault);
    m_beatFxChannel->connectValueChangeRequest(this, [this](double v) {
        if (v < 1) {
            return; // one input is always selected: pressing the lit one keeps it
        }
        m_beatFxChannel->setAndConfirm(v);
        applyBeatFxChannel(static_cast<int>(v));
    });
    m_colorFx = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("color_fx")), true, false, true, kColorFxFilter);
    m_colorFx->connectValueChangeRequest(this, &MixerControls::colorFxRequested);
    for (int c = 0; c < decks; ++c) {
        auto pKnob = std::make_unique<ControlPotmeter>(
                ConfigKey(kGroup, QStringLiteral("color%1").arg(c + 1)), -1.0, 1.0);
        connect(pKnob.get(), &ControlObject::valueChanged, this, [this, c](double v) {
            const double locked = lockColor(c, v, true);
            if (locked != v) {
                m_color[c]->set(locked); // the knob is drawn where it caught
            }
            applyColor(c);
        });
        m_color.push_back(std::move(pKnob));
    }
    m_lock.resize(decks);
    m_centerLock = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("center_lock")), true);
    m_centerLock->setButtonMode(ControlPushButton::TOGGLE);
    m_hwHeadMix = std::make_unique<ControlPotmeter>(ConfigKey(kGroup, QStringLiteral("hw_head_mix")));
    connect(m_hwHeadMix.get(), &ControlObject::valueChanged, this, &MixerControls::hwHeadMixTurned);
    // A potmeter (-1 .. 1), so the knobs drawing it stay within their travel.
    m_headMixShown = std::make_unique<ControlPotmeter>(
            ConfigKey(kGroup, QStringLiteral("head_mix_shown")), -1.0, 1.0);
    m_colorParam = std::make_unique<ControlPotmeter>(ConfigKey(kGroup, QStringLiteral("color_param")));
    connect(m_colorParam.get(), &ControlObject::valueChanged, this, [this](double) {
        applyColorParam();
    });
    m_micFx = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("mic_fx")));
    m_micFx->connectValueChangeRequest(this, &MixerControls::micFxRequested);
    m_masterCue = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("master_cue")));
    m_masterCue->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_masterCue.get(), &ControlObject::valueChanged, this, [this](double v) {
        applyMasterCue(v > 0);
    });

    // Layouts 2 and 3
    for (int k = 0; k < 2; ++k) {
        Filter& f = m_filters[k];
        auto key = [k](const char* what) {
            return ConfigKey(kGroup, QStringLiteral("filter%1_%2").arg(k + 1).arg(QLatin1String(what)));
        };
        f.mode = std::make_unique<ControlObject>(key("mode"), true, false, true, kLpf);
        f.mode->connectValueChangeRequest(this, [this, k](double v) {
            filterModeRequested(k, v);
        });
        f.lfoOn = std::make_unique<ControlPushButton>(key("lfo_on"));
        f.lfoOn->setButtonMode(ControlPushButton::TOGGLE);
        f.lfoRate = std::make_unique<ControlPotmeter>(key("lfo"), 0.0, 1.0, false, true, false, true, 4.0 / 6.0);
        f.depth = std::make_unique<ControlPotmeter>(key("depth"), 0.0, 1.0, false, true, false, true, 0.5);
        f.xfader = std::make_unique<ControlPushButton>(key("xfader"));
        f.xfader->setButtonMode(ControlPushButton::TOGGLE);
        connect(f.xfader.get(), &ControlObject::valueChanged, this, [this, k](double) {
            applyFilterMeta(k);
            if (k == 0) {
                applyFilterMeta(1);
            }
        });
        f.x2 = std::make_unique<ControlPushButton>(key("x2"));
        f.x2->setButtonMode(ControlPushButton::TOGGLE);
        f.tap = std::make_unique<ControlPushButton>(key("tap"));
        f.beat = std::make_unique<ControlObject>(key("beat"));
        connect(f.lfoOn.get(), &ControlObject::valueChanged, this, [this, k](double v) {
            if (v > 0) {
                m_filters[k].phase = 0;
                if (!m_lfoTimer.isActive()) {
                    m_lfoClock.start();
                    m_lfoTimer.start();
                }
            } else {
                applyFilterMeta(k); // back to where FREQ is
                m_filters[k].beat->set(0.0);
            }
        });
        connect(f.tap.get(), &ControlObject::valueChanged, this, [this, k](double v) {
            if (v > 0) {
                filterTap(k);
            }
        });
    }
    m_vcfLink = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("vcf_link")));
    m_vcfLink->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_vcfLink.get(), &ControlObject::valueChanged, this, [this](double) {
        applyFilterMeta(1);
    });
    const QStringList auxInputs = {QStringLiteral("Channel1"),
            QStringLiteral("Channel2"),
            QStringLiteral("Channel3"),
            QStringLiteral("Channel4"),
            QStringLiteral("Microphone"),
            QStringLiteral("Microphone2")};
    for (int a = 1; a <= 2; ++a) {
        for (const QString& input : auxInputs) {
            const int index = static_cast<int>(m_auxSend.size());
            m_auxSend.push_back(std::make_unique<ControlPotmeter>(
                    ConfigKey(kGroup, QStringLiteral("aux%1_%2").arg(a).arg(input))));
            m_auxSendTo.emplace_back(a + 1, QStringLiteral("[%1]").arg(input)); // units 3 / 4
            connect(m_auxSend.back().get(), &ControlObject::valueChanged, this, [this, index](double) {
                applyAuxSend(index);
            });
        }
    }
    for (int c = 0; c < decks; ++c) {
        auto pAssign = std::make_unique<ControlPushButton>(
                ConfigKey(kGroup, QStringLiteral("filter_assign%1").arg(c + 1)));
        pAssign->setButtonMode(ControlPushButton::TOGGLE);
        pAssign->setStates(3);
        connect(pAssign.get(), &ControlObject::valueChanged, this, [this, c](double) {
            applyFilterAssign(c);
        });
        m_filterAssign.push_back(std::move(pAssign));
    }
    m_boothMute = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("booth_mute")));
    m_boothMute->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_boothMute.get(), &ControlObject::valueChanged, this, [this](double v) {
        const QString master = QStringLiteral("[Master]");
        if (v > 0) {
            m_boothGainBefore = get(master, "booth_gain");
            set(master, "booth_gain", 0.0);
        } else {
            set(master, "booth_gain", m_boothGainBefore);
        }
    });
    m_lfoTimer.setInterval(15);
    connect(&m_lfoTimer, &QTimer::timeout, this, &MixerControls::lfoTick);

    if (qApp) {
        qApp->installEventFilter(this); // the beat FX dial's wheel
    }
}

void MixerControls::setWindow(QWidget* pWindow) {
    m_beatFx->setWindow(pWindow);
}

MixerControls::~MixerControls() {
    if (qApp) {
        qApp->removeEventFilter(this);
    }
}

void MixerControls::setEffectsManager(std::shared_ptr<EffectsManager> pEffectsManager) {
    m_pEffectsManager = std::move(pEffectsManager);
    if (m_layout != 0) {
        QTimer::singleShot(0, this, &MixerControls::setUpLayout);
    }
}

bool MixerControls::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pEvent->type() == QEvent::Wheel && pObject->objectName() == QLatin1String("BeatFxDial")) {
        // One notch of the wheel = one effect along the dial.
        const int dy = static_cast<QWheelEvent*>(pEvent)->angleDelta().y();
        if (dy != 0) {
            const double position = std::round(m_beatFxSelect->get()) + (dy > 0 ? 1 : -1);
            const double clamped = std::clamp(position, 0.0, static_cast<double>(kBeatFxPositions - 1));
            m_beatFxSelect->set(clamped);
            beatFxTurned(clamped); // set() here doesn't signal back to us
        }
        return true;
    }
    return false;
}

void MixerControls::layoutChanged(int layout) {
    if (layout == m_layout) {
        return;
    }
    m_layout = layout;
    // The engine's effect units may not be there yet when the skin loads.
    QTimer::singleShot(0, this, &MixerControls::setUpLayout);
}

void MixerControls::setUpLayout() {
    if (!m_pEffectsManager) {
        return;
    }
    qInfo() << "B2B: mixer layout" << m_layout;
    for (Filter& f : m_filters) {
        f.freq.reset();
    }
    m_lfoTimer.stop();
    m_beatFx->setActive(false);
    switch (m_layout) {
    case 1: {
        applyBeatFxChannel(static_cast<int>(m_beatFxChannel->get()));
        // BEAT FX: one effect plays all 14 (ON/OFF is its parameter, so
        // the unit stays on and echoes / reverb ring out after OFF).
        loadEffect(unitGroup(0), 0, "org.mixxx.effects.b2bbeatfx");
        set(unitGroup(0), "mix_mode", 0.0);
        set(unitGroup(0), "mix", 1.0);
        set(unitGroup(0), "enabled", 1.0);
        m_beatFx->setActive(true);
        m_beatFxLoaded = -1;
        loadBeatFx(static_cast<int>(std::lround(m_beatFxSelect->get())));
        applyColorFx();
        // A controller's filter knob (the quick effect's super1) turns the
        // COLOR knob, so the skin and the hardware agree.
        m_quickSuper.clear();
        m_quickSuperSet.assign(m_color.size(), -1);
        for (int c = 0; c < static_cast<int>(m_color.size()); ++c) {
            auto pProxy = std::make_unique<ControlProxy>(quickGroup(c + 1), QStringLiteral("super1"), this);
            pProxy->connectValueChanged(this, [this, c](double v) {
                if (m_layout != 1 || std::fabs(v - m_quickSuperSet[c]) < 1e-6) {
                    return; // ours
                }
                // A hardware knob is where it is: the COLOR knob goes where
                // that filter setting is (through the curve backwards), and
                // CENTER LOCK leaves it alone. (Shaped again and locked, a
                // slow turn kept snapping back to the middle.)
                m_quickSuperSet[c] = v;
                const double knob = std::clamp(colorKnobFor(v * 2.0 - 1.0), -1.0, 1.0);
                if (c < static_cast<int>(m_lock.size())) {
                    m_lock[c].held = false;
                    m_lock[c].last = knob;
                }
                m_color[c]->set(knob);
                const int fx = static_cast<int>(m_colorFx->get());
                if (fx == 0) {
                    // No COLOR FX picked (a DDJ has no buttons for them):
                    // the knob is the filter, as on the controller.
                    set(quickGroup(c + 1), "mix", 1.0);
                } else if (fx != kColorFxFilter) {
                    applyColor(c); // SPACE, DUB ECHO...: the knob drives it
                }
            });
            m_quickSuper.push_back(std::move(pProxy));
        }
        applyMicFx();
        loadEffect(unitGroup(kMicReverbUnit), kMicReverbUnit, "org.mixxx.effects.reverb");
        routeToMics(kMicReverbUnit);
        set(unitGroup(kMicReverbUnit), "enabled", 0.0); // MIC REVERB starts off
        break;
    }
    case 2:
    case 3: {
        const int filters = m_layout == 2 ? 1 : 2;
        for (int k = 0; k < filters; ++k) {
            m_filters[k].freq = std::make_unique<ControlProxy>(unitGroup(k), QStringLiteral("super1"), this);
            m_filters[k].freq->connectValueChanged(this, [this, k](double) {
                applyFilterMeta(k);
                if (k == 0) {
                    applyFilterMeta(1); // VCF LINK
                }
            });
            applyFilterMode(k);
        }
        m_crossfader = std::make_unique<ControlProxy>(
                QStringLiteral("[Master]"), QStringLiteral("crossfader"), this);
        m_crossfader->connectValueChanged(this, [this](double) {
            // VCF TO XFADER
            if (m_filters[0].xfader->get() > 0 || m_filters[1].xfader->get() > 0) {
                applyFilterMeta(0);
                applyFilterMeta(1);
            }
        });
        if (m_layout == 2) {
            // EXT ON: an echo on the mix, as the external effects loop.
            loadEffect(unitGroup(1), 1, "org.mixxx.effects.echo");
            set(unitGroup(1), "mix_mode", 1.0);
        }
        if (m_layout == 3) {
            // AUX 1 is an echo (effect unit 3), AUX 2 a reverb (unit 4):
            // sends, so dry + wet; each input's AUX knobs send it.
            loadEffect(unitGroup(2), 2, "org.mixxx.effects.echo");
            set(unitGroup(2), "mix_mode", 1.0);
            set(unitGroup(2), "enabled", 1.0);
            loadEffect(unitGroup(3), 3, "org.mixxx.effects.reverb");
            set(unitGroup(3), "mix_mode", 1.0);
            set(unitGroup(3), "enabled", 1.0);
            for (int i = 0; i < static_cast<int>(m_auxSend.size()); ++i) {
                applyAuxSend(i);
            }
            for (int c = 0; c < m_numDecks; ++c) {
                applyFilterAssign(c);
            }
        }
        break;
    }
    default:
        break;
    }
}

void MixerControls::loadEffect(const QString& chainGroup, int chainIndex, const char* effectId, bool clearOthers) {
    if (!m_pEffectsManager) {
        return;
    }
    EffectChainPointer pChain = chainIndex >= 0
            ? m_pEffectsManager->getStandardEffectChain(chainIndex)
            : m_pEffectsManager->getEffectChain(chainGroup);
    if (!pChain) {
        return;
    }
    const QString slotPrefix = chainGroup.left(chainGroup.size() - 1);
    if (clearOthers) {
        for (int s = 2; s <= 3; ++s) {
            set(QStringLiteral("%1_Effect%2]").arg(slotPrefix).arg(s), "clear", 1.0);
        }
    }
    EffectSlotPointer pSlot = pChain->getEffectSlot(0);
    if (!pSlot || !effectId) {
        return;
    }
    EffectManifestPointer pManifest = m_pEffectsManager->getBackendManager()->getManifest(
            QString::fromLatin1(effectId), EffectBackendType::BuiltIn);
    if (!pManifest) {
        return;
    }
    if (!pSlot->getManifest() || pSlot->getManifest()->id() != pManifest->id()) {
        pSlot->loadEffectWithDefaults(pManifest);
        qInfo() << "B2B: loaded" << pManifest->id() << "into" << chainGroup;
    }
    set(QStringLiteral("%1_Effect1]").arg(slotPrefix), "enabled", 1.0);
}

// ----------------------------------------------------------------- layout 1 --

void MixerControls::resetMixer() {
    if (m_layout != 1) {
        return;
    }
    m_colorFx->setAndConfirm(0.0);
    for (auto& pColor : m_color) {
        pColor->setAndConfirm(pColor->defaultValue());
    }
    m_colorParam->setAndConfirm(m_colorParam->defaultValue());
    applyColorFx();
    applyColorParam();
    m_micFx->setAndConfirm(0.0);
    applyMicFx();
    // BEAT FX: off and its settings back first, then its dial and channel.
    m_beatFx->reset();
    m_beatFxSelect->setAndConfirm(kBeatFxDefault);
    m_beatFxLoaded = -1; // (load it again even if the dial was there)
    loadBeatFx(std::clamp(static_cast<int>(std::lround(m_beatFxSelect->get())), 0, kBeatFxPositions - 1));
    m_beatFxChannel->setAndConfirm(m_beatFxChannel->defaultValue());
    applyBeatFxChannel(static_cast<int>(m_beatFxChannel->get()));
}

void MixerControls::beatFxTurned(double value) {
    const int position = std::clamp(static_cast<int>(std::lround(value)), 0, kBeatFxPositions - 1);
    if (position != m_beatFxLoaded && m_layout == 1) {
        loadBeatFx(position);
    }
    if (value != std::round(value)) {
        m_beatFxSettle.start();
    }
}

void MixerControls::loadBeatFx(int position) {
    m_beatFxLoaded = position;
    m_beatFx->pickedLocally(position);
}

void MixerControls::applyBeatFxChannel(int channel) {
    if (m_layout != 1) {
        return;
    }
    for (int i = 0; i < static_cast<int>(std::size(kBeatFxInputs)); ++i) {
        const ConfigKey input(unitGroup(0),
                QStringLiteral("group_%1_enable").arg(QLatin1String(kBeatFxInputs[i])));
        if (ControlObject::exists(input)) {
            ControlObject::set(input, i + 1 == channel ? 1.0 : 0.0);
        }
    }
}

void MixerControls::colorFxRequested(double value) {
    int fx = std::clamp(static_cast<int>(std::lround(value)), 0, kColorFxFilter);
    if (fx == static_cast<int>(m_colorFx->get())) {
        fx = 0; // the lit button pressed again: off
    }
    m_colorFx->setAndConfirm(fx);
    applyColorFx();
}

void MixerControls::applyColorFx() {
    if (m_layout != 1) {
        return;
    }
    const int fx = std::clamp(static_cast<int>(m_colorFx->get()), 0, kColorFxFilter);
    for (int c = 0; c < static_cast<int>(m_color.size()); ++c) {
        const QString group = quickGroup(c + 1);
        if (!ControlObject::exists(ConfigKey(group, QStringLiteral("mix")))) {
            continue;
        }
        loadEffect(group, -1, kColorFx[fx] ? kColorFx[fx] : kColorFx[kColorFxFilter]);
        set(group, "mix_mode", 0.0); // dry / wet
        set(group, "enabled", 1.0);
        applyColor(c);
    }
    applyColorParam();
}

void MixerControls::applyColor(int c) {
    if (m_layout != 1) {
        return;
    }
    const QString group = quickGroup(c + 1);
    const int fx = static_cast<int>(m_colorFx->get());
    const double v = m_color[c]->get();
    const double shaped = colorCurve(v);
    if (fx == kColorFxFilter) {
        // Low-pass to the left, high-pass to the right, flat in the middle.
        setQuickSuper(c, (shaped + 1.0) / 2.0);
        set(group, "mix", 1.0);
    } else if (fx == 0) {
        setQuickSuper(c, 0.5);
        set(group, "mix", 0.0);
    } else {
        // Centre = off, either way = more.
        set(group, "mix", kColorWetMax * std::fabs(shaped));
    }
}

void MixerControls::setQuickSuper(int c, double value) {
    if (c < static_cast<int>(m_quickSuperSet.size())) {
        m_quickSuperSet[c] = value;
    }
    set(quickGroup(c + 1), "super1", value);
}

void MixerControls::applyColorParam() {
    if (m_layout != 1) {
        return;
    }
    const int fx = static_cast<int>(m_colorFx->get());
    const double p = m_colorParam->get();
    for (int c = 0; c < static_cast<int>(m_color.size()); ++c) {
        const QString effect = QStringLiteral("[QuickEffectRack1_[Channel%1]_Effect1]").arg(c + 1);
        if (fx == kColorFxFilter) {
            setParameter(effect, QStringLiteral("parameter2"), p); // resonance
        } else if (fx != 0) {
            setQuickSuper(c, 0.5);
            set(effect, "meta", p);
        }
    }
}

void MixerControls::micFxRequested(double value) {
    int fx = std::clamp(static_cast<int>(std::lround(value)), 0, 3);
    if (fx == static_cast<int>(m_micFx->get())) {
        fx = 0;
    }
    m_micFx->setAndConfirm(fx);
    applyMicFx();
}

void MixerControls::applyMicFx() {
    if (m_layout != 1) {
        return;
    }
    const int fx = std::clamp(static_cast<int>(m_micFx->get()), 0, 3);
    const QString group = unitGroup(kMicFxUnit);
    if (fx > 0) {
        loadEffect(group, kMicFxUnit, kMicFx[fx]);
    }
    routeToMics(kMicFxUnit);
    set(group, "enabled", fx > 0 ? 1.0 : 0.0);
}

double MixerControls::lockColor(int c, double v, bool relative) {
    if (c >= static_cast<int>(m_lock.size())) {
        return v;
    }
    Lock& lock = m_lock[c];
    const double before = lock.last;
    if (m_centerLock->get() <= 0) {
        lock.held = false;
        lock.last = v;
        return v;
    }
    if (lock.held) {
        // Caught at the centre (the skin's knob was put back there, so its
        // moves are the turning since).
        if (lock.since.elapsed() < kLockSwallowMs) {
            lock.push = 0; // still the quick turn that got it here
            lock.last = 0;
            return 0;
        }
        lock.push = relative ? lock.push + v : v;
        if (std::fabs(lock.push) < kLockEscape) {
            lock.last = 0;
            return 0;
        }
        lock.held = false;
        lock.last = std::clamp(lock.push, -1.0, 1.0);
        return lock.last;
    }
    // Near the centre, or past it in one quick move: it catches.
    if (std::fabs(v) < kLockZone || before * v < 0) {
        lock.held = true;
        lock.push = 0;
        lock.last = 0;
        lock.since.start();
        return 0;
    }
    lock.last = v;
    return v;
}

bool MixerControls::headphonesMixedByController() {
    // Which sound card the headphones are on (Preferences > Sound Hardware),
    // looked at now and then.
    if (m_headphonesChecked.isValid() && m_headphonesChecked.elapsed() < 10000) {
        return m_headphonesOnController;
    }
    m_headphonesChecked.start();
    m_headphonesOnController = false;
    QFile file(QDir(m_settingsPath).filePath(QStringLiteral("soundconfig.xml")));
    QDomDocument doc;
    if (m_settingsPath.isEmpty() || !file.open(QIODevice::ReadOnly) || !doc.setContent(&file)) {
        return false;
    }
    const QDomNodeList devices = doc.elementsByTagName(QStringLiteral("SoundDevice"));
    for (int i = 0; i < devices.size(); ++i) {
        const QDomElement device = devices.at(i).toElement();
        const QDomNodeList outputs = device.elementsByTagName(QStringLiteral("output"));
        for (int o = 0; o < outputs.size(); ++o) {
            if (outputs.at(o).toElement().attribute(QStringLiteral("type")) == QLatin1String("Headphones") &&
                    device.attribute(QStringLiteral("name"))
                            .contains(QRegularExpression(QStringLiteral("DDJ.?400"),
                                    QRegularExpression::CaseInsensitiveOption))) {
                m_headphonesOnController = true;
            }
        }
    }
    return m_headphonesOnController;
}

void MixerControls::hwHeadMixTurned(double value) {
    const QString master = QStringLiteral("[Master]");
    const double mix = std::clamp(value * 2.0 - 1.0, -1.0, 1.0);
    watchHeadMix();
    if (headphonesMixedByController()) {
        // The controller blends cue and master itself: Mixxx sends it the
        // cue alone, and the skin shows the controller's knob.
        m_settingHeadMix = true;
        set(master, "headMix", -1.0);
        m_settingHeadMix = false;
    } else {
        m_settingHeadMix = true;
        set(master, "headMix", mix);
        m_settingHeadMix = false;
    }
    m_headMixShown->set(mix);
}

void MixerControls::watchHeadMix() {
    if (m_headMix) {
        return;
    }
    // The skins' MIX knobs are drawn where the mix is (or where the
    // controller's knob is, when it does the blending).
    m_headMix = std::make_unique<ControlProxy>(QStringLiteral("[Master]"), QStringLiteral("headMix"), this);
    m_headMixShown->set(m_headMix->get());
    m_headMix->connectValueChanged(this, [this](double v) {
        if (!m_settingHeadMix) {
            m_headMixShown->set(v); // turned in the skin
        }
    });
}

void MixerControls::applyMasterCue(bool on) {
    // Like a club mixer's Master CUE: the master joins the cue in the
    // headphones (the engine adds it); the MIX knob stays where it is.
    set(QStringLiteral("[Master]"), "b2b_main_cue", on ? 1.0 : 0.0);
}

// ------------------------------------------------------------ layouts 2 / 3 --

void MixerControls::filterModeRequested(int k, double value) {
    int mode = std::clamp(static_cast<int>(std::lround(value)), 0, kLpf);
    const int current = static_cast<int>(m_filters[k].mode->get());
    if (m_layout == 2 && mode == current) {
        mode = 0; // the 2-channel mixer: the lit HPF / LPF again turns it off
    }
    if (m_layout != 2 && mode == 0) {
        return; // the club mixer always has a mode; FILTER ON switches it
    }
    m_filters[k].mode->setAndConfirm(mode);
    applyFilterMode(k);
}

void MixerControls::applyFilterMode(int k) {
    if (m_layout != 2 && m_layout != 3) {
        return;
    }
    int mode = static_cast<int>(m_filters[k].mode->get());
    const QString unit = unitGroup(k);
    const QString effect = unitEffect(k);
    if (m_layout == 2) {
        set(unit, "enabled", mode > 0 ? 1.0 : 0.0);
        if (mode == 0) {
            return;
        }
    } else if (mode == 0) {
        mode = kLpf;
    }
    loadEffect(unit, k, "org.mixxx.effects.filter");
    set(unit, "mix_mode", 0.0);
    set(unit, "mix", 1.0);
    // FREQ (the unit's super knob) moves the cutoff of the chosen side; the
    // other side stays out of the way.
    const bool hpf = mode == kHpf || mode == kBpf;
    const bool lpf = mode == kLpf || mode == kBpf;
    set(effect, "parameter1_link_type", lpf ? 1.0 : 0.0);
    set(effect, "parameter3_link_type", hpf ? 1.0 : 0.0);
    set(effect, "parameter1_link_inverse", 0.0);
    set(effect, "parameter3_link_inverse", 0.0);
    if (!lpf) {
        setParameter(effect, kLpfParam, 1.0); // fully open
    }
    if (!hpf) {
        setParameter(effect, kHpfParam, 0.0); // fully open
    }
    applyFilterMeta(k);
}

void MixerControls::filterTap(int k) {
    Filter& f = m_filters[k];
    if (f.tapClock.isValid() && f.tapClock.elapsed() < 2000 && f.taps > 0) {
        const double bpm = 60000.0 / std::max<qint64>(150, f.tapClock.elapsed());
        f.tapBpm = f.taps > 1 ? 0.6 * f.tapBpm + 0.4 * bpm : bpm;
        ++f.taps;
    } else {
        f.taps = 1;
    }
    f.tapClock.start();
    f.phase = 0; // the tap is the beat
}

void MixerControls::lfoTick() {
    const double seconds = m_lfoClock.restart() / 1000.0;
    bool any = false;
    if (!m_clockBpm) {
        m_clockBpm = std::make_unique<ControlProxy>(
                QStringLiteral("[InternalClock]"), QStringLiteral("bpm"), this);
    }
    for (int k = 0; k < 2; ++k) {
        Filter& f = m_filters[k];
        if (f.lfoOn->get() <= 0 || !f.freq) {
            continue;
        }
        any = true;
        double bpm = f.tapBpm > 0 ? f.tapBpm : m_clockBpm->get();
        if (bpm <= 0) {
            bpm = 120;
        }
        const int rate = std::clamp(static_cast<int>(std::lround(f.lfoRate->get() * 6)), 0, 6);
        double beats = kLfoBeats[rate];
        if (f.x2->get() > 0) {
            beats /= 2;
        }
        f.phase = std::fmod(f.phase + seconds * bpm / 60.0 / beats, 1.0);
        const double wave = (1.0 - std::cos(2 * M_PI * f.phase)) / 2.0; // 0 .. 1 .. 0
        int mode = static_cast<int>(f.mode->get());
        if (mode == 0) {
            mode = kLpf;
        }
        const double depth = 0.6 * f.depth->get();
        double offset = 0;
        if (mode == kHpf) {
            offset = depth * wave; // sweeps up from FREQ
        } else if (mode == kLpf) {
            offset = -depth * wave; // sweeps down from FREQ
        } else {
            offset = depth * 0.5 * std::sin(2 * M_PI * f.phase);
        }
        applyFilterMeta(k, offset);
        f.beat->set(f.phase < 0.15 ? 1.0 : 0.0);
    }
    if (!any) {
        m_lfoTimer.stop();
    }
}

double MixerControls::filterBase(int k) const {
    if (k == 1 && m_vcfLink->get() > 0) {
        return filterBase(0);
    }
    if (m_filters[k].xfader->get() > 0 && m_crossfader) {
        return (m_crossfader->get() + 1.0) / 2.0;
    }
    if (m_filters[k].freq) {
        return m_filters[k].freq->get();
    }
    return get(unitGroup(k), "super1");
}

void MixerControls::applyFilterMeta(int k, double offset) {
    if (!m_filters[k].freq) {
        return; // this layout has no such filter
    }
    set(unitEffect(k), "meta", std::clamp(filterBase(k) + offset, 0.0, 1.0));
}

void MixerControls::applyAuxSend(int index) {
    if (m_layout != 3) {
        return;
    }
    const auto& [unit, input] = m_auxSendTo[index];
    const ConfigKey key(unitGroup(unit), QStringLiteral("group_%1_enable").arg(input));
    if (ControlObject::exists(key)) {
        // Mixxx sends are on or off: up at all = sent.
        ControlObject::set(key, m_auxSend[index]->get() > 0.01 ? 1.0 : 0.0);
    }
}

void MixerControls::applyFilterAssign(int c) {
    if (m_layout != 3) {
        return;
    }
    const int to = static_cast<int>(m_filterAssign[c]->get()); // 0 none, 1, 2
    const QString input = QStringLiteral("group_[Channel%1]_enable").arg(c + 1);
    for (int k = 0; k < 2; ++k) {
        const ConfigKey key(unitGroup(k), input);
        if (ControlObject::exists(key)) {
            ControlObject::set(key, to == k + 1 ? 1.0 : 0.0);
        }
    }
}

} // namespace mixxx::b2b

#include "moc_b2bmixercontrols.cpp"
