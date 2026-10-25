#include "b2b/b2bloopreadout.h"

#include <QPainter>
#include <QWidget>
#include <cmath>

#include "b2b/b2btheme.h"
#include "control/controlobject.h"
#include "moc_b2bloopreadout.cpp"

namespace mixxx::b2b {

namespace {
const QColor kFrame(58, 63, 74);      // as the KEY and BPM boxes' outlines
const QColor kFrameLabel(138, 143, 153);
const QColor kLoopLit(255, 176, 32);  // the waveform's loop colour
} // namespace

/// The badge: a box like KEY and BPM beside it, LOOP and the length in the
/// loop's orange.
class LoopBadge : public QWidget {
  public:
    explicit LoopBadge(QWidget* pParent)
            : QWidget(pParent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }
    void setText(const QString& text) {
        if (text != m_text) {
            m_text = text;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(5, 6, 10));
        p.setPen(kFrame);
        p.drawRect(QRect(0, 0, width() - 1, height() - 1));
        // LOOP small at the top left (as KEY, BPM), the length below.
        const int labelPx = std::max(6, static_cast<int>(std::lround(height() * 0.26)));
        const int valuePx = std::max(9, static_cast<int>(std::lround(height() * 0.56)));
        const int pad = std::max(2, height() / 9);
        QFont label = theme::font(theme::Face::Label, labelPx);
        p.setFont(label);
        p.setPen(kFrameLabel);
        p.drawText(QRect(pad + 1, pad - 1, width(), labelPx + 2), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("LOOP"));
        QFont value = theme::font(theme::Face::Readout, valuePx);
        p.setFont(value);
        p.setPen(kLoopLit);
        p.drawText(QRect(pad, 0, width() - 2 * pad, height() - 1), Qt::AlignRight | Qt::AlignBottom, m_text);
    }

  private:
    QString m_text;
};

LoopReadoutUi::LoopReadoutUi(QObject* pParent)
        : QObject(pParent) {
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &LoopReadoutUi::refresh);
}

void LoopReadoutUi::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    m_timer.start();
    refresh();
}

QString LoopReadoutUi::lengthText(double beats) {
    if (beats <= 0) {
        return QString();
    }
    if (beats < 1.0) {
        const double parts = 1.0 / beats;
        const double whole = std::round(parts);
        if (std::fabs(parts - whole) < whole * 0.02) {
            return QStringLiteral("1/%1").arg(static_cast<int>(whole));
        }
        return QString::number(beats, 'f', 2);
    }
    const double whole = std::round(beats);
    if (std::fabs(beats - whole) < 0.02) {
        return QString::number(static_cast<int>(whole));
    }
    return QString::number(beats, 'f', 1);
}

void LoopReadoutUi::refresh() {
    if (!m_pWindow) {
        return;
    }
    for (int deck = 0; deck < static_cast<int>(m_badges.size()); ++deck) {
        // The screen's top row: the title and artist, then KEY and BPM.
        // The badge goes left of KEY, as tall as the title and artist; the
        // two are cut shorter while it shows.
        const QString n = QString::number(deck + 1);
        QWidget* pKey = m_pWindow->findChild<QWidget*>(QStringLiteral("Key") + n);
        QWidget* pTitle = m_pWindow->findChild<QWidget*>(QStringLiteral("Title") + n);
        QWidget* pArtist = m_pWindow->findChild<QWidget*>(QStringLiteral("Artist") + n);
        if (!pKey || !pTitle || pTitle->parentWidget() != pKey->parentWidget()) {
            m_badges[deck].clear();
            continue;
        }
        if (!m_badges[deck] || m_badges[deck]->parentWidget() != pKey->parentWidget()) {
            m_badges[deck] = new LoopBadge(pKey->parentWidget());
            m_widths[deck] = {pTitle->width(), pArtist ? pArtist->width() : 0};
        }
        LoopBadge* pBadge = m_badges[deck];
        const ConfigKey key(QStringLiteral("[B2BBooth]"), QStringLiteral("deck%1_loop_beats").arg(deck + 1));
        const double beats = ControlObject::exists(key) ? ControlObject::get(key) : 0.0;
        const bool shown = beats != 0 && pKey->isVisible();
        // The skin's own widths (it is drawn to scale: kept as it changes).
        if (!pBadge->isVisible()) {
            m_widths[deck] = {pTitle->width(), pArtist ? pArtist->width() : 0};
        }
        if (!shown) {
            if (pBadge->isVisible()) {
                pBadge->hide();
                pTitle->resize(m_widths[deck].first, pTitle->height());
                if (pArtist) {
                    pArtist->resize(m_widths[deck].second, pArtist->height());
                }
            }
            continue;
        }
        // (No beat grid to count it: LOOP, with no number.)
        pBadge->setText(lengthText(beats));
        const QRect keyAt = pKey->geometry();
        const int gap = std::max(4, keyAt.height() / 2);
        const int w = qRound(keyAt.width() * 1.2);
        const int top = pTitle->geometry().top() + 1;
        const int bottom = pArtist ? pArtist->geometry().bottom() : keyAt.bottom();
        const QRect at(keyAt.left() - gap - w, top, w, std::max(keyAt.height(), bottom - top + 1));
        if (pBadge->geometry() != at) {
            pBadge->setGeometry(at);
        }
        const int room = std::max(10, at.left() - gap / 2 - pTitle->x());
        // Wider than we cut it: the skin drew it again (a resize): its own.
        if (pTitle->width() > room) {
            m_widths[deck].first = pTitle->width();
        }
        if (pArtist && pArtist->width() > room) {
            m_widths[deck].second = pArtist->width();
        }
        pTitle->resize(std::min(m_widths[deck].first, room), pTitle->height());
        if (pArtist) {
            pArtist->resize(std::min(m_widths[deck].second, room), pArtist->height());
        }
        pBadge->show();
        pBadge->raise();
    }
}

} // namespace mixxx::b2b
