#pragma once

#include <QMap>
#include <memory>

#include "control/pollingcontrolproxy.h"
#include "effects/backends/builtin/b2bbeatfxdsp.h"
#include "effects/backends/effectprocessor.h"
#include "util/types.h"

/// B2B: the club mixer's BEAT FX (like a Pioneer DJM-A9's), one effect that
/// is all 14: DELAY, ECHO, PING PONG, SPIRAL, HELIX, REVERB, FLANGER,
/// PHASER, FILTER, TRIPLET FILTER, TRANS, ROLL, TRIPLET ROLL and MOBIUS.
/// The mixer's controls set its parameters (see mixxx::b2b::BeatFx); being
/// parameters, a session's other side hears the same.
///
/// Parameters (knobs, in this order):
///   fx 0..13, beats (beat value; REVERB: size %), time (ms, 0 = beat
///   timing; REVERB: %), level 0..1, on 0/1 (ON/OFF or the X-PAD), low /
///   mid / hi 0/1 (FX FREQUENCY), wave 0 SAW / 1 TRIANGLE (MOBIUS),
///   quantize 0/1, tap_bpm (0 = AUTO), xpad 0/1 (shown only).
///
/// The beat: an input deck's own (tempo and where its beat is); for the
/// master, mic and crossfader sides, or with a tapped tempo, the controls
/// [B2BBooth],beatfx_bpm and beatfx_phase (set by the app).
///
/// The unit stays switched on: OFF stops feeding the effect, so ECHO, PING
/// PONG, SPIRAL, HELIX and REVERB fade away by themselves; the others stop.
class B2BBeatFxGroupState : public EffectState {
  public:
    explicit B2BBeatFxGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              dsp(engineParameters.sampleRate()),
              sampleRate(engineParameters.sampleRate()) {
    }
    b2bfx::Dsp dsp;
    double sampleRate;
};

class B2BBeatFxEffect : public EffectProcessorImpl<B2BBeatFxGroupState> {
  public:
    B2BBeatFxEffect();
    ~B2BBeatFxEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();
    /// Makes [B2BBooth],beatfx_bpm / beatfx_phase (if not there yet).
    static void ensureTimingControls();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            B2BBeatFxGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatureState) override;

  private:
    B2BBeatFxEffect(const B2BBeatFxEffect&) = delete;
    void operator=(const B2BBeatFxEffect&) = delete;

    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pFx;
    EngineEffectParameterPointer m_pBeats;
    EngineEffectParameterPointer m_pTime;
    EngineEffectParameterPointer m_pLevel;
    EngineEffectParameterPointer m_pOn;
    EngineEffectParameterPointer m_pLow;
    EngineEffectParameterPointer m_pMid;
    EngineEffectParameterPointer m_pHi;
    EngineEffectParameterPointer m_pWave;
    EngineEffectParameterPointer m_pQuantize;
    EngineEffectParameterPointer m_pTapBpm;
    PollingControlProxy m_bpm;
    PollingControlProxy m_phase;
};
