#include "b2b/b2bpointer.h"

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <cmath>
#include <algorithm>

#include "b2b/b2bmanager.h"
#include "moc_b2bpointer.cpp"
#include "skin/legacy/legacyskinparser.h"

namespace mixxx::b2b {

namespace {
constexpr int kTickMs = 33;          // ~30 updates a second while moving
constexpr int kKeepAliveMs = 1000;   // resend a still pointer now and then
constexpr int kForgetMs = 3000;      // nothing heard: hide theirs
constexpr int kMarkW = 96;
constexpr int kMarkH = 44;
constexpr double kQuant = 65535.0;

// x and y (0..1, or hidden), button down: one number.
double pack(bool shown, double x, double y, bool pressed) {
    if (!shown) {
        return -1.0;
    }
    const double qx = std::round(std::clamp(x, 0.0, 1.0) * kQuant);
    const double qy = std::round(std::clamp(y, 0.0, 1.0) * kQuant);
    return (qx * 65536.0 + qy) * 2.0 + (pressed ? 1.0 : 0.0);
}
} // namespace

/// The friend's pointer: an arrow and a FRIEND tag. A small window of its
/// own (so it shows over the GPU-drawn waveforms too); on Wayland, where
/// windows can't place themselves, a see-through layer over the skin.
class PointerMark : public QWidget {
  public:
    PointerMark(QWidget* pParent, bool topLevel)
            : QWidget(pParent,
                      topLevel ? Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                                      Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint
                               : Qt::Widget),
              m_topLevel(topLevel) {
        setObjectName(QStringLiteral("B2BPointerMark")); // (the room's booth stream leaves it out)
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        if (topLevel) {
            resize(kMarkW, kMarkH);
        }
    }
    bool topLevel() const {
        return m_topLevel;
    }
    /// The tip of the arrow, in the area's coordinates (layer) or on the
    /// screen (window).
    void showAt(QPoint tip, bool pressed, const QString& note, const QString& label) {
        const QRect before = area();
        const bool changed = pressed != m_pressed || note != m_note || label != m_label;
        m_pressed = pressed;
        m_note = note;
        m_label = label;
        if (m_topLevel) {
            resize(std::max(kMarkW, fontMetrics().horizontalAdvance(label) + 40), kMarkH);
        }
        if (m_topLevel) {
            m_tip = QPoint(6, 6);
            move(tip - m_tip);
        } else {
            m_tip = tip;
            if (parentWidget()) {
                setGeometry(parentWidget()->rect());
            }
            raise();
        }
        if (!isVisible()) {
            show();
        }
        if (m_topLevel || changed || !m_note.isEmpty()) {
            update();
        } else {
            // A layer over the whole skin: only where the arrow was and is,
            // or everything under it is drawn again each time it moves.
            update(before | area());
        }
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (!m_note.isEmpty()) {
            // Not on the same view: say so instead (layer only).
            QFont f = font();
            f.setPixelSize(12);
            f.setBold(true);
            p.setFont(f);
            const QRect box(12, height() - 34, p.fontMetrics().horizontalAdvance(m_note) + 20, 24);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(20, 22, 27, 220));
            p.drawRoundedRect(box, 4, 4);
            p.setPen(QColor(255, 138, 31));
            p.drawText(box, Qt::AlignCenter, m_note);
            return;
        }
        const QPointF t(m_tip);
        if (m_pressed) {
            p.setPen(QPen(QColor(255, 138, 31, 220), 2.5));
            p.setBrush(QColor(255, 138, 31, 60));
            p.drawEllipse(t, 9, 9);
        }
        QPainterPath arrow;
        arrow.moveTo(t);
        arrow.lineTo(t + QPointF(0, 17));
        arrow.lineTo(t + QPointF(4.5, 13));
        arrow.lineTo(t + QPointF(8, 20));
        arrow.lineTo(t + QPointF(10.5, 19));
        arrow.lineTo(t + QPointF(7.2, 12.2));
        arrow.lineTo(t + QPointF(12.5, 12.2));
        arrow.closeSubpath();
        p.setPen(QPen(QColor(26, 13, 0), 1.4));
        p.setBrush(QColor(255, 138, 31));
        p.drawPath(arrow);
        QFont f = font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        const QString& label = m_label;
        const QRect tag(static_cast<int>(t.x()) + 12, static_cast<int>(t.y()) + 18,
                p.fontMetrics().horizontalAdvance(label) + 10, 15);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 138, 31));
        p.drawRoundedRect(tag, 3, 3);
        p.setPen(QColor(26, 13, 0));
        p.drawText(tag, Qt::AlignCenter, label);
    }

  private:
    /// Where the arrow and its tag are drawn (layer coordinates).
    QRect area() const {
        const int w = fontMetrics().horizontalAdvance(m_label) + 40;
        return QRect(m_tip - QPoint(12, 12), QSize(std::max(w, 48), 56));
    }
    bool m_topLevel;
    QPoint m_tip;
    bool m_pressed = false;
    QString m_note;
    QString m_label;
};

