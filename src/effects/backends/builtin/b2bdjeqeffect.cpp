#include "effects/backends/builtin/b2bdjeqeffect.h"

#include <cmath>

#include "effects/backends/builtin/equalizer_util.h"
#include "effects/backends/effectmanifest.h"
#include "effects/defs.h"
#include "engine/effects/engineeffectparameter.h"

namespace {

// ---- Tuning (by ear against a real DJM) -------------------------------
// The crossovers are in Preferences -> Mixer (B2B's starting values are
// kDefaultLowFrequency / kDefaultHighFrequency in the header).
constexpr double kMaxDb = 6.0;      // knob fully right
constexpr double kEqMinDb = -26.0;  // EQ mode, knob fully left
constexpr double kIsoMinDb = -40.0; // ISOLATOR: the curve heads here ...
constexpr double kIsoFade = 4.0;    // ... and fades to silence at the end
// How the cut builds up over the knob's left half: dB = min * x^kCurve
// (x 0 at the centre .. 1 fully left). 1 = even in dB; higher = gentler
// at first, stronger at the end. 1.8: a quarter of the way -2.1 dB, half
// way -7.5 dB, three quarters -15.5 dB, fully left -26 dB.
constexpr double kCurve = 1.8;
// ------------------------------------------------------------------------

double dbToGain(double db) {
    return std::pow(10.0, db / 20.0);
}

} // namespace

// static
QString B2BDjEQEffect::getId() {
    return "org.mixxx.effects.b2bdjeq";
}

// static
EffectManifestPointer B2BDjEQEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("DJ Mixer Equalizer"));
    pManifest->setShortName(QObject::tr("DJ EQ"));
    pManifest->setAuthor("B2B");
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "A club mixer's 3-band EQ, like a Pioneer DJM: in EQ mode each "
            "band goes from +6 dB to -26 dB, in ISOLATOR mode to a full "
            "kill (Preferences -> Mixer). LOW is the sub and bass, below the "
            "Low / Mid crossover; HIGH is above the Mid / High crossover.") +
            " " + EqualizerUtil::adjustFrequencyShelvesTip());
    pManifest->setIsMixingEQ(true);
    pManifest->setEffectRampsFromDry(true);
    EqualizerUtil::createCommonParameters(pManifest.data(), true); // 0 .. 1 .. 2
    return pManifest;
}

B2BDjEQEffect::B2BDjEQEffect()
        : m_pLoFreqCorner(kMixerProfile, kLowEqFrequency),
          m_pHiFreqCorner(kMixerProfile, kHighEqFrequency),
          m_pIsolator(kMixerProfile, kIsolatorKey) {
}

// static
double B2BDjEQEffect::bandGain(double knob, bool kill, bool isolator) {
    if (kill) {
        return 0.0;
    }
    if (knob >= 1.0) {
        return dbToGain(kMaxDb * std::min(knob - 1.0, 1.0)); // exactly 1 at the centre
    }
    const double x = std::clamp(1.0 - knob, 0.0, 1.0);
    const double shape = std::pow(x, kCurve);
    if (isolator) {
        return dbToGain(kIsoMinDb * shape) * (1.0 - std::pow(x, kIsoFade));
    }
    return dbToGain(kEqMinDb * shape);
}

void B2BDjEQEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pPotLow = parameters.value("low");
    m_pPotMid = parameters.value("mid");
    m_pPotHigh = parameters.value("high");
    m_pKillLow = parameters.value("killLow");
    m_pKillMid = parameters.value("killMid");
    m_pKillHigh = parameters.value("killHigh");
}

void B2BDjEQEffect::processChannel(
        B2BDjEQEffectGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);
    if (enableState == EffectEnableState::Disabling) {
        // Ramp to dry (and from dry when enabling).
        pState->processChannelAndPause(pInput, pOutput, engineParameters.samplesPerBuffer());
        return;
    }
    const bool isolator = m_pIsolator.toBool();
    pState->processChannel(pInput,
            pOutput,
            engineParameters.samplesPerBuffer(),
            engineParameters.sampleRate(),
            bandGain(m_pPotLow->value(), m_pKillLow->toBool(), isolator),
            bandGain(m_pPotMid->value(), m_pKillMid->toBool(), isolator),
            bandGain(m_pPotHigh->value(), m_pKillHigh->toBool(), isolator),
            m_pLoFreqCorner.get(),
            m_pHiFreqCorner.get());
}
