#include "b2b/b2bnowplaying.h"

#include <QBoxLayout>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPaintEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QWidget>
#include <cmath>

#include "b2b/b2bmanager.h"
#include "b2b/b2btheme.h"
#include "control/controlobject.h"
#include "mixer/deck.h"
#include "mixer/playermanager.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "widget/wlabel.h"
#include "moc_b2bnowplaying.cpp"

namespace mixxx::b2b {

/// One deck's line: copper DM Mono capitals. Too long for the player: it
/// waits a moment, glides along to the end, comes round to the start again
/// after a gap, and waits again. Paints itself (a plain widget, no style).
class NowLine : public QWidget {
  public:
    explicit NowLine(QWidget* pParent)
            : QWidget(pParent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumWidth(40);
        m_tick.setInterval(33); // ~30 steps a second while it moves
        QObject::connect(&m_tick, &QTimer::timeout, this, [this] { step(); });
    }
    void setText(const QString& text) {
        if (text != m_text) {
            // (A new track starts again from the beginning; the same one
            // with a new BPM or "now:" carries on where it is.)
            if (trackOf(text) != trackOf(m_text)) {
                restart();
            }
            m_text = text;
            setToolTip(text);
            updateMoving();
            update();
        }
    }
    QString text() const {
        return m_text;
    }
    QSize sizeHint() const override {
        return QSize(200, 14);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QFont f = lineFont();
        p.setFont(f);
        p.setPen(QColor(theme::kCopper));
        const QString shown = m_text.toUpper();
        const int textWidth = QFontMetrics(f).horizontalAdvance(shown);
        if (textWidth <= width()) {
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, shown);
            return;
        }
        // Along the line: the text, a gap, the text again (so the start
        // comes round as the end goes).
        p.setClipRect(rect());
        const double x = -m_offset;
        const double next = x + textWidth + gap(f);
        p.drawText(QRectF(x, 0, textWidth + 1, height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
        if (next < width()) {
            p.drawText(QRectF(next, 0, textWidth + 1, height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
        }
    }
    void resizeEvent(QResizeEvent* pEvent) override {
        QWidget::resizeEvent(pEvent);
        updateMoving();
    }
    void showEvent(QShowEvent* pEvent) override {
        QWidget::showEvent(pEvent);
        updateMoving();
    }
    void hideEvent(QHideEvent* pEvent) override {
        QWidget::hideEvent(pEvent);
        m_tick.stop();
    }

  private:
    static constexpr int kWaitMs = 2000;    // still at the start, to read it
    static constexpr double kPxPerHeight = 2.2; // speed: the line's height, 2.2 times a second

    QFont lineFont() const {
        // The skin's row is scaled with the booth: the type with it.
        const int px = std::max(8, static_cast<int>(std::lround(height() * 0.6)));
        QFont f = theme::font(theme::Face::Label, px);
        f.setLetterSpacing(QFont::AbsoluteSpacing, px * 0.03);
        return f;
    }
    static double gap(const QFont& f) {
        return QFontMetrics(f).horizontalAdvance(QStringLiteral("M")) * 4.0;
    }
    /// The part that names the track (before " · 128 BPM"...): what makes
    /// it a different line.
    static QString trackOf(const QString& text) {
        const int bpm = text.indexOf(QRegularExpression(QStringLiteral(" \\x{00B7} \\d+ bpm"),
                QRegularExpression::CaseInsensitiveOption));
        QString track = bpm >= 0 ? text.left(bpm) : text;
        track.remove(QRegularExpression(QStringLiteral("(now|cued): "), QRegularExpression::CaseInsensitiveOption));
        return track;
    }
    void restart() {
        m_offset = 0;
        m_clock.invalidate();
    }
    bool overflowing() const {
        return QFontMetrics(lineFont()).horizontalAdvance(m_text.toUpper()) > width();
    }
    void updateMoving() {
        if (isVisible() && overflowing()) {
            if (!m_tick.isActive()) {
                m_clock.invalidate();
                m_tick.start();
            }
        } else {
            m_tick.stop();
            if (m_offset != 0) {
                m_offset = 0;
                update();
            }
        }
    }
    void step() {
        // Time since the last step, so it moves evenly however busy the
        // app is.
        if (!m_clock.isValid()) {
            m_clock.start();
            m_waitedMs = m_offset == 0 ? 0 : kWaitMs;
            return;
        }
        const qint64 ms = m_clock.restart();
        if (m_offset == 0 && m_waitedMs < kWaitMs) {
            m_waitedMs += ms;
            return;
        }
        const QFont f = lineFont();
        const double cycle = QFontMetrics(f).horizontalAdvance(m_text.toUpper()) + gap(f);
        m_offset += height() * kPxPerHeight * static_cast<double>(ms) / 1000.0;
        if (m_offset >= cycle) {
            // Round to the start again: wait there.
            m_offset = 0;
            m_waitedMs = 0;
        }
        update();
    }

    QString m_text;
    QTimer m_tick;
    QElapsedTimer m_clock;
    qint64 m_waitedMs = 0;
    double m_offset = 0; // how far along, in pixels
};

namespace {
constexpr int kDecks = 4;
const QString kDot = QStringLiteral(" · ");
} // namespace

NowPlayingUi::NowPlayingUi(PlayerManager* pPlayerManager, QObject* pParent)
        : QObject(pParent),
          m_pPlayerManager(pPlayerManager) {
    m_timer.setInterval(300);
    connect(&m_timer, &QTimer::timeout, this, &NowPlayingUi::refresh);
}

void NowPlayingUi::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    // (Started here, on the window's thread, once there is one.)
    m_timer.start();
    refresh();
}

void NowPlayingUi::setManager(Manager* pManager) {
    m_pManager = pManager;
    refresh();
}

void NowPlayingUi::setSides(const QString& mySide, const QString& myName, const QString& otherName) {
    m_mySide = mySide;
    m_myName = myName;
    m_otherName = otherName;
}

QString NowPlayingUi::line(int deck) const {
    return deck >= 0 && deck < kDecks && m_lines[deck] ? m_lines[deck]->text() : QString();
}

QString NowPlayingUi::whoOf(int deck) const {
    // In a session, whoever plays it now; in your own room, you;
    // otherwise nobody's in particular.
    if (m_pManager && m_pManager->peerConnected()) {
        return m_pManager->deckIsMine(deck) ? m_myName
                                            : (m_otherName.isEmpty() ? m_pManager->friendName() : m_otherName);
    }
    return m_mySide.isEmpty() ? QString() : m_myName;
}

QString NowPlayingUi::sideOf(int deck) const {
    if (m_mySide.isEmpty()) {
        return QString();
    }
    const bool mine = !(m_pManager && m_pManager->peerConnected()) || m_pManager->deckIsMine(deck);
    return mine ? m_mySide : (m_mySide == QLatin1String("A") ? QStringLiteral("B") : QStringLiteral("A"));
}

QString NowPlayingUi::text(int deck) const {
    // The deck by its number and who has it: no side letter here (a
    // "side a" next to "deck 3" read as two names for one deck).
    QStringList parts;
    const QString who = whoOf(deck);
    parts << tr("deck %1").arg(deck + 1);
    if (!who.isEmpty()) {
        parts << who;
    }
    const QString group = PlayerManager::groupForDeck(deck);
    Deck* pDeck = m_pPlayerManager ? m_pPlayerManager->getDeck(deck) : nullptr;
    const TrackPointer pTrack = pDeck ? pDeck->getLoadedTrack() : TrackPointer();
    if (!pTrack) {
        parts << tr("empty");
        return parts.join(kDot);
    }
    QString title = pTrack->getTitle().trimmed();
    if (title.isEmpty()) {
        title = QFileInfo(pTrack->getLocation()).completeBaseName();
    }
    const QString artist = pTrack->getArtist().trimmed();
    const bool playing = ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0;
    parts << (playing ? tr("now: %1") : tr("cued: %1"))
                     .arg(artist.isEmpty() ? title : title + QStringLiteral(" — ") + artist);
    const double bpm = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
    if (bpm > 0) {
        parts << tr("%1 bpm").arg(std::lround(bpm));
    }
    const auto key = KeyUtils::keyFromNumericValue(ControlObject::get(ConfigKey(group, QStringLiteral("key"))));
    if (key != mixxx::track::io::key::INVALID) {
        parts << KeyUtils::keyToString(key);
    }
    return parts.join(kDot);
}

void NowPlayingUi::refresh() {
    if (!m_pWindow) {
        return;
    }
    for (int deck = 0; deck < kDecks; ++deck) {
        // The skin's title row above the player (made again with the skin).
        auto* pHead = m_pWindow->findChild<QWidget*>(QStringLiteral("DeckHead%1").arg(deck + 1));
        if (!pHead) {
            m_lines[deck].clear();
            continue;
        }
        if (!m_lines[deck] || m_lines[deck]->parentWidget() != pHead) {
            auto* pLine = new NowLine(pHead);
            // The title's name: clicking it turns the player round, as
            // clicking the title did (DeckFlip).
            pLine->setObjectName(QStringLiteral("DeckTitle%1").arg(deck + 1));
            if (auto* pLayout = qobject_cast<QBoxLayout*>(pHead->layout())) {
                pLayout->insertWidget(0, pLine, 1);
            }
            m_lines[deck] = pLine;
        }
        // In place of its PLAYER N title.
        if (auto* pTitle = pHead->findChild<WLabel*>(QStringLiteral("DeckTitle%1").arg(deck + 1))) {
            pTitle->hide();
        }
        m_lines[deck]->setText(text(deck));
        m_lines[deck]->show();
    }
}

} // namespace mixxx::b2b