PointerShare::PointerShare(QObject* pParent)
        : QObject(pParent) {
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &PointerShare::tick);
    m_sinceSent.start();
}

PointerShare::~PointerShare() {
    delete m_pMark.data();
}

void PointerShare::setWindow(QMainWindow* pWindow) {
    m_pWindow = pWindow;
}

void PointerShare::setEnabled(bool on) {
    m_enabled = on;
    if (!on) {
        hideMark();
        if (m_pManager) {
            m_pManager->sendPointer(-1.0, mySkin() + QLatin1Char('|'));
        }
    }
}

void PointerShare::setManager(Manager* pManager) {
    if (m_pManager) {
        disconnect(m_pManager, nullptr, this, nullptr);
    }
    m_pManager = pManager;
    m_visible = false;
    m_lastSent = -2;
    hideMark();
    if (pManager) {
        connect(pManager, &Manager::pointerReceived, this, &PointerShare::received);
        m_timer.start();
    } else {
        m_timer.stop();
    }
}

QWidget* PointerShare::area() const {
    return m_pWindow ? m_pWindow->centralWidget() : nullptr;
}

QString PointerShare::mySkin() const {
    return QDir(LegacySkinParser::lastSkinPath()).dirName();
}

namespace {

// The parts of the skins that look the same in the 2- and 4-deck views:
// positions are sent relative to the one under the pointer, so they land on
// the same control whichever view each DJ has.
bool isAnchor(const QString& name) {
    static const QStringList kAnchors = {QStringLiteral("Booth"), QStringLiteral("Booth3"),
            QStringLiteral("Booth4"), QStringLiteral("Strip3"), QStringLiteral("Strip4"),
            QStringLiteral("Mixer"), QStringLiteral("Player1"), QStringLiteral("Player2"),
            QStringLiteral("Player3"), QStringLiteral("Player4"), QStringLiteral("Side3"),
            QStringLiteral("Side4")};
    return kAnchors.contains(name);
}

// The two-deck view's player turned round to deck 3 / 4 (Side3, Side4) is
// the same picture as the four-deck view's player 3 / 4 (Booth3, Booth4):
// sent by the four-deck name, found by either.
QString sentName(const QString& name) {
    if (name == QLatin1String("Side3")) {
        return QStringLiteral("Booth3");
    }
    if (name == QLatin1String("Side4")) {
        return QStringLiteral("Booth4");
    }
    return name;
}

QWidget* visibleAnchor(QWidget* pArea, const QString& name) {
    QStringList names{name};
    if (name == QLatin1String("Booth3")) {
        names << QStringLiteral("Side3");
    } else if (name == QLatin1String("Booth4")) {
        names << QStringLiteral("Side4");
    }
    for (const QString& n : names) {
        const auto found = pArea->findChildren<QWidget*>(n);
        for (QWidget* pWidget : found) {
            if (pWidget->isVisible()) {
                return pWidget;
            }
        }
    }
    return nullptr;
}

} // namespace

void PointerShare::hideMark() {
    if (m_pMark) {
        m_pMark->hide();
    }
}

