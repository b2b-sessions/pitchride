#pragma once

#include <QObject>
#include <QString>
#include <array>

#include "preferences/configobject.h"

namespace mixxx::b2b::routing {

/// "A deck each" with controllers: your controller's channels 1 and 2 are
/// always your two decks. A DDJ-400 on the friend's side plays decks 3 and
/// 4; on the host's side nothing changes.
///
/// The controller code (scripts and plain MIDI mappings) asks here which
/// deck a group it names really is: "[Channel1]" -> "[Channel3]", and the
/// groups that carry a deck inside them ("[EqualizerRack1_[Channel1]_Effect1]",
/// "group_[Channel1]_enable"). Scripts never see the routed names: a
/// callback gets the group the script asked for. While the decks are
/// split, a controller's crossfader moves nothing (two crossfaders on one
/// mix would fight); the one on screen still works.
///
/// The controllers' thread reads it; the main thread sets it. When the
/// routing changes, the controllers are set up again (their mappings are
/// loaded with the old routing).

/// Main thread: where each of the controller's channels goes (deck[n] is
/// the deck, 0..3, for its channel n+1), and whether its crossfader is
/// ignored. Says whether the decks moved (then changed() follows).
bool set(const std::array<int, 4>& decks, bool ignoreCrossfader);
/// No routing: channel n is deck n, the crossfader works.
inline bool clear() {
    return set({0, 1, 2, 3}, false);
}

/// The deck a controller's channel (0..3) plays.
int deck(int channel);
/// A group as it really is ("[Channel1]" -> "[Channel3]").
QString group(const QString& group);
/// A control as it really is (its group, and a deck named in its item).
ConfigKey key(const ConfigKey& key);
/// A controller may not move this (routed) control now: the crossfader,
/// while the decks are split.
bool ignored(const ConfigKey& routedKey);

/// Says changed() when the decks move: ControllerManager sets the
/// controllers up again.
class Notifier : public QObject {
    Q_OBJECT
  public:
    static Notifier* instance();
  signals:
    void changed();
};

} // namespace mixxx::b2b::routing
