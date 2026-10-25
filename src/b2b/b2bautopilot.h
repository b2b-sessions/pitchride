#pragma once

#include <QElapsedTimer>
#include <QString>
#include <functional>
#include <vector>

class PlayerManager;

namespace mixxx::b2b {

/// Test helper for the leader: performs a scripted DJ set (loads, play/pause,
/// crossfader sweeps, EQ kills, seeks, tempo changes, random moves) by setting
/// Mixxx controls exactly like a controller would. Lets two Mixxx instances be
/// compared without a human or a controller.
///
/// Enabled with B2B_AUTOPILOT="/path/trackA.mp3|/path/trackB.mp3" (host), or
/// B2B_AUTOPILOT="moves|/path/trackC.mp3" for the guest: once it is live it
/// swaps deck 1 to track C (testing loads in the other direction), then makes
/// random moves. Moves only happen while this side is live.
class Autopilot {
  public:
    /// `startDelaySec`: wait this long before the first move (lets Mixxx finish starting up).
    Autopilot(PlayerManager* pPlayerManager, const QString& spec, int numDecks, double startDelaySec);

    /// Call regularly from the main thread.
    void tick(bool live);

    bool finished() const {
        return m_next >= m_steps.size();
    }

  private:
    struct Step {
        double atSec;
        std::function<void()> action;
    };
    void at(double sec, std::function<void()> action);
    void whileListening(double sec, std::function<void()> action);
    static void set(const QString& group, const QString& item, double value);

    PlayerManager* m_pPlayerManager;
    std::vector<Step> m_steps;
    std::vector<Step> m_listenSteps; // guest: deck prep tests before it goes live
    size_t m_nextListen = 0;
    QElapsedTimer m_sinceCreated;
    double m_listenStartDelaySec = 0;
    size_t m_next = 0;
    QElapsedTimer m_elapsed;
    double m_startDelaySec = 0;
    bool m_guest = false;     // script starts when we first go live
    bool m_together = false;  // "a deck each" test: moves whoever is live
    bool m_started = false;
};

} // namespace mixxx::b2b
