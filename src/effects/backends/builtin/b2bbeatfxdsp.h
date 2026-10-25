#pragma once

// B2B: the DSP of the club mixer's BEAT FX (see b2bbeatfxeffect.h), with no
// Mixxx dependencies but the CAPS plate reverb, so it can be rendered and
// listened to outside the app (tools/b2b_beatfx_render.cpp).
//
// All 14 effects are recreated by ear from their descriptions; none copies
// another mixer's implementation.

#include <Reverb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace b2bfx {

enum Fx {
    kDelay = 0,
    kEcho,
    kPingPong,
    kSpiral,
    kHelix,
    kReverb,
    kFlanger,
    kPhaser,
    kFilter,
    kTripletFilter,
    kTrans,
    kRoll,
    kTripletRoll,
    kMobius,
    kFxCount
};

/// Effects whose sound carries on after OFF (they stop feeding, the
/// repeats / reverb fade away by themselves).
inline bool hasTail(int fx) {
    return fx == kEcho || fx == kPingPong || fx == kSpiral || fx == kHelix || fx == kReverb;
}

/// What the controls say (the effect's parameters).
struct Params {
    int fx = kFlanger;
    double beats = 1.0;   // beat value (REVERB: size in %)
    double timeMs = 0.0;  // > 0: TIME knob, manual (REVERB: %)
    double level = 0.7;   // LEVEL/DEPTH 0 .. 1
    bool on = false;      // ON/OFF or the X-PAD held
    bool low = true;      // FX FREQUENCY
    bool mid = true;
    bool hi = true;
    int wave = 0;         // MOBIUS: 0 SAW, 1 TRIANGLE
    bool quantize = true; // lock to the beat grid
};

/// Where the beat is, for this buffer.
struct Timing {
    double beatFrames = 22050; // one beat, in frames
    bool hasFraction = false;  // the beat phase at the buffer end is known
    double fractionAtEnd = 0;  // 0 .. 1
    bool fractionExact = true; // from the track (else from the app: follow gently)
};

class Dsp {
  public:
    static constexpr double kHistorySeconds = 8.0;
    static constexpr double kLineSeconds = 8.0;
    static constexpr int kChunk = 1024;

    explicit Dsp(double sampleRate)
            : m_sr(sampleRate),
              m_histFrames(static_cast<int>(sampleRate * kHistorySeconds)),
              m_lineFrames(static_cast<int>(sampleRate * kLineSeconds)),
              m_hist(static_cast<size_t>(m_histFrames) * 2, 0.f),
              m_line(static_cast<size_t>(m_lineFrames) * 2, 0.f),
              m_fxIn(kChunk * 2),
              m_rest(kChunk * 2),
              m_wet(kChunk * 2) {
        m_reverb.init(static_cast<float>(sampleRate));
        setBandFilters();
    }

    /// in / out: interleaved stereo, frames long (may be the same buffer).
    void process(const float* in, float* out, int frames, const Params& p, const Timing& t) {
        // The beat phase: where the beat will be at this buffer's end.
        const double beatFrames = std::max(64.0, t.beatFrames);
        if (t.hasFraction) {
            const double predicted = m_beatPos + frames / beatFrames;
            double err = t.fractionAtEnd - (predicted - std::floor(predicted));
            err -= std::round(err); // -0.5 .. 0.5
            if (t.fractionExact || std::fabs(err) > 0.2) {
                m_beatPos += err;
            } else {
                m_beatPos += 0.1 * err;
            }
        }
        int done = 0;
        while (done < frames) {
            const int n = std::min(kChunk, frames - done);
            processChunk(in + 2 * done, out + 2 * done, n, p, beatFrames);
            done += n;
        }
        // Keep the counters small (whole multiples of 16 beats).
        if (m_beatPos > 1 << 20) {
            m_beatPos -= 16 * std::floor((m_beatPos - 1024) / 16);
        }
    }

