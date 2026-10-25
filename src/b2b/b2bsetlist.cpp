#include "b2b/b2bsetlist.h"

#include <algorithm>
#include <cstdlib>

#include "moc_b2bsetlist.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kMaxEntries = 500;
// The same track this close to one already listed is that one again.
constexpr qint64 kSameWithinSec = 180;
} // namespace

QJsonObject SetList::Entry::toJson() const {
    return QJsonObject{{QStringLiteral("s"), side},
            {QStringLiteral("t"), static_cast<double>(at.toSecsSinceEpoch())},
            {QStringLiteral("ti"), title},
            {QStringLiteral("ar"), artist},
            {QStringLiteral("dj"), dj}};
}

SetList::Entry SetList::Entry::fromJson(const QJsonObject& o) {
    Entry e;
    e.side = o.value(QStringLiteral("s")).toString().left(1);
    e.at = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(o.value(QStringLiteral("t")).toDouble()));
    e.title = o.value(QStringLiteral("ti")).toString().left(200);
    e.artist = o.value(QStringLiteral("ar")).toString().left(200);
    e.dj = o.value(QStringLiteral("dj")).toString().left(80);
    return e;
}

SetList::SetList(QObject* pParent)
        : QObject(pParent) {
}

void SetList::clear() {
    m_entries.clear();
    m_watch = {};
    m_sinceObserved.invalidate();
    emit changed();
}

void SetList::observe(const QVector<Deck>& decks, double xf) {
    // The time since the last look (at most a second: a pause in the
    // looking isn't a second of listening).
    const qint64 dt = m_sinceObserved.isValid() ? std::min<qint64>(m_sinceObserved.restart(), 1000) : 0;
    if (!m_sinceObserved.isValid()) {
        m_sinceObserved.start();
    }
    for (int d = 0; d < static_cast<int>(m_watch.size()); ++d) {
        Watch& w = m_watch[d];
        const Deck* pDeck = d < decks.size() ? &decks[d] : nullptr;
        if (!pDeck || pDeck->id.isEmpty()) {
            w = Watch{};
            continue;
        }
        if (pDeck->id != w.id) {
            w = Watch{};
            w.id = pDeck->id;
        }
        // In the mix: playing, its fader up, and the crossfader not
        // cutting its side.
        const bool cut = (pDeck->orientation == 0 && xf > 0.95) || (pDeck->orientation == 2 && xf < -0.95);
        const bool heard = pDeck->play && pDeck->volume > 0.05 && !cut;
        if (!heard || w.listed) {
            continue;
        }
        if (w.heardMs == 0) {
            w.firstHeard = QDateTime::currentDateTime();
        }
        w.heardMs += dt;
        if (w.heardMs >= kHeardMs) {
            w.listed = true;
            Entry e;
            e.side = pDeck->side;
            e.at = w.firstHeard;
            e.title = pDeck->title;
            e.artist = pDeck->artist;
            e.dj = pDeck->dj;
            if (append(e)) {
                emit added(e);
                emit changed();
            }
        }
    }
}

void SetList::add(const Entry& entry) {
    if (append(entry)) {
        emit added(entry);
        emit changed();
    }
}

bool SetList::append(const Entry& entry) {
    if (entry.title.isEmpty() || !entry.at.isValid()) {
        return false;
    }
    for (const Entry& e : std::as_const(m_entries)) {
        if (e.title == entry.title && e.artist == entry.artist &&
                std::llabs(e.at.secsTo(entry.at)) < kSameWithinSec) {
            return false;
        }
    }
    // In time order (an entry from elsewhere may be older).
    auto at = std::upper_bound(m_entries.begin(), m_entries.end(), entry, [](const Entry& a, const Entry& b) {
        return a.at < b.at;
    });
    m_entries.insert(at, entry);
    while (m_entries.size() > kMaxEntries) {
        m_entries.removeFirst();
    }
    return true;
}

} // namespace mixxx::b2b
