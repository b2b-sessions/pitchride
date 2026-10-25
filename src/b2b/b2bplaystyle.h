#pragma once

#include <QCoreApplication>
#include <QString>

namespace mixxx::b2b {

/// How a room plays, as its host chooses it (the host dialog, the room
/// strip's Change). Split is the default: two people, usually a 2-channel
/// controller each, and each controller plays its own two decks.
enum class PlayStyle {
    Split = 0,   ///< a deck each: decks 1 and 2 the host's, 3 and 4 the friend's
    AllMine = 1, ///< a deck each: every deck the host's; the friend asks with +
    Turns = 2,   ///< taking turns: one of you is live at a time
};

/// The choice in words (the same in the dialog and the strip's menu).
inline QString playStyleText(PlayStyle style) {
    switch (style) {
    case PlayStyle::Split:
        return QCoreApplication::translate("PlayStyle", "two decks each: 1 and 2 are yours, 3 and 4 your friend's");
    case PlayStyle::AllMine:
        return QCoreApplication::translate("PlayStyle", "you start with every deck: your friend asks for one");
    case PlayStyle::Turns:
        return QCoreApplication::translate("PlayStyle", "take turns: one of you plays at a time");
    }
    return QString();
}

} // namespace mixxx::b2b