void PointerShare::tick() {
    QWidget* pArea = area();
    if (!m_pManager || !pArea || !pArea->isVisible() || m_covered) { // none, a skin loading, or the Rooms page
        hideMark();
        return;
    }
    // Ours: which part of the skin it is over, and where in it.
    if (m_enabled && m_pManager->peerConnected()) {
        const QPoint global = QCursor::pos();
        QWidget* pUnder = QApplication::widgetAt(global);
        if (pUnder && m_pMark && (pUnder == m_pMark || m_pMark->isAncestorOf(pUnder))) {
            pUnder = nullptr; // the friend's pointer, right under ours: look below it
            const QPoint local = pArea->mapFromGlobal(global);
            QWidget* pChild = pArea->childAt(local);
            pUnder = pChild ? pChild : pArea;
        }
        const bool onSkin = pUnder && (pUnder == pArea || pArea->isAncestorOf(pUnder));
        QWidget* pAnchor = pArea;
        QString anchorName;
        for (QWidget* w = pUnder; onSkin && w && w != pArea; w = w->parentWidget()) {
            if (isAnchor(w->objectName())) {
                pAnchor = w;
                anchorName = sentName(w->objectName());
                break;
            }
        }
        const QPoint local = pAnchor->mapFromGlobal(global);
        double packed = pack(onSkin && pAnchor->rect().contains(local),
                local.x() / std::max(1.0, static_cast<double>(pAnchor->width())),
                local.y() / std::max(1.0, static_cast<double>(pAnchor->height())),
                QGuiApplication::mouseButtons() & Qt::LeftButton);
        if (qEnvironmentVariableIntValue("B2B_POINTER_TEST") > 0) {
            // Testing: going round in circles on the left player, pressed half the time.
            const double t = QDateTime::currentMSecsSinceEpoch() / 1000.0;
            anchorName = QStringLiteral("Booth");
            packed = pack(true, 0.3 + 0.1 * std::cos(t), 0.5 + 0.15 * std::sin(t), std::fmod(t, 4.0) < 2.0);
        }
        const QString where = mySkin() + QLatin1Char('|') + anchorName;
        if (packed != m_lastSent || where != m_lastWhere ||
                (packed >= 0 && m_sinceSent.elapsed() > kKeepAliveMs)) {
            m_pManager->sendPointer(packed, where);
            m_lastSent = packed;
            m_lastWhere = where;
            m_sinceSent.restart();
        }
    }
    // Theirs.
    if (!m_enabled || !m_visible || !m_sinceHeard.isValid() || m_sinceHeard.elapsed() > kForgetMs) {
        hideMark();
        return;
    }
    QString note;
    QString label = tr("FRIEND");
    QPoint tip; // in the area
    if (m_peerSkin != mySkin()) {
        note = tr("Your friend uses another skin (%1): pick the same to see their pointer").arg(m_peerSkin);
    } else {
        QWidget* pAnchor = m_peerAnchor.isEmpty() ? pArea : visibleAnchor(pArea, m_peerAnchor);
        double x = m_x;
        if (!pAnchor) {
            // Not on our screen (decks 3 / 4 while we show two and haven't
            // turned that player round): at the edge, saying which deck.
            const QChar deck = m_peerAnchor.back();
            if (deck == QLatin1Char('3') || deck == QLatin1Char('4')) {
                label = tr("FRIEND \xC2\xB7 DECK %1").arg(deck);
                pAnchor = visibleAnchor(pArea, deck == QLatin1Char('3') ? QStringLiteral("Player1")
                                                                         : QStringLiteral("Player2"));
                if (!pAnchor) {
                    pAnchor = visibleAnchor(pArea, QStringLiteral("Booth"));
                    x = deck == QLatin1Char('3') ? 0.0 : 1.0;
                }
            }
        }
        if (!pAnchor) {
            hideMark();
            return;
        }
        QPoint inAnchor(static_cast<int>(x * pAnchor->width()), static_cast<int>(m_y * pAnchor->height()));
        if (pAnchor->objectName() == QLatin1String("Booth")) {
            // On deck 1 / 2 while we've turned that player round to deck 3 /
            // 4: not on our screen either. At that side's edge, saying so.
            for (int deck : {1, 2}) {
                QWidget* pSide = pAnchor->findChild<QWidget*>(QStringLiteral("Side%1").arg(deck));
                if (pSide && !pSide->isVisible() &&
                        QRect(pSide->mapTo(pAnchor, QPoint(0, 0)), pSide->size()).contains(inAnchor)) {
                    label = tr("FRIEND \xC2\xB7 DECK %1").arg(deck);
                    inAnchor.setX(deck == 1 ? 0 : pAnchor->width() - 1);
                }
            }
        }
        tip = pAnchor->mapTo(pArea, inAnchor);
        if (pAnchor == pArea) {
            tip = QPoint(static_cast<int>(x * pArea->width()), static_cast<int>(m_y * pArea->height()));
        }
    }
    const bool layer = QGuiApplication::platformName().startsWith(QLatin1String("wayland")) ||
            qEnvironmentVariableIntValue("B2B_POINTER_LAYER") > 0 || !note.isEmpty();
    // On Wayland windows can't place themselves: draw on the skin instead
    // (B2B_POINTER_LAYER=1 does that anywhere, e.g. X11 without compositing).
    if (!m_pMark || m_pMark->topLevel() == layer || (layer && m_pMark->parentWidget() != pArea)) {
        delete m_pMark.data();
        m_pMark = new PointerMark(layer ? pArea : static_cast<QWidget*>(m_pWindow.data()), !layer);
    }
    m_pMark->showAt(layer ? tip : pArea->mapToGlobal(tip), m_pressed, note, label);
}

void PointerShare::received(double packed, const QString& where) {
    const int bar = where.indexOf(QLatin1Char('|'));
    m_peerSkin = bar >= 0 ? where.left(bar) : where.section(QLatin1Char('/'), 0, 0);
    m_peerAnchor = bar >= 0 ? where.mid(bar + 1) : QString();
    m_sinceHeard.restart();
    if (packed < 0) {
        m_visible = false;
        hideMark();
        return;
    }
    const auto bits = static_cast<long long>(packed);
    m_pressed = (bits & 1) != 0;
    const long long xy = bits >> 1;
    m_x = static_cast<double>(xy >> 16) / kQuant;
    m_y = static_cast<double>(xy & 0xFFFF) / kQuant;
    m_visible = true;
}

} // namespace mixxx::b2b