  private:
    // ------------------------------------------------------------ helpers --
    struct Svf {
        double ic1[2] = {0, 0};
        double ic2[2] = {0, 0};
        double a1 = 0, a2 = 0, a3 = 0, k = 1.414;
        void set(double fc, double sr, double q) {
            const double g = std::tan(M_PI * std::min(fc, sr * 0.45) / sr);
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        // Returns low; band / high through the pointers.
        double tick(int ch, double x, double* band = nullptr, double* high = nullptr) {
            const double v3 = x - ic2[ch];
            const double v1 = a1 * ic1[ch] + a2 * v3;
            const double v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
            ic1[ch] = 2 * v1 - ic1[ch];
            ic2[ch] = 2 * v2 - ic2[ch];
            if (band) {
                *band = v1;
            }
            if (high) {
                *high = x - k * v1 - v2;
            }
            return v2;
        }
    };

    static double frac(double x) {
        return x - std::floor(x);
    }
    // One step of a linear ramp towards target, rate per frame.
    static double toward(double v, double target, double rate) {
        if (v < target) {
            return std::min(target, v + rate);
        }
        return std::max(target, v - rate);
    }

    void setBandFilters() {
        m_lowSplit.set(250.0, m_sr, 0.7071);
        m_highSplit.set(3000.0, m_sr, 0.7071);
    }

    // The feedback line: interleaved, m_lineFrames long; reads before the
    // effect started writing give silence.
    float lineAt(int ch, double delayFrames) const {
        if (delayFrames >= m_lineValid) {
            return 0.f;
        }
        const double pos = m_lineW - delayFrames;
        double base = std::floor(pos);
        const double f = pos - base;
        int i0 = static_cast<int>(base) % m_lineFrames;
        if (i0 < 0) {
            i0 += m_lineFrames;
        }
        const int i1 = (i0 + 1) % m_lineFrames;
        return static_cast<float>((1 - f) * m_line[2 * i0 + ch] + f * m_line[2 * i1 + ch]);
    }
    void lineWrite(float l, float r) {
        m_line[2 * m_lineW] = l;
        m_line[2 * m_lineW + 1] = r;
        m_lineW = (m_lineW + 1) % m_lineFrames;
        m_lineValid = std::min(m_lineValid + 1, static_cast<double>(m_lineFrames - 2));
    }

    void resetFx() {
        m_lineValid = 0; // forget the old line (no memset in the audio thread)
        m_echoLp[0] = m_echoLp[1] = 0;
        m_helixHp[0] = m_helixHp[1] = 0;
        m_helixLp[0] = m_helixLp[1] = 0;
        m_helixHpIn[0] = m_helixHpIn[1] = 0;
        for (auto& ch : m_ap) {
            std::fill(std::begin(ch), std::end(ch), 0.0);
        }
        m_phaserFb[0] = m_phaserFb[1] = 0;
        m_reverb.activate();
        m_reverbSend = 0;
        m_rolling = false;
        m_transGain = 1;
        m_mobiusS = 0;
        m_mobiusU = 0;
        std::fill(std::begin(m_osc), std::end(m_osc), 0.0);
        m_lfoFree = 0;
        m_periodS = 0;
    }

    static constexpr int kOsc = 9;

