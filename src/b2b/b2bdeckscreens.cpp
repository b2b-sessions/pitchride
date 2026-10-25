#include "b2b/b2bdeckscreens.h"

#include <QDebug>
#include <QFontDatabase>
#include <QFontInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <cmath>
#include <algorithm>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "moc_b2bdeckscreens.cpp"

namespace mixxx::b2b {

namespace {
const QString kGroup = QStringLiteral("[B2BBooth]");
constexpr int kPowerUpMs = 1100;

QFont monoFont(int px, bool bold = true) {
    // The app's display mono (bundled, res/fonts): Medium is its weight.
    QFont f(QStringList{QStringLiteral("JetBrains Mono Medium"), QStringLiteral("JetBrains Mono"),
            QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Menlo"),
            QStringLiteral("monospace")});
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(std::max(6, px));
    f.setBold(bold && !QFontInfo(f).family().contains(QLatin1String("JetBrains Mono")));
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    return f;
}

const QColor kBlack(0, 0, 0);
const QColor kWhite(244, 247, 252);
const QColor kBlue(31, 107, 255);
const QColor kDimBlue(46, 70, 120);
const QColor kAmber(255, 138, 31);
} // namespace

/// One player's screen with no track: black glass, scanlines, and a blinking
/// PRESS SOURCE. Clicking it opens the music browser. When a track loads it
/// powers up like an old CRT (a line, a flash, READING TRACK) before the
/// deck shows.
class StandbyScreen : public QWidget {
  public:
    StandbyScreen(int deck, QWidget* pParent)
            : QWidget(pParent),
              m_deck(deck) {
        setObjectName(QStringLiteral("StandbyScreen"));
        setAttribute(Qt::WA_OpaquePaintEvent);
        setCursor(Qt::PointingHandCursor);
        setToolTip(tr("Click to choose a track for player %1 (same as SOURCE)").arg(deck + 1));
        m_blink.start();
        m_tick.setInterval(40);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            const ConfigKey powering(kGroup, QStringLiteral("deck%1_powering").arg(m_deck + 1));
            if (!m_powering && ControlObject::get(powering) > 0) {
                m_powering = true; // a track came in: power up
                m_power.start();
            }
            if (m_powering && m_power.elapsed() > kPowerUpMs) {
                m_powering = false;
                ControlObject::set(ConfigKey(kGroup, QStringLiteral("deck%1_powering").arg(m_deck + 1)), 0.0);
            }
            update();
        });
        m_tick.start();
    }

  protected:
    void mousePressEvent(QMouseEvent* pEvent) override {
        if (pEvent->button() == Qt::LeftButton && !m_powering) {
            const ConfigKey key(kGroup, QStringLiteral("deck%1_show_browse").arg(m_deck + 1));
            ControlObject::set(key, 1.0);
            ControlObject::set(key, 0.0);
        }
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = rect();
        p.fillRect(r, kBlack);
        if (m_powering) {
            paintPowerUp(p, r);
        } else {
            paintStandby(p, r);
        }
        // Glass: scanlines and a faint vignette.
        p.setPen(Qt::NoPen);
        for (int y = 0; y < height(); y += 3) {
            p.fillRect(QRectF(0, y, width(), 1), QColor(0, 0, 0, 70));
        }
        QRadialGradient vignette(r.center(), std::max(r.width(), r.height()) * 0.75);
        vignette.setColorAt(0.6, QColor(0, 0, 0, 0));
        vignette.setColorAt(1.0, QColor(0, 0, 0, 150));
        p.fillRect(r, vignette);
    }

  private:
    void paintStandby(QPainter& p, const QRectF& r) {
        const double u = r.height() / 100.0; // layout unit
        // Frame and header strip.
        p.setPen(QPen(kDimBlue, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0.5 + 2 * u, 0.5 + 2 * u, -0.5 - 2 * u, -0.5 - 2 * u));
        p.setFont(monoFont(static_cast<int>(6 * u)));
        p.setPen(kWhite);
        p.drawText(QRectF(5 * u, 4 * u, r.width() * 0.6, 9 * u), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("PLAYER %1").arg(m_deck + 1));
        p.setPen(kAmber);
        p.drawText(QRectF(r.width() * 0.4, 4 * u, r.width() * 0.6 - 5 * u, 9 * u),
                Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("STANDBY"));
        p.fillRect(QRectF(5 * u, 14 * u, r.width() - 10 * u, 1), kBlue);

        // The prompt: blinking, like a waiting machine.
        const bool on = (m_blink.elapsed() / 600) % 2 == 0;
        const QRectF mid(0, r.height() * 0.30, r.width(), r.height() * 0.30);
        p.setFont(monoFont(static_cast<int>(12 * u)));
        const QString prompt = QStringLiteral("PRESS SOURCE");
        p.setPen(on ? kWhite : QColor(70, 78, 96));
        p.drawText(mid, Qt::AlignCenter, (on ? QStringLiteral("▶ ") : QStringLiteral("  ")) + prompt);
        p.setFont(monoFont(static_cast<int>(5.2 * u), false));
        p.setPen(QColor(120, 140, 180));
        p.drawText(QRectF(0, mid.bottom() - 2 * u, r.width(), 9 * u), Qt::AlignCenter,
                tr("OR CLICK HERE TO LOAD A TRACK"));

        // Bottom: an empty meter row and NO TRACK.
        const double y = r.height() - 16 * u;
        const int cells = 24;
        const double cw = (r.width() - 10 * u) / cells;
        for (int i = 0; i < cells; ++i) {
            p.fillRect(QRectF(5 * u + i * cw + 1, y, cw - 2, 4 * u), QColor(14, 22, 40));
        }
        p.setFont(monoFont(static_cast<int>(4.6 * u)));
        p.setPen(kDimBlue);
        p.drawText(QRectF(5 * u, y + 5 * u, r.width() - 10 * u, 7 * u), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("NO TRACK"));
        p.drawText(QRectF(5 * u, y + 5 * u, r.width() - 10 * u, 7 * u), Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("--:--.-"));
    }

    void paintPowerUp(QPainter& p, const QRectF& r) {
        const double t = std::min(1.0, m_power.elapsed() / static_cast<double>(kPowerUpMs));
        const double cy = r.center().y();
        if (t < 0.18) {
            // A line grows from the middle.
            const double k = t / 0.18;
            const double w = r.width() * k;
            p.fillRect(QRectF(r.center().x() - w / 2, cy - 1, w, 2), kWhite);
            p.fillRect(QRectF(r.center().x() - w / 2, cy - 4, w, 8), QColor(31, 107, 255, 90));
        } else if (t < 0.38) {
            // It opens into a flash.
            const double k = (t - 0.18) / 0.20;
            const double h = r.height() * k;
            p.fillRect(QRectF(0, cy - h / 2, r.width(), h),
                    QColor(31, 107, 255, static_cast<int>(200 * (1 - k)) + 30));
            p.fillRect(QRectF(0, cy - 1, r.width(), 2), kWhite);
        } else {
            // Reading the track.
            const double k = (t - 0.38) / 0.62;
            const double u = r.height() / 100.0;
            p.setFont(monoFont(static_cast<int>(8 * u)));
            p.setPen(kWhite);
            p.drawText(QRectF(0, cy - 20 * u, r.width(), 12 * u), Qt::AlignCenter,
                    QStringLiteral("READING TRACK"));
            const double bw = r.width() * 0.6;
            const QRectF bar(r.center().x() - bw / 2, cy, bw, 6 * u);
            p.setPen(QPen(kDimBlue, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(bar);
            const int cells = 20;
            const int lit = static_cast<int>(std::ceil(k * cells));
            const double cw = (bw - 4) / cells;
            for (int i = 0; i < lit; ++i) {
                p.fillRect(QRectF(bar.left() + 2 + i * cw + 1, bar.top() + 2, cw - 2, bar.height() - 4), kBlue);
            }
        }
    }

    int m_deck;
    QTimer m_tick;
    QElapsedTimer m_blink;
    QElapsedTimer m_power;
    bool m_powering = false;
};

DeckScreensUi::DeckScreensUi(QObject* pParent)
        : QObject(pParent) {
    m_timer.setInterval(700);
    connect(&m_timer, &QTimer::timeout, this, &DeckScreensUi::refresh);
    m_timer.start();
}

DeckScreensUi::~DeckScreensUi() = default;

void DeckScreensUi::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    refresh();
}

void DeckScreensUi::refresh() {
    for (int i = 0; i < static_cast<int>(m_decks.size()); ++i) {
        Deck& d = m_decks[i];
        QWidget* pSlot = m_pWindow
                ? m_pWindow->findChild<QWidget*>(QStringLiteral("Standby%1").arg(i + 1))
                : nullptr;
        if (d.screen && d.slot == pSlot && d.screen->geometry() != pSlot->rect()) {
            d.screen->setGeometry(pSlot->rect()); // the skin was scaled
        }
        if (pSlot != d.slot || (pSlot && !d.screen)) {
            d.slot = pSlot;
            if (pSlot) {
                if (qEnvironmentVariableIsSet("B2B_DEBUG_SCREENS")) {
                    qInfo() << "B2B screens: deck" << i + 1 << "slot" << pSlot
                            << pSlot->mapTo(pSlot->window(), QPoint()) << "old screen" << d.screen.data();
                }
                d.screen = new StandbyScreen(i, pSlot);
                d.screen->setGeometry(pSlot->rect());
                d.screen->show();
            }
        }
    }
}

} // namespace mixxx::b2b
