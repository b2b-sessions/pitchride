#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <array>

namespace mixxx::b2b {

/// The room's setlist (DESIGN.md 7, room layout 4: "the back of the
/// sleeve"): every track played in the room, with its side, the time, the
/// title, the artist and the DJ, oldest first.
///
/// A track is played when it has been heard in the mix for a while (its
/// deck playing, its fader up, the crossfader not cutting it), once per
/// time it's loaded. The same rule runs wherever it's watched from:
/// - the host's app (side a) watches its own decks, which carry both DJs'
///   tracks in a session, and tells side b over the session (all of it
///   when side b links, then each new one);
/// - side b's app takes the host's word for it while linked;
/// - a listener's app watches the decks as the host's app streams them,
///   with who has each deck (the stream's "dj" and "ab" for each deck).
class SetList : public QObject {
    Q_OBJECT
  public:
    struct Entry {
        QString side;   // "A" or "B" (empty: not known)
        QDateTime at;   // when it was first heard
        QString title;
        QString artist;
        QString dj;
        QJsonObject toJson() const;
        static Entry fromJson(const QJsonObject& o);
    };
    /// A deck as it is now.
    struct Deck {
        QString id; // the track (its loading): a new id is a new play
        QString title;
        QString artist;
        bool play = false;
        double volume = 0;   // the channel fader, 0..1
        int orientation = 1; // the crossfader side: 0 left, 1 centre, 2 right
        QString side;        // whose deck: "A" / "B"
        QString dj;
    };

    /// Heard for this long, a track is in the setlist.
    static constexpr int kHeardMs = 8000;

    explicit SetList(QObject* pParent = nullptr);

    const QVector<Entry>& entries() const {
        return m_entries;
    }
    /// The decks as they are now (call a few times a second); xf: the
    /// crossfader, -1 (left) .. 1 (right).
    void observe(const QVector<Deck>& decks, double xf);
    /// An entry from elsewhere (the host's app); the same track at about
    /// the same time is there only once.
    void add(const Entry& entry);
    /// Another room (or none): the list starts again.
    void clear();

  signals:
    /// A track went in (here, or from elsewhere).
    void added(const SetList::Entry& entry);
    void changed();

  private:
    struct Watch {
        QString id;
        qint64 heardMs = 0;
        bool listed = false;
        QDateTime firstHeard;
    };
    bool append(const Entry& entry);

    QVector<Entry> m_entries;
    std::array<Watch, 4> m_watch;
    QElapsedTimer m_sinceObserved;
};

} // namespace mixxx::b2b
