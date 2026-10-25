#include "b2b/b2bpeerbrowser.h"

#include <QPainter>
#include <QRegularExpression>
#include <QWidget>
#include <algorithm>

#include "b2b/b2bexplorer.h"
#include "b2b/b2bmanager.h"
#include "control/controlobject.h"
#include "moc_b2bpeerbrowser.cpp"

namespace mixxx::b2b {

namespace {

constexpr int kTickMs = 300;       // how often our browser is looked at
constexpr int kKeepAliveMs = 3000; // resend an unchanged screen now and then
constexpr int kStaleMs = 8000;     // nothing from the friend for this long: hide it
constexpr int kMaxBytes = 62;      // a session message's text (64 bytes)

// The music browser's look (see b2bexplorer.cpp).
const QColor kBg(10, 14, 23);
const QColor kLine(26, 35, 54);
const QColor kAccent(47, 107, 255);
const QColor kText(245, 247, 251);
const QColor kGrey(139, 149, 171);
const QColor kKey(98, 214, 132);
const QColor kTag(255, 138, 31);

/// At most `bytes` of UTF-8, cut at a character.
QString fit(QString text, int bytes) {
    while (text.toUtf8().size() > bytes) {
        text.chop(1);
    }
    return text;
}

ConfigKey peerKey(int deck) {
    return ConfigKey(QStringLiteral("[B2BBooth]"), QStringLiteral("deck%1_peer_browsing").arg(deck));
}

} // namespace

/// The friend's browser, drawn on a player's screen. Clicks do nothing.
class PeerScreen : public QWidget {
  public:
    explicit PeerScreen(QWidget* pParent)
            : QWidget(pParent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setToolTip(tr("Your friend's music browser (to look at)"));
    }
    void setView(int deck, bool tracks, int current, int total, const QString& heading, const QString& crumb,
            const QStringList& rows) {
        m_deck = deck;
        m_tracks = tracks;
        m_current = current;
        m_total = total;
        m_heading = heading;
        m_crumb = crumb;
        m_rows = rows;
        update();
    }

  protected:
    void mousePressEvent(QMouseEvent*) override {
    }
    void wheelEvent(QWheelEvent*) override {
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), kBg);
        const int w = width();
        const int h = height();
        const int px = std::clamp(h / 19, 8, 14);
        const int pad = std::max(4, px / 2);

        // Header: whose screen, the heading, where it is.
        QFont tagFont = font();
        tagFont.setPixelSize(std::max(7, px - 4));
        tagFont.setBold(true);
        p.setFont(tagFont);
        const QString tag = tr("FRIEND'S SCREEN");
        const int tagW = QFontMetrics(tagFont).horizontalAdvance(tag) + 10;
        const QRect tagRect(w - pad - tagW, pad, tagW, px + 2);
        p.setPen(Qt::NoPen);
        p.setBrush(kTag);
        p.drawRoundedRect(tagRect, 3, 3);
        p.setPen(QColor(23, 11, 0));
        p.drawText(tagRect, Qt::AlignCenter, tag);

        QFont headFont = font();
        headFont.setPixelSize(px + 4);
        headFont.setBold(true);
        p.setFont(headFont);
        p.setPen(kText);
        const int headH = px + 8;
        p.drawText(QRect(pad, pad - 2, w - 3 * pad - tagW, headH), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(headFont).elidedText(m_heading, Qt::ElideRight, w - 3 * pad - tagW));
        QFont smallFont = font();
        smallFont.setPixelSize(std::max(7, px - 3));
        smallFont.setBold(true);
        p.setFont(smallFont);
        p.setPen(kGrey);
        const int crumbTop = pad + headH - 2;
        QString crumb = m_crumb;
        if (m_tracks && m_total > 0) {
            crumb += QStringLiteral("   [%1]").arg(m_total, 4, 10, QLatin1Char('0'));
        }
        p.drawText(QRect(pad, crumbTop, w - 2 * pad, px), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(smallFont).elidedText(crumb, Qt::ElideRight, w - 2 * pad));
        const int top = crumbTop + px + pad;
        p.setPen(kLine);
        p.drawLine(0, top - 1, w, top - 1);

