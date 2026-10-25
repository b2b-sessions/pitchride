#pragma once

#include <QString>
#include <atomic>
#include <cstdint>

/// Online B2B, "a deck each": the mixer row is anyone's. A move on the
/// friend's channel (EQ, filter, trim, fader) is heard `lead` after it's
/// made, at the same moment on both computers, like the friend's own moves
/// on it. The fader and trim are timed by the B2B hook itself; the EQ and
/// filter go through Mixxx's effects, so their changes carry the moment
/// they're due (EffectsRequest::b2bDue) and the engine holds them till then.
namespace mixxx::b2b::mixtiming {

/// The engine's content time for this audio buffer (ns), while playing a
/// deck each; 0 otherwise. Set by the B2B hook each callback.
inline std::atomic<int64_t> g_contentNow{0};

/// Main thread: when a change to this effect control is due (content time,
/// ns), or 0 for at once. Set while a B2B session is on.
using DueFn = int64_t (*)(const QString& group, const QString& item);
inline std::atomic<DueFn> g_due{nullptr};

inline int64_t due(const QString& group, const QString& item) {
    const DueFn f = g_due.load(std::memory_order_acquire);
    return f ? f(group, item) : 0;
}

} // namespace mixxx::b2b::mixtiming