    // -------------------------------------------------------------- chunk --
    void processChunk(const float* in, float* out, int n, const Params& p, double beatFrames) {
        const double sr = m_sr;
        const double gateRate = 1.0 / (0.008 * sr);  // 8 ms on / off
        const double fadeRate = 1.0 / (0.012 * sr);  // 12 ms effect change
        const double bandRate = 1.0 / (0.010 * sr);
        const double levelRate = 1.0 / (0.030 * sr);

        // Which effect: a change fades the old one out, then the new one in.
        const int wantFx = std::clamp(p.fx, 0, kFxCount - 1);

        // The bands into the effect, the rest stays dry.
        for (int i = 0; i < n; ++i) {
            m_wLow = toward(m_wLow, p.low ? 1 : 0, bandRate);
            m_wMid = toward(m_wMid, p.mid ? 1 : 0, bandRate);
            m_wHigh = toward(m_wHigh, p.hi ? 1 : 0, bandRate);
            for (int ch = 0; ch < 2; ++ch) {
                const double x = in[2 * i + ch];
                const double low = m_lowSplit.tick(ch, x);
                double high = 0;
                m_highSplit.tick(ch, x, nullptr, &high);
                const double midBand = x - low - high;
                double sel = x;
                if (m_wLow < 1 || m_wMid < 1 || m_wHigh < 1) {
                    sel = m_wLow * low + m_wMid * midBand + m_wHigh * high;
                }
                m_fxIn[2 * i + ch] = static_cast<float>(sel);
                m_rest[2 * i + ch] = static_cast<float>(x - sel);
            }
        }

        // The effect's period (frames) and how its parts are timed.
        double period;
        const bool manual = p.timeMs > 0 && wantFx != kReverb;
        if (manual) {
            period = p.timeMs * sr / 1000.0;
        } else {
            period = std::max(1.0 / 32, p.beats) * beatFrames;
        }
        period = std::clamp(period, 16.0, m_lineFrames * 0.45);
        const double beatsPerCycle = manual ? period / beatFrames : std::max(1.0 / 32, p.beats);

        // REVERB: size in % (beat value or TIME).
        const double reverbPct = std::clamp(p.timeMs > 0 && wantFx == kReverb ? p.timeMs : p.beats, 1.0, 100.0);

        if (wantFx == kReverb || m_fx == kReverb) {
            // The plate works in blocks: send ramps over the chunk.
            const double targetGate = (p.on && m_fx == wantFx) ? 1.0 : 0.0;
            const double sendNow = std::clamp(
                    toward(m_gate, targetGate, gateRate * n) * toward(m_fade, m_fx == wantFx ? 1 : 0, fadeRate * n),
                    0.0, 1.0);
            const float decay = static_cast<float>(0.25 + 0.74 * reverbPct / 100.0);
            m_reverb.processBuffer(m_fxIn.data(), m_wet.data(), static_cast<unsigned>(2 * n),
                    0.85f, decay, 0.35f, static_cast<float>(sendNow), static_cast<float>(m_reverbSend));
            m_reverbSend = sendNow;
        }

        for (int i = 0; i < n; ++i) {
            // Effect change: fade out, switch, fade in.
            if (wantFx != m_fx) {
                m_fade = toward(m_fade, 0, fadeRate);
                if (m_fade <= 0) {
                    m_fx = wantFx;
                    resetFx();
                    if (p.on) {
                        startEngaged(p, period, beatsPerCycle);
                    }
                }
            } else {
                m_fade = toward(m_fade, 1, fadeRate);
            }
            const bool wasOn = m_gateTarget;
            m_gateTarget = p.on;
            if (m_gateTarget && !wasOn) {
                startEngaged(p, period, beatsPerCycle);
            }
            m_gate = toward(m_gate, p.on ? 1 : 0, gateRate);
            m_level = toward(m_level, std::clamp(p.level, 0.0, 1.0), levelRate);

            m_beatPos += 1.0 / beatFrames;
            m_freePos += 1.0 / beatFrames;
            m_lfoFree = frac(m_lfoFree + 1.0 / period);
            // Where in its cycle the effect is: locked to the grid (QUANTIZE),
            // or counted from when it was switched on.
            double cycle;
            if (manual) {
                cycle = m_lfoFree;
            } else {
                cycle = frac((p.quantize ? m_beatPos : m_freePos) / beatsPerCycle);
            }

            // The delays glide to a new length (X-PAD, BEAT) instead of jumping.
            if (m_periodS <= 0) {
                m_periodS = period;
            }
            m_periodS += (period - m_periodS) * (1.0 / (0.04 * sr));
            const double delay = m_periodS;
            const double G = m_gate * m_fade; // the effect's sound, on / off
            const double L = m_level;
            const float aL = m_fxIn[2 * i];
            const float aR = m_fxIn[2 * i + 1];
            double yL = aL;
            double yR = aR;

            // The history (rolls): stops recording once a roll has a long
            // enough piece, so what it repeats isn't overwritten.
            const bool writeHist = !m_rolling || (m_abs - m_rollStart) < m_histFrames / 2;
            if (writeHist) {
                m_hist[2 * m_histW] = aL;
                m_hist[2 * m_histW + 1] = aR;
                m_histW = (m_histW + 1) % m_histFrames;
                m_histAbs = m_abs + 1;
            }

            switch (m_fx) {
            case kDelay: {
                const double dL = lineAt(0, delay);
                const double dR = lineAt(1, delay);
                lineWrite(aL, aR);
                yL = aL + G * L * dL;
                yR = aR + G * L * dR;
                break;
            }
            case kEcho: {
                const double fb = 0.35 + 0.5 * L;
                const double dL = lineAt(0, delay);
                const double dR = lineAt(1, delay);
                m_echoLp[0] += 0.45 * (dL - m_echoLp[0]); // repeats get darker
                m_echoLp[1] += 0.45 * (dR - m_echoLp[1]);
                const double send = G;
                lineWrite(static_cast<float>(send * aL + fb * m_echoLp[0]),
                        static_cast<float>(send * aR + fb * m_echoLp[1]));
                yL = aL + m_fade * L * dL;
                yR = aR + m_fade * L * dR;
                break;
            }
            case kPingPong: {
                const double fb = 0.35 + 0.5 * L;
                const double dL = lineAt(0, delay);
                const double dR = lineAt(1, delay);
                const double mono = 0.5 * (aL + aR);
                // The first repeat on the left, the next on the right ...
                lineWrite(static_cast<float>(G * mono + fb * dR), static_cast<float>(fb * dL));
                yL = aL + m_fade * L * dL;
                yR = aR + m_fade * L * dR;
                break;
            }
            case kSpiral: {
                // Echo whose every repeat is shifted up (two crossfaded
                // read heads gliding through the line).
                const double window = 0.05 * m_sr;
                const double ratio = std::pow(2.0, 2.0 / 12.0); // +2 semitones a repeat
                m_spiral = frac(m_spiral + (ratio - 1.0) / window);
                double sL = 0;
                double sR = 0;
                for (int h = 0; h < 2; ++h) {
                    const double ph = frac(m_spiral + 0.5 * h);
                    const double d = delay + window * (1.0 - ph);
                    const double g = std::sin(M_PI * ph);
                    sL += g * lineAt(0, d);
                    sR += g * lineAt(1, d);
                }
                const double fb = 0.4 + 0.45 * L;
                lineWrite(static_cast<float>(G * aL + fb * sL), static_cast<float>(G * aR + fb * sR));
                yL = aL + m_fade * L * sL;
                yR = aR + m_fade * L * sR;
                break;
            }
            case kHelix: {
                // The sound caught on a loop of the beat length that keeps
                // going round, building up and wearing a little each pass.
                const double hold = 0.62 + 0.25 * L;
                const double dL = lineAt(0, delay);
                const double dR = lineAt(1, delay);
                double t[2] = {dL, dR};
                for (int ch = 0; ch < 2; ++ch) {
                    m_helixLp[ch] += 0.35 * (t[ch] - m_helixLp[ch]);   // darker
                    const double hp = m_helixLp[ch] - m_helixHpIn[ch] + 0.995 * m_helixHp[ch];
                    m_helixHpIn[ch] = m_helixLp[ch];
                    m_helixHp[ch] = hp;                                // thinner
                    t[ch] = hp;
                }
                const double send = G * (0.3 + 0.5 * L);
                lineWrite(static_cast<float>(send * aL + hold * t[0]), static_cast<float>(send * aR + hold * t[1]));
                const double duck = 1.0 - 0.5 * G * L;
                yL = aL * duck + m_fade * 0.8 * dL;
                yR = aR * duck + m_fade * 0.8 * dR;
                break;
            }
            case kReverb: {
                yL = aL + m_fade * L * 1.3 * m_wet[2 * i];
                yR = aR + m_fade * L * 1.3 * m_wet[2 * i + 1];
                break;
            }
            case kFlanger: {
                const double lfo = 0.5 - 0.5 * std::cos(2 * M_PI * cycle);
                const double d = (0.0004 + 0.0045 * lfo) * m_sr;
                const double fL = lineAt(0, d);
                const double fR = lineAt(1, d);
                lineWrite(static_cast<float>(aL + 0.7 * fL), static_cast<float>(aR + 0.7 * fR));
                yL = aL + G * L * (0.5 * (aL + fL) - aL);
                yR = aR + G * L * (0.5 * (aR + fR) - aR);
                break;
            }
            case kPhaser: {
                const double lfo = 0.5 - 0.5 * std::cos(2 * M_PI * cycle);
                const double fc = 200.0 * std::pow(20.0, lfo); // 200 Hz .. 4 kHz
                const double tn = std::tan(M_PI * fc / m_sr);
                const double c = (tn - 1) / (tn + 1);
                const double a[2] = {aL, aR};
                double ap[2];
                for (int ch = 0; ch < 2; ++ch) {
                    double x = a[ch] + 0.6 * m_phaserFb[ch];
                    for (int s = 0; s < 6; ++s) {
                        const double y = c * x + m_ap[ch][s];
                        m_ap[ch][s] = x - c * y;
                        x = y;
                    }
                    m_phaserFb[ch] = x;
                    ap[ch] = x;
                }
                yL = aL + G * L * (0.5 * (aL + ap[0]) - aL);
                yR = aR + G * L * (0.5 * (aR + ap[1]) - aR);
                break;
            }
            case kFilter:
            case kTripletFilter: {
                const double lfo = 0.5 - 0.5 * std::cos(2 * M_PI * cycle);
                const double fc = 18000.0 * std::pow(120.0 / 18000.0, L * lfo);
                if ((i & 15) == 0) {
                    m_sweep.set(fc, m_sr, 1.6);
                }
                const double lL = m_sweep.tick(0, aL);
                const double lR = m_sweep.tick(1, aR);
                yL = aL + G * (lL - aL);
                yR = aR + G * (lR - aR);
                break;
            }
            case kTrans: {
                const double closed = cycle >= 0.5 ? 1.0 : 0.0;
                m_transGain += (1.0 - closed - m_transGain) * (1.0 / (0.002 * m_sr)); // 2 ms edges
                const double gain = 1.0 - G * L * (1.0 - std::clamp(m_transGain, 0.0, 1.0));
                yL = aL * gain;
                yR = aR * gain;
                break;
            }
            case kRoll:
            case kTripletRoll: {
                float rL = aL;
                float rR = aR;
                if (m_rolling) {
                    const long long into = m_abs - m_rollStart;
                    const long long len = std::max<long long>(16, static_cast<long long>(period));
                    const long long pos = into % len;
                    readHist(m_rollStart + pos, &rL, &rR);
                    // Each time round, 3 ms across the seam: the piece's start
                    // fades in over what followed its end.
                    const long long xf = static_cast<long long>(0.003 * m_sr);
                    if (into >= len && pos < xf && len > 4 * xf) {
                        float cL = rL;
                        float cR = rR;
                        readHist(m_rollStart + len + pos, &cL, &cR);
                        const double w = static_cast<double>(pos) / xf;
                        rL = static_cast<float>(w * rL + (1 - w) * cL);
                        rR = static_cast<float>(w * rR + (1 - w) * cR);
                    }
                    if (!m_gateTarget && m_gate <= 0) {
                        m_rolling = false; // back to the track, where it is now
                    }
                }
                const double w = G * (0.4 + 0.6 * L);
                yL = aL + w * (rL - aL);
                yR = aR + w * (rR - aR);
                break;
            }
            case kMobius: {
                // An endlessly rising (SAW) or rising-and-falling (TRIANGLE)
                // tone: octaves under a bell over pitch, gliding.
                const double step = 1.0 / period; // an octave per beat value
                if (p.wave == 0) {
                    m_mobiusS += step;
                    if (m_mobiusS >= 1) {
                        m_mobiusS -= 1;
                        // Each voice is now where the one below it was: hand the
                        // phases up; a silent one comes in at the bottom.
                        for (int k = kOsc - 1; k > 0; --k) {
                            m_osc[k] = m_osc[k - 1];
                        }
                        m_osc[0] = 0;
                    }
                } else {
                    m_mobiusU = std::fmod(m_mobiusU + step, 2.0);
                    m_mobiusS = m_mobiusU < 1 ? m_mobiusU : 2 - m_mobiusU;
                }
                double tone = 0;
                for (int k = 0; k < kOsc; ++k) {
                    const double oct = k + m_mobiusS;
                    const double f = 27.5 * std::pow(2.0, oct);
                    // Silent at both ends of the range, loudest in the middle.
                    const double amp = 0.5 - 0.5 * std::cos(2 * M_PI * oct / kOsc);
                    m_osc[k] = frac(m_osc[k] + f / m_sr);
                    tone += amp * std::sin(2 * M_PI * m_osc[k]);
                }
                tone *= 0.09;
                yL = aL + G * L * tone;
                yR = aR + G * L * tone;
                break;
            }
            default:
                break;
            }
            ++m_abs;
            out[2 * i] = static_cast<float>(m_rest[2 * i] + yL);
            out[2 * i + 1] = static_cast<float>(m_rest[2 * i + 1] + yR);
        }
    }