        // The rows, the cursor in blue.
        const int rowH = std::max(px + 6, (h - top) / 10);
        QFont rowFont = font();
        rowFont.setPixelSize(px);
        rowFont.setBold(true);
        const QFontMetrics fm(rowFont);
        if (m_rows.isEmpty()) {
            p.setFont(rowFont);
            p.setPen(kGrey);
            p.drawText(QRect(0, top, w, h - top), Qt::AlignCenter,
                    m_tracks ? tr("No tracks here yet") : tr("Nothing here yet"));
        }
        for (int i = 0; i < m_rows.size(); ++i) {
            const QRect r(0, top + i * rowH, w, rowH);
            if (r.top() >= h) {
                break;
            }
            const bool selected = i == m_current;
            p.fillRect(r, selected ? kAccent : kBg);
            p.setPen(kLine);
            p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
            const QStringList cols = m_rows[i].split(QLatin1Char('\t'));
            p.setFont(rowFont);
            if (m_tracks && cols.size() >= 4) {
                // 001   Artist - Title          124.0   8A
                const int numW = fm.horizontalAdvance(QStringLiteral("0000"));
                const int keyW = fm.horizontalAdvance(QStringLiteral("00A0"));
                const int bpmW = fm.horizontalAdvance(QStringLiteral("000.00"));
                p.setPen(selected ? kText : kGrey);
                p.drawText(QRect(pad, r.top(), numW, rowH), Qt::AlignLeft | Qt::AlignVCenter, cols[0]);
                const int titleX = pad + numW + pad;
                const int titleW = w - titleX - bpmW - keyW - 2 * pad;
                p.setPen(kText);
                p.drawText(QRect(titleX, r.top(), titleW, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                        fm.elidedText(cols.mid(3).join(QLatin1Char('\t')), Qt::ElideRight, titleW));
                p.drawText(QRect(w - pad - keyW - bpmW, r.top(), bpmW, rowH), Qt::AlignRight | Qt::AlignVCenter,
                        cols[1]);
                p.setPen(selected ? kText : kKey);
                p.drawText(QRect(w - pad - keyW, r.top(), keyW, rowH), Qt::AlignRight | Qt::AlignVCenter, cols[2]);
            } else {
                // Playlist                3  >
                const QString label = cols.value(0);
                const QString detail = cols.value(1);
                const int detailW = detail.isEmpty() ? 0 : fm.horizontalAdvance(detail) + pad;
                p.setPen(kText);
                p.drawText(QRect(pad * 2, r.top(), w - 4 * pad - detailW - px, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                        fm.elidedText(label, Qt::ElideRight, w - 4 * pad - detailW - px));
                p.setPen(selected ? kText : kGrey);
                p.drawText(QRect(w - 2 * pad - px - detailW, r.top(), detailW, rowH),
                        Qt::AlignRight | Qt::AlignVCenter, detail);
                p.drawText(QRect(w - pad - px, r.top(), px, rowH), Qt::AlignCenter, QStringLiteral("›"));
            }
        }
    }

  private:
    int m_deck = 0;
    bool m_tracks = false;
    int m_current = -1;
    int m_total = 0;
    QString m_heading;
    QString m_crumb;
    QStringList m_rows;
};

PeerBrowser::PeerBrowser(QObject* pParent)
        : QObject(pParent) {
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &PeerBrowser::tick);
    m_timer.start();
}

PeerBrowser::~PeerBrowser() {
    hide();
}

void PeerBrowser::setWindow(QWidget* pWindow) {
    // A new skin: the old screen went with the old widgets.
    hide();
    m_pWindow = pWindow;
}

void PeerBrowser::setManager(Manager* pManager) {
    if (m_pManager) {
        disconnect(m_pManager, nullptr, this, nullptr);
    }
    m_pManager = pManager;
    m_lastSent.clear();
    m_sentOpen = false;
    hide();
    if (pManager) {
        connect(pManager, &Manager::browserViewReceived, this, &PeerBrowser::received);
    }
}

void PeerBrowser::tick() {
    // The friend's screen: gone quiet (they left, or the line dropped).
    if (m_pScreen && m_sinceReceived.isValid() && m_sinceReceived.elapsed() > kStaleMs) {
        hide();
    }
    // We opened our own browser on that player: ours wins.
    if (m_pScreen && ControlObject::get(ConfigKey(QStringLiteral("[B2BBooth]"),
                             QStringLiteral("deck%1_browsing").arg(m_shownDeck))) > 0) {
        hide();
    }
    if (!m_pManager || !m_pManager->peerConnected()) {
        return;
    }
    LibraryExplorer::Snapshot snap;
    if (!LibraryExplorer::snapshot(&snap)) {
        if (m_sentOpen) {
            m_pManager->sendBrowserView(6, QString::number(m_sentDeck));
            m_sentOpen = false;
            m_lastSent.clear();
        }
        return;
    }
    QStringList messages;
    messages << QStringLiteral("%1\t%2\t%3\t%4\t%5")
                        .arg(snap.deck)
                        .arg(snap.tracks ? 1 : 0)
                        .arg(snap.current)
                        .arg(snap.total)
                        .arg(snap.rows.size());
    messages << fit(snap.heading, kMaxBytes) << fit(snap.crumb, kMaxBytes);
    for (int i = 0; i < snap.rows.size(); ++i) {
        messages << fit(QString::number(i) + QLatin1Char('\t') + snap.rows[i], kMaxBytes);
    }
    const bool changed = messages != m_lastSent;
    if (!changed && m_sinceSent.isValid() && m_sinceSent.elapsed() < kKeepAliveMs) {
        return;
    }
    if (m_sentOpen && m_sentDeck != snap.deck) {
        m_pManager->sendBrowserView(6, QString::number(m_sentDeck));
    }
    for (int i = 0; i < messages.size(); ++i) {
        m_pManager->sendBrowserView(i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 3 : 4, messages[i]);
    }
    m_pManager->sendBrowserView(5, QString());
    m_lastSent = messages;
    m_sinceSent.start();
    m_sentOpen = true;
    m_sentDeck = snap.deck;
}

