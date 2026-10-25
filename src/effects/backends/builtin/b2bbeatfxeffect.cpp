#include "effects/backends/builtin/b2bbeatfxeffect.h"

#include <cmath>

#include "control/controlobject.h"
#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
const QString kBooth = QStringLiteral("[B2BBooth]");

void addKnob(EffectManifest* pManifest,
        const char* id,
        const QString& name,
        double minimum,
        double defaultValue,
        double maximum) {
    EffectManifestParameterPointer p = pManifest->addParameter();
    p->setId(QString::fromLatin1(id));
    p->setName(name);
    p->setShortName(name);
    p->setDescription(name);
    p->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    p->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    p->setRange(minimum, defaultValue, maximum);
}
} // namespace

// static
QString B2BBeatFxEffect::getId() {
    return "org.mixxx.effects.b2bbeatfx";
}

// static
EffectManifestPointer B2BBeatFxEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Club Mixer Beat FX"));
    pManifest->setShortName(QObject::tr("Beat FX"));
    pManifest->setAuthor("B2B");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "The club mixer's BEAT FX: 14 effects timed to the beat, played "
            "from the mixer's BEAT FX section."));
    EffectManifest* p = pManifest.data();
    addKnob(p, "fx", QObject::tr("Effect"), 0, b2bfx::kFlanger, b2bfx::kFxCount - 1);
    addKnob(p, "beats", QObject::tr("Beat"), 1.0 / 32, 1, 100);
    addKnob(p, "time", QObject::tr("Time"), 0, 0, 4000);
    addKnob(p, "level", QObject::tr("Level/Depth"), 0, 0.7, 1);
    addKnob(p, "on", QObject::tr("On"), 0, 0, 1);
    addKnob(p, "low", QObject::tr("Low"), 0, 1, 1);
    addKnob(p, "mid", QObject::tr("Mid"), 0, 1, 1);
    addKnob(p, "hi", QObject::tr("Hi"), 0, 1, 1);
    addKnob(p, "wave", QObject::tr("Wave"), 0, 0, 1);
    addKnob(p, "quantize", QObject::tr("Quantize"), 0, 1, 1);
    addKnob(p, "tap_bpm", QObject::tr("Tap BPM"), 0, 0, 300);
    addKnob(p, "xpad", QObject::tr("X-Pad"), 0, 0, 1);
    return pManifest;
}

// static
void B2BBeatFxEffect::ensureTimingControls() {
    // Made once, kept for the app's life (the engine reads them).
    for (const char* item : {"beatfx_bpm", "beatfx_phase"}) {
        const ConfigKey key(kBooth, QString::fromLatin1(item));
        if (!ControlObject::exists(key)) {
            new ControlObject(key);
        }
    }
}

B2BBeatFxEffect::B2BBeatFxEffect()
        : m_bpm((ensureTimingControls(), kBooth), QStringLiteral("beatfx_bpm")),
          m_phase(kBooth, QStringLiteral("beatfx_phase")) {
}

void B2BBeatFxEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pFx = parameters.value("fx");
    m_pBeats = parameters.value("beats");
    m_pTime = parameters.value("time");
    m_pLevel = parameters.value("level");
    m_pOn = parameters.value("on");
    m_pLow = parameters.value("low");
    m_pMid = parameters.value("mid");
    m_pHi = parameters.value("hi");
    m_pWave = parameters.value("wave");
    m_pQuantize = parameters.value("quantize");
    m_pTapBpm = parameters.value("tap_bpm");
}

void B2BBeatFxEffect::processChannel(
        B2BBeatFxGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(enableState); // the engine fades in and out (an input switched)
    b2bfx::Params p;
    p.fx = static_cast<int>(std::lround(m_pFx->value()));
    p.beats = m_pBeats->value();
    p.timeMs = m_pTime->value();
    p.level = m_pLevel->value();
    p.on = m_pOn->value() > 0.5;
    p.low = m_pLow->value() > 0.5;
    p.mid = m_pMid->value() > 0.5;
    p.hi = m_pHi->value() > 0.5;
    p.wave = m_pWave->value() > 0.5 ? 1 : 0;
    p.quantize = m_pQuantize->value() > 0.5;

    const double rate = engineParameters.sampleRate();
    b2bfx::Timing t;
    const double tapBpm = m_pTapBpm->value();
    if (tapBpm > 1) {
        // TAP: the tapped tempo, its phase from the taps.
        t.beatFrames = 60.0 / tapBpm * rate;
        t.hasFraction = true;
        t.fractionAtEnd = m_phase.get();
        t.fractionExact = false;
    } else if (groupFeatures.beat_length.has_value() && groupFeatures.beat_length->seconds > 0) {
        // AUTO, a deck: its own tempo and beat.
        t.beatFrames = groupFeatures.beat_length->seconds * rate;
        t.hasFraction = groupFeatures.beat_fraction_buffer_end.has_value();
        t.fractionAtEnd = groupFeatures.beat_fraction_buffer_end.value_or(0);
        t.fractionExact = true;
    } else {
        // AUTO, the master / mic / a crossfader side: the leading deck's.
        const double bpm = m_bpm.get();
        t.beatFrames = 60.0 / (bpm > 1 ? bpm : 120.0) * rate;
        t.hasFraction = bpm > 1;
        t.fractionAtEnd = m_phase.get();
        t.fractionExact = false;
    }
    const int frames = static_cast<int>(engineParameters.framesPerBuffer());
    pState->dsp.process(pInput, pOutput, frames, p, t);
}
