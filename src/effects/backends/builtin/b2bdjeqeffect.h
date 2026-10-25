#pragma once

#include <QMap>

#include "control/pollingcontrolproxy.h"
#include "effects/backends/builtin/lvmixeqbase.h"
#include "effects/backends/effectprocessor.h"
#include "engine/filters/enginefilterbessel4.h"
#include "util/types.h"

/// B2B: a club mixer's channel EQ, like a Pioneer DJM's (900NXS2 / A9).
///
/// Three bands split by two crossovers (Bessel 4th order with the Lipshitz /
/// Vanderkooy mix, as Mixxx's Bessel4 isolator, so all knobs at the centre
/// passes the sound unchanged). The crossovers are the ones in Preferences
/// -> Mixer ("Low / Mid" and "Mid / High"; B2B starts them at 130 Hz and
/// 10 kHz): LOW is the sub and bass, the kick's punch stays in MID.
///
/// Like the DJM's EQ / ISOLATOR switch, Preferences -> Mixer picks:
///   EQ (default)  each band +6 dB (right) .. 0 (centre) .. -26 dB (left)
///   ISOLATOR      +6 dB .. 0 .. full kill
/// The cut is gradual at first and stronger towards the end of the turn.
/// The kill buttons cut a band fully in both. Gain changes ramp across each
/// audio buffer (no zipper noise).
class B2BDjEQEffectGroupState : public LVMixEQEffectGroupState<EngineFilterBessel4Low> {
  public:
    B2BDjEQEffectGroupState(const mixxx::EngineParameters& engineParameters)
            : LVMixEQEffectGroupState<EngineFilterBessel4Low>(engineParameters) {
    }
};

class B2BDjEQEffect : public EffectProcessorImpl<B2BDjEQEffectGroupState> {
  public:
    B2BDjEQEffect();
    ~B2BDjEQEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    // [Mixer Profile] B2BEqIsolator: 0 EQ, 1 ISOLATOR (a control, made by
    // the effects manager; saved by Preferences -> Mixer).
    static inline const QString kIsolatorKey = QStringLiteral("B2BEqIsolator");
    // The crossovers B2B starts with (Hz; Preferences -> Mixer changes them).
    static constexpr double kDefaultLowFrequency = 130.0;
    static constexpr double kDefaultHighFrequency = 10000.0;

    /// The knob (0 .. 1 centre .. 2) to a linear gain.
    static double bandGain(double knob, bool kill, bool isolator);

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            B2BDjEQEffectGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatureState) override;

  private:
    B2BDjEQEffect(const B2BDjEQEffect&) = delete;
    void operator=(const B2BDjEQEffect&) = delete;

    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pPotLow;
    EngineEffectParameterPointer m_pPotMid;
    EngineEffectParameterPointer m_pPotHigh;
    EngineEffectParameterPointer m_pKillLow;
    EngineEffectParameterPointer m_pKillMid;
    EngineEffectParameterPointer m_pKillHigh;
    PollingControlProxy m_pLoFreqCorner;
    PollingControlProxy m_pHiFreqCorner;
    PollingControlProxy m_pIsolator;
};