void PeerBrowser::received(double kind, const QString& text) {
    m_sinceReceived.start();
    switch (static_cast<int>(kind)) {
    case 1: {
        const QStringList f = text.split(QLatin1Char('\t'));
        m_incoming = View();
        m_incoming.deck = f.value(0).toInt();
        m_incoming.tracks = f.value(1).toInt() != 0;
        m_incoming.current = f.value(2).toInt();
        m_incoming.total = f.value(3).toInt();
        break;
    }
    case 2:
        m_incoming.heading = text;
        break;
    case 3:
        m_incoming.crumb = text;
        break;
    case 4:
        m_incoming.rows.append(text.section(QLatin1Char('\t'), 1));
        break;
    case 5:
        show(m_incoming);
        break;
    case 6:
        if (text.toInt() == m_shownDeck) {
            hide();
        }
        break;
    default:
        break;
    }
}

void PeerBrowser::show(const View& view) {
    if (!m_pWindow || view.deck < 1 || view.deck > 4) {
        return;
    }
    // Our own browser on that player wins: we're using it.
    if (ControlObject::get(ConfigKey(QStringLiteral("[B2BBooth]"), QStringLiteral("deck%1_browsing").arg(view.deck))) > 0) {
        hide();
        return;
    }
    if (m_pScreen && m_shownDeck != view.deck) {
        hide();
    }
    if (!m_pScreen) {
        // Where the player's screen is: the skin kit's StandbyN slot covers
        // it; B2B Booth's PlayerN has its "Screen".
        QWidget* pArea = m_pWindow->findChild<QWidget*>(QStringLiteral("Standby%1").arg(view.deck));
        if (!pArea) {
            if (QWidget* pPlayer = m_pWindow->findChild<QWidget*>(QStringLiteral("Player%1").arg(view.deck))) {
                pArea = pPlayer->findChild<QWidget*>(QStringLiteral("Screen"));
            }
        }
        if (!pArea || !pArea->parentWidget()) {
            return;
        }
        // That deck isn't on our screen (two decks, and that player turned
        // round to the other one): nothing to show, and nothing to hide. The
        // other deck's waveform sits in the very same place. (Their browser
        // comes again in a moment: turned back, it shows then.)
        QWidget* pPlayer = pArea->parentWidget();
        if (!pPlayer->isVisible()) {
            return;
        }
        m_pScreen = new PeerScreen(pPlayer);
        m_pScreen->setGeometry(pArea->geometry());
        // Waveforms are drawn on top of anything over them: hide that deck's
        // in the way (the skin kit's screen hides itself, see below).
        const QRect area(pArea->mapTo(m_pWindow, QPoint(0, 0)), pArea->size());
        const auto all = pPlayer->findChildren<QWidget*>();
        for (QWidget* pWidget : all) {
            if (pWidget->inherits("WWaveformViewer") && pWidget->isVisible() &&
                    QRect(pWidget->mapTo(m_pWindow, QPoint(0, 0)), pWidget->size()).intersects(area)) {
                pWidget->hide();
                m_hiddenWaveforms.append(pWidget);
            }
        }
        m_pScreen->show();
        m_pScreen->raise();
        m_shownDeck = view.deck;
        ControlObject::set(peerKey(view.deck), 1.0);
    }
    m_pScreen->setView(view.deck, view.tracks, view.current, view.total, view.heading, view.crumb, view.rows);
}

void PeerBrowser::hide() {
    if (m_shownDeck > 0) {
        ControlObject::set(peerKey(m_shownDeck), 0.0);
    }
    for (const QPointer<QWidget>& pWidget : std::as_const(m_hiddenWaveforms)) {
        if (pWidget) {
            pWidget->show();
        }
    }
    m_hiddenWaveforms.clear();
    if (m_pScreen) {
        m_pScreen->deleteLater();
    }
    m_pScreen = nullptr;
    m_shownDeck = 0;
}

} // namespace mixxx::b2b
