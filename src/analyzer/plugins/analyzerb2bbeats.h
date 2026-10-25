#pragma once

#include <QObject>
#include <memory>
#include <vector>

#include "analyzer/plugins/analyzerplugin.h"

class FFTReal;

namespace mixxx {

/// B2B beat grid analyzer for DJ music (constant tempo).
///
/// Mixxx's Queen Mary tracker follows beats locally and often lands on the
/// wrong tempo for broken beats (drum & bass read as 3/4 or half speed) or
/// sits off the kick. Dance music has one tempo for the whole track, so this
/// analyzer looks at the whole track at once:
///
///  1. Onset strength, twice: the low end (kick) and the full spectrum
///     (log-magnitude spectral flux, 5 ms steps).
///  2. Tempo: autocorrelation of the onsets scored as a comb (the beat and
///     its multiples), folded into the preferred BPM range (default 88-175,
///     where nearly all DJ music lives).
///  3. Exact BPM: every candidate period near that tempo is tried by folding
///     the whole track onto one beat; the sharpest fold wins (0.001 BPM).
///  4. Phase: the peak of that fold, weighted to the kick, so grid lines sit
///     on the kicks; and the bar (which beat is "1") from the low end.
///
/// The result is a constant grid, returned as beat positions.
class AnalyzerB2BBeats : public AnalyzerBeatsPlugin {
  public:
    static AnalyzerPluginInfo pluginInfo() {
        return AnalyzerPluginInfo(
                "b2b-beats:1",
                QObject::tr("B2B"),
                QObject::tr("B2B Beat Grid (DJ music, constant tempo)"),
                true);
    }

    AnalyzerB2BBeats();
    ~AnalyzerB2BBeats() override;

    AnalyzerPluginInfo info() const override {
        return pluginInfo();
    }

    bool initialize(mixxx::audio::SampleRate sampleRate) override;
    bool processSamples(const CSAMPLE* pIn, SINT iLen) override;
    bool finalize() override;

    bool supportsBeatTracking() const override {
        return true;
    }
    QVector<mixxx::audio::FramePos> getBeats() const override {
        return m_resultBeats;
    }

    /// Preferred BPM range: results are doubled or halved into it.
    static void setBpmRange(double minBpm, double maxBpm);

  private:
    void processFrame();

    mixxx::audio::SampleRate m_sampleRate;
    int m_hop = 0;
    int m_window = 0;
    std::unique_ptr<FFTReal> m_pFft;
    std::vector<double> m_frame;      // sliding mono window
    std::vector<double> m_windowFn;   // Hann
    std::vector<double> m_re, m_im, m_in;
    std::vector<double> m_prevLog;    // previous frame, log magnitude per bin
    int m_filled = 0;                 // samples in m_frame
    int m_sinceHop = 0;
    int64_t m_framesSeen = 0;         // analysis frames so far
    std::vector<float> m_odfLow;      // kick band onset strength per frame
    std::vector<float> m_odfAll;      // full band onset strength per frame
    QVector<mixxx::audio::FramePos> m_resultBeats;
};

} // namespace mixxx
