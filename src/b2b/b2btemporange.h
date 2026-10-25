#pragma once

#include <QString>
#include <array>
#include <cmath>

/// A CDJ's tempo ranges and how finely each one sets the tempo: the one
/// place for them (the TEMPO range button, the tempo fader's steps, the
/// screen's readout).
namespace mixxx::b2b::tempo {

struct Range {
    double span;    // Mixxx's rateRange: the fader's full travel each way (0.06 = ±6 %)
    double stepPct; // the tempo moves in these steps (percent)
    const char* label;
};

inline constexpr std::array<Range, 4> kRanges = {{
        {0.06, 0.02, "\xC2\xB1" "6"},  // ±6
        {0.10, 0.05, "\xC2\xB1" "10"}, // ±10
        {0.16, 0.05, "\xC2\xB1" "16"}, // ±16
        {1.00, 0.50, "WIDE"},          // ±100
}};

/// Every deck starts on this one (±6).
inline constexpr int kFirst = 0;

/// The range nearest Mixxx's rateRange (a friend's deck in a session, or a
/// range set some other way).
inline int indexOf(double span) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(kRanges.size()); ++i) {
        if (std::fabs(kRanges[i].span - span) < std::fabs(kRanges[best].span - span)) {
            best = i;
        }
    }
    return best;
}

/// "±6", "±10", "±16", "WIDE".
inline QString label(double span) {
    return QString::fromUtf8(kRanges[indexOf(span)].label);
}

/// A tempo (percent) on the range's steps.
inline double snapPct(double pct, double span) {
    const double step = kRanges[indexOf(span)].stepPct;
    return std::round(pct / step) * step;
}

} // namespace mixxx::b2b::tempo
