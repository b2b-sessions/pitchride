#include "b2b/b2bflyerwall.h"

#include <QDir>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QRandomGenerator>

#include "b2b/b2btheme.h"

namespace mixxx::b2b {

namespace {
constexpr int kPad = 18; // around the row, and between posters
constexpr int kGap = 16;
const QColor kDim(5, 8, 31, 64);           // booth-deep over them: the decks first
const QColor kShadeBottom(5, 8, 31, 210);  // where the panels' bar meets them
} // namespace

FlyerWall::FlyerWall(QWidget* pParent)
        : QWidget(pParent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // The campaign's posters (res/b2b/posters, made from the website's).
    const QDir dir(QStringLiteral(":/b2b/posters"));
    QStringList names = dir.entryList({QStringLiteral("*.jpg")}, QDir::Files);
    // A different wall each time.
    for (int i = names.size() - 1; i > 0; --i) {
        names.swapItemsAt(i, static_cast<int>(QRandomGenerator::global()->bounded(i + 1)));
    }
    for (const QString& name : std::as_const(names)) {
        QImage image(dir.filePath(name));
        if (!image.isNull()) {
            m_posters.append({image, QImage()});
        }
    }
    hide();
}

void FlyerWall::setPosterHeight(int px) {
    if (px != m_posterHeight) {
        m_posterHeight = px;
        update();
    }
}

void FlyerWall::paintEvent(QPaintEvent* pEvent) {
    QPainter p(this);
    // The blue under the booth, as the panels paint it (one continuous
    // world), then the row of posters on it.
    QLinearGradient ground(0, 0, 0, std::max(1, m_posterHeight + 2 * kPad));
    ground.setColorAt(0.0, QColor(theme::kBoothDeep));
    ground.setColorAt(1.0, QColor(QStringLiteral("#0B1452")));
    p.fillRect(pEvent->rect(), ground);

    const int h = m_posterHeight;
    if (h > 40 && !m_posters.isEmpty()) {
        const qreal dpr = devicePixelRatioF();
        const int w = qRound(h * 3.0 / 4.0); // the posters are 3:4
        if (dpr != m_scaledDpr || m_posters.first().scaled.height() != qRound(h * dpr)) {
            for (Poster& poster : m_posters) {
                poster.scaled = poster.source.scaled(qRound(w * dpr), qRound(h * dpr),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                poster.scaled.setDevicePixelRatio(dpr);
            }
            m_scaledDpr = dpr;
        }
        // As many as fit, the row centred; more than we have: round again.
        const int fit = std::max(1, (width() - 2 * kPad + kGap) / (w + kGap));
        const int row = fit * w + (fit - 1) * kGap;
        int x = (width() - row) / 2;
        for (int i = 0; i < fit; ++i) {
            p.drawImage(QPoint(x, kPad), m_posters[i % m_posters.size()].scaled);
            x += w + kGap;
        }
        p.fillRect(QRect(0, kPad, width(), h), kDim);
    }
    // A soft shade where the panels' bar covers them.
    QLinearGradient shade(0, height() - 26, 0, height());
    shade.setColorAt(0.0, QColor(5, 8, 31, 0));
    shade.setColorAt(1.0, kShadeBottom);
    p.fillRect(QRect(0, height() - 26, width(), 26), shade);
}

} // namespace mixxx::b2b