    // The history at an absolute frame (unchanged if it's gone).
    void readHist(long long at, float* pL, float* pR) const {
        const long long back = (m_histAbs - 1) - at;
        if (back < 0 || back >= m_histFrames - 1) {
            return;
        }
        int idx = (m_histW - 1 - static_cast<int>(back)) % m_histFrames;
        if (idx < 0) {
            idx += m_histFrames;
        }
        *pL = m_hist[2 * idx];
        *pR = m_hist[2 * idx + 1];
    }

    // ON (or the X-PAD) pressed: count the free timing from here; a roll
    // catches its piece (from the last beat of its length with QUANTIZE, so
    // it starts on the beat; from now without).
    void startEngaged(const Params& p, double period, double beatsPerCycle) {
        m_freePos = 0;
        m_lfoFree = 0;
        if (m_fx == kRoll || m_fx == kTripletRoll) {
            long long since = 0;
            if (p.quantize && p.timeMs <= 0) {
                since = static_cast<long long>(frac(m_beatPos / beatsPerCycle) * period);
            }
            m_rollStart = m_abs - since;
            m_rolling = true;
        }
    }

    const double m_sr;
    const int m_histFrames;
    const int m_lineFrames;
    std::vector<float> m_hist;
    std::vector<float> m_line;
    std::vector<float> m_fxIn;
    std::vector<float> m_rest;
    std::vector<float> m_wet;
    int m_histW = 0;
    long long m_histAbs = 0;
    int m_lineW = 0;
    double m_lineValid = 0;
    long long m_abs = 0;

    Svf m_lowSplit;
    Svf m_highSplit;
    Svf m_sweep;
    double m_wLow = 1, m_wMid = 1, m_wHigh = 1;

    int m_fx = kFlanger;
    double m_fade = 1;
    double m_gate = 0;
    bool m_gateTarget = false;
    double m_level = 0.7;
    double m_beatPos = 0;
    double m_freePos = 0;
    double m_lfoFree = 0;
    double m_periodS = 0;

    double m_echoLp[2] = {0, 0};
    double m_spiral = 0;
    double m_helixLp[2] = {0, 0};
    double m_helixHp[2] = {0, 0};
    double m_helixHpIn[2] = {0, 0};
    double m_ap[2][6] = {};
    double m_phaserFb[2] = {0, 0};
    MixxxPlateX2 m_reverb;
    double m_reverbSend = 0;
    bool m_rolling = false;
    long long m_rollStart = 0;
    double m_transGain = 1;
    double m_mobiusS = 0;
    double m_mobiusU = 0;
    double m_osc[kOsc] = {};
};

} // namespace b2bfx
