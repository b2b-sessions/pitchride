#include "b2b/b2bdeckflip.h"

#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

#include "control/controlobject.h"
#include "moc_b2bdeckflip.cpp"
#include "widget/wglwidget.h"

namespace mixxx::b2b {

namespace {
constexpr int kTurnMs = 420;     // the whole turn
constexpr int kCaptureMs = 45;   // OpenGL views read back (a couple of frames)
const QColor kBehind(10, 11, 14); // the booth's canvas, behind the card
} // namespace

DeckFlip::DeckFlip(QMainWindow* pWindow)
        : QWidget(pWindow),
          m_pWindow(pWindow) {
    // An ordinary widget over the booth. (Not a window of its own: on
    // Wayland that came up as a second, empty window over the app.) The
    // OpenGL views, which would show through it, are hidden while it turns.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
    m_frame.setInterval(16);
    connect(&m_frame, &QTimer::timeout, this, [this] {
        if (m_clock.elapsed() >= kTurnMs) {
            finish();
            return;
        }
        update();
    });
    m_titles.setInterval(1000);
    connect(&m_titles, &QTimer::timeout, this, &DeckFlip::watchTitles);
    m_titles.start();
}

void DeckFlip::watchTitles() {
    if (!m_pWindow) {
        return;
    }
    m_hasSides = m_pWindow->findChild<QWidget*>(QStringLiteral("Side3")) != nullptr;
    for (int n = 1; n <= 4; ++n) {
        for (QWidget* pTitle : m_pWindow->findChildren<QWidget*>(QStringLiteral("DeckTitle%1").arg(n))) {
            if (!pTitle->property("b2bFlipWatched").toBool()) {
                pTitle->setProperty("b2bFlipWatched", true);
                pTitle->installEventFilter(this);
            }
        }
    }
}

bool DeckFlip::eventFilter(QObject* pObject, QEvent* pEvent) {
    // Only the two events we act on: a title also gets events while the
    // skin is being torn down (a reload), when nothing may be looked up.
    if (pEvent->type() != QEvent::Enter && pEvent->type() != QEvent::MouseButtonPress) {
        return false;
    }
    auto* pTitle = qobject_cast<QWidget*>(pObject);
    if (!pTitle || !m_pWindow) {
        return false;
    }
    // Only where the players turn round (the two-deck skin has the sides;
    // watchTitles() looks, between events).
    const int n = pTitle->objectName().right(1).toInt();
    const bool sides = m_hasSides;
    if (pEvent->type() == QEvent::Enter && sides) {
        pTitle->setCursor(Qt::PointingHandCursor);
        pTitle->setToolTip(tr("Turn this player round to deck %1").arg(n <= 2 ? n + 2 : n - 2));
    } else if (pEvent->type() == QEvent::MouseButtonPress && sides &&
            static_cast<QMouseEvent*>(pEvent)->button() == Qt::LeftButton && n >= 1 && n <= 4) {
        const int other = n <= 2 ? n + 2 : n - 2;
        const ConfigKey key(QStringLiteral("[B2BBooth]"), QStringLiteral("deck%1_select").arg(other));
        ControlObject::set(key, 1.0);
        ControlObject::set(key, 0.0);
        return true;
    }
    return false;
}

QWidget* DeckFlip::shownSide(int side) const {
    if (!m_pWindow) {
        return nullptr;
    }
    for (int n : {side + 1, side + 3}) {
        const QString name = QStringLiteral("Side%1").arg(n);
        for (QWidget* pWidget : m_pWindow->findChildren<QWidget*>(name)) {
            if (pWidget->isVisibleTo(m_pWindow)) {
                return pWidget;
            }
        }
    }
    return nullptr;
}

QImage DeckFlip::picture(const QRect& rect) const {
    QWidget* pCentral = m_pWindow ? m_pWindow->centralWidget() : nullptr;
    if (!pCentral) {
        return QImage();
    }
    // The widgets (not this one: it's not in the central widget)...
    const QRect inCentral(pCentral->mapFrom(m_pWindow, rect.topLeft()), rect.size());
    QImage image = pCentral->grab(inCentral).toImage();
    // ...and the OpenGL views, which a grab leaves blank.
    QPainter p(&image);
    for (WGLWidget* pGl : WGLWidget::allWidgets()) {
        if (!pGl->isVisibleTo(m_pWindow)) {
            continue;
        }
        const QRect at(pGl->mapTo(m_pWindow, QPoint(0, 0)) - rect.topLeft(), pGl->size());
        if (!at.intersects(QRect(QPoint(0, 0), rect.size()))) {
            continue;
        }
        if (pGl->capturedFrame().isNull()) {
            p.fillRect(at, kBehind); // not drawn yet: dark, not a white box
        } else {
            p.drawImage(at, pGl->capturedFrame());
        }
    }
    return image;
}

void DeckFlip::hideGlViews(const QRect& rect, bool hide) {
    if (!hide) {
        for (const QPointer<QWidget>& pView : std::as_const(m_hidden)) {
            if (pView) {
                pView->show();
            }
        }
        m_hidden.clear();
        return;
    }
    for (QWidget* pWidget : m_pWindow->findChildren<QWidget*>()) {
        if (pWidget->isVisible() && !pWidget->isWindow() &&
                QLatin1String(pWidget->metaObject()->className()) == QLatin1String("QWindowContainer") &&
                QRect(pWidget->mapTo(m_pWindow, QPoint(0, 0)), pWidget->size()).intersects(rect)) {
            pWidget->hide();
            m_hidden.append(pWidget);
        }
    }
}

void DeckFlip::flip(int side, std::function<void()> apply) {
    QWidget* pFrom = m_busy ? nullptr : shownSide(side);
    if (!pFrom || !m_pWindow) {
        apply(); // a skin without sides, or mid-turn: at once
        return;
    }
    m_busy = true;
    m_rect = QRect(pFrom->mapTo(m_pWindow, QPoint(0, 0)), pFrom->size());
    WGLWidget::requestCapture(true);
    // 1. The side going, as it looks now (once the OpenGL views have been
    //    read back), and held over it while it switches.
    QTimer::singleShot(kCaptureMs, this, [this, side, apply] {
        m_from = picture(m_rect);
        m_to = QImage();
        hideGlViews(m_rect, true);
        setGeometry(m_rect);
        raise();
        show();
        update();
        apply();
        hideGlViews(m_rect, true); // the coming side's views too
        // 2. The side coming, once it's drawn (under the picture).
        QTimer::singleShot(kCaptureMs, this, [this, side] {
            QWidget* pTo = shownSide(side);
            m_to = pTo ? picture(m_rect) : m_from;
            WGLWidget::requestCapture(false);
            raise();
            m_clock.start();
            m_frame.start();
            update();
        });
    });
}

void DeckFlip::finish() {
    m_frame.stop();
    hide();
    hideGlViews(m_rect, false);
    m_from = QImage();
    m_to = QImage();
    m_busy = false;
}

void DeckFlip::paintEvent(QPaintEvent* /*pEvent*/) {
    QPainter p(this);
    if (m_to.isNull() || !m_clock.isValid() || !m_frame.isActive()) {
        // Waiting for the side coming: the side going, still.
        if (!m_from.isNull()) {
            p.drawImage(rect(), m_from);
        }
        return;
    }
    p.fillRect(rect(), kBehind);
    // Eased: quick in the middle, settling at the end.
    const double t = std::clamp(m_clock.elapsed() / static_cast<double>(kTurnMs), 0.0, 1.0);
    const double eased = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
    const double angle = 180.0 * eased;
    const bool first = angle < 90.0;
    const QImage& face = first ? m_from : m_to;
    const double shown = first ? angle : angle - 180.0;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF centre(width() / 2.0, height() / 2.0);
    QTransform turn;
    turn.translate(centre.x(), centre.y());
    turn.rotate(shown, Qt::YAxis);
    turn.translate(-centre.x(), -centre.y());
    p.setTransform(turn);
    p.drawImage(rect(), face);
    // Darker as it turns edge-on.
    const int shade = static_cast<int>(150 * std::sin(qDegreesToRadians(std::fabs(shown))));
    p.fillRect(rect(), QColor(0, 0, 0, shade));
}

} // namespace mixxx::b2b
