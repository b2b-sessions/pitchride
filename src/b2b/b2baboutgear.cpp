#include "b2b/b2baboutgear.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "b2b/b2btheme.h"
#include "moc_b2baboutgear.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kPanelWidth = 340;
constexpr int kGapPx = 6;         // between the button and its panel / the MIXER title
constexpr int kLeastSide = 14;    // the button, however small the booth is drawn
constexpr int kMostSide = 26;

QString words(const char* text) {
    return QCoreApplication::translate("AboutGear", text);
}

/// Space and Enter on a focused button are the button's: not the app's
/// keyboard shortcuts (Space is one: the bigger library).
bool claimsKey(QEvent* pEvent) {
    if (pEvent->type() != QEvent::ShortcutOverride) {
        return false;
    }
    auto* pKey = static_cast<QKeyEvent*>(pEvent);
    const int key = pKey->key();
    if (pKey->modifiers() == Qt::NoModifier &&
            (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter)) {
        pEvent->accept();
        return true;
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------- button

AboutButton::AboutButton(const about::Text& text, QWidget* pParent)
        : QAbstractButton(pParent),
          m_text(text) {
    const QString name = words(text.tooltip);
    setToolTip(name);
    setAccessibleName(name);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    connect(this, &QAbstractButton::clicked, this, [this] {
        togglePanel(m_byKeyboard);
        m_byKeyboard = false;
    });
}

AboutButton::~AboutButton() {
    delete m_pPanel; // (a window of its own: not one of our children)
}

bool AboutButton::event(QEvent* pEvent) {
    return claimsKey(pEvent) || QAbstractButton::event(pEvent);
}

QSize AboutButton::sizeHint() const {
    return QSize(18, 18);
}

void AboutButton::togglePanel(bool byKeyboard) {
    if (m_pPanel && m_pPanel->isVisible()) {
        m_pPanel->close();
        return;
    }
    if (!m_pPanel) {
        m_pPanel = new AboutPanel(m_text, this);
    }
    m_pPanel->showBy(this, byKeyboard);
    update();
}

void AboutButton::keyPressEvent(QKeyEvent* pEvent) {
    // Space is a button's own; Enter too. (From the keyboard: the panel
    // opens with its link ready to Enter.)
    if (pEvent->key() == Qt::Key_Space) {
        m_byKeyboard = true;
    }
    if (pEvent->key() == Qt::Key_Return || pEvent->key() == Qt::Key_Enter) {
        m_byKeyboard = true;
        click();
        pEvent->accept();
        return;
    }
    QAbstractButton::keyPressEvent(pEvent);
}

void AboutButton::enterEvent(QEnterEvent* pEvent) {
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(pEvent);
}

void AboutButton::leaveEvent(QEvent* pEvent) {
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(pEvent);
}

void AboutButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool open = m_pPanel && m_pPanel->isVisible();
    const qreal side = std::min(width(), height());
    const QRectF ring(QPointF((width() - side) / 2.0 + 1.0, (height() - side) / 2.0 + 1.0),
            QSizeF(side - 2.0, side - 2.0));
    // The copper of the deck titles; lit (filled) while its panel is open,
    // brighter under the pointer, an orange ring when it has the keyboard.
    QColor line(theme::kCopper);
    QColor glyph(theme::kCopper);
    QColor fill(Qt::transparent);
    if (open) {
        fill = QColor(theme::kCopper);
        line = QColor(theme::kCopper);
        glyph = QColor(theme::kBoothDeep);
    } else if (m_hovered || isDown()) {
        fill = QColor(227, 160, 107, 46);
        line = QColor(theme::kType);
        glyph = QColor(theme::kType);
    } else {
        line.setAlpha(190);
    }
    p.setPen(QPen(line, 1.0));
    p.setBrush(fill);
    p.drawEllipse(ring);
    if (hasFocus()) {
        p.setPen(QPen(QColor(theme::kAction), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ring.adjusted(-0.5, -0.5, 0.5, 0.5));
    }
    // The "i", drawn (crisp at any size): a dot and a stem.
    const QPointF c = ring.center();
    const qreal d = ring.height();
    p.setPen(Qt::NoPen);
    p.setBrush(glyph);
    const qreal dot = std::max(1.1, d * 0.075);
    p.drawEllipse(QPointF(c.x(), c.y() - d * 0.24), dot, dot);
    const qreal stemW = std::max(1.4, d * 0.1);
    p.drawRect(QRectF(c.x() - stemW / 2.0, c.y() - d * 0.07, stemW, d * 0.33));
}

// ---------------------------------------------------------------- panel

AboutPanel::AboutPanel(const about::Text& text, AboutButton* pButton)
        : QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          m_pButton(pButton) {
    setObjectName(QStringLiteral("AboutPanel"));
    setAccessibleName(words(text.title));
    setFixedWidth(kPanelWidth);
    using namespace theme;
    setStyleSheet(QStringLiteral(
            "#AboutPanel { background: %1; border: 1px solid %2; }"
            "QLabel { background: transparent; }"
            "#AboutTitle { color: %3; %4 font-size: 17px; }"
            "#AboutBody { color: #DFE6FF; %5 font-size: 13px; }"
            "#AboutLink { color: %6; %7 font-size: 12px; text-decoration: underline;"
            "  background: transparent; border: none; padding: 2px 0; text-align: left; }"
            "#AboutLink:hover { color: %8; }"
            "#AboutLink:focus { color: %8; background: rgba(242, 81, 30, 0.14); }")
                    .arg(QLatin1String(kDeep),
                            QLatin1String(kLineStrong),
                            QLatin1String(kType),
                            css(Face::Display),
                            css(Face::Body),
                            QLatin1String(kAction),
                            css(Face::Label),
                            QLatin1String(kActionHover)));

    auto* pCol = new QVBoxLayout(this);
    pCol->setContentsMargins(18, 16, 18, 16);
    pCol->setSpacing(10);

    auto* pTitle = new QLabel(words(text.title), this);
    pTitle->setObjectName(QStringLiteral("AboutTitle"));
    pCol->addWidget(pTitle);

    auto* pBody = new QLabel(words(text.body), this);
    pBody->setObjectName(QStringLiteral("AboutBody"));
    pBody->setWordWrap(true);
    pBody->setTextFormat(Qt::PlainText);
    pCol->addWidget(pBody);

    auto* pLink = new QPushButton(words(text.linkText), this);
    pLink->setObjectName(QStringLiteral("AboutLink"));
    pLink->setCursor(Qt::PointingHandCursor);
    pLink->setFocusPolicy(Qt::StrongFocus);
    pLink->setAutoDefault(true); // (Enter clicks it too)
    pLink->installEventFilter(this);
    const QUrl url(QString::fromLatin1(text.url));
    pLink->setToolTip(url.toString());
    pLink->setAccessibleDescription(url.toString());
    connect(pLink, &QPushButton::clicked, this, [this, url] {
        QDesktopServices::openUrl(url);
        close();
    });
    pCol->addWidget(pLink, 0, Qt::AlignLeft);
    m_pLink = pLink;
    setFocusPolicy(Qt::StrongFocus);
}

void AboutPanel::showBy(QWidget* pButton, bool byKeyboard) {
    adjustSize();
    // Under the button, its right edge lined up with the button's (so it
    // falls over the deck, not off the window), kept inside the window.
    QWidget* pWindow = pButton->window();
    const QRect area = pWindow ? QRect(pWindow->mapToGlobal(QPoint(0, 0)), pWindow->size())
                               : pButton->screen()->availableGeometry();
    const QPoint under = pButton->mapToGlobal(QPoint(pButton->width(), pButton->height() + kGapPx));
    int x = under.x() - width();
    if (x < area.left() + kGapPx) {
        x = pButton->mapToGlobal(QPoint(0, 0)).x();
    }
    x = std::clamp(x, area.left() + kGapPx, std::max(area.left() + kGapPx, area.right() - width() - kGapPx));
    const int y = std::clamp(under.y(), area.top(), std::max(area.top(), area.bottom() - height()));
    move(x, y);
    show();
    raise();
    activateWindow();
    // From the keyboard: the link has it (Enter opens it). Clicked open:
    // the panel itself (Tab to the link).
    if (byKeyboard) {
        m_pLink->setFocus(Qt::PopupFocusReason);
    } else {
        setFocus(Qt::PopupFocusReason);
    }
}

bool AboutPanel::eventFilter(QObject* pWatched, QEvent* pEvent) {
    if (pWatched == m_pLink && claimsKey(pEvent)) {
        return true;
    }
    return QFrame::eventFilter(pWatched, pEvent);
}

void AboutPanel::mousePressEvent(QMouseEvent* pEvent) {
    if (!rect().contains(pEvent->position().toPoint())) {
        // A click elsewhere closes it. On its own button, that is all the
        // click does (not closed, then opened again).
        const bool onButton = m_pButton &&
                m_pButton->rect().contains(m_pButton->mapFromGlobal(pEvent->globalPosition().toPoint()));
        setAttribute(Qt::WA_NoMouseReplay, onButton);
        close();
        return;
    }
    QFrame::mousePressEvent(pEvent);
}

void AboutPanel::keyPressEvent(QKeyEvent* pEvent) {
    if (pEvent->key() == Qt::Key_Escape) {
        close();
        if (m_pButton) {
            m_pButton->setFocus(Qt::OtherFocusReason);
        }
        pEvent->accept();
        return;
    }
    QFrame::keyPressEvent(pEvent);
}

void AboutPanel::hideEvent(QHideEvent* pEvent) {
    QFrame::hideEvent(pEvent);
    if (m_pButton) {
        m_pButton->update(); // no longer lit
    }
}

// ---------------------------------------------------------------- placing them

AboutGearUi::AboutGearUi(QObject* pParent)
        : QObject(pParent) {
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &AboutGearUi::refresh);
}

void AboutGearUi::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    m_timer.start();
    refresh();
}

void AboutGearUi::refresh() {
    if (!m_pWindow) {
        return;
    }
    // Each deck: at the end of its title row (the skin's DeckHeadN, made
    // again with the skin), as tall as the row.
    for (int deck = 0; deck < static_cast<int>(m_decks.size()); ++deck) {
        auto* pHead = m_pWindow->findChild<QWidget*>(QStringLiteral("DeckHead%1").arg(deck + 1));
        if (!pHead) {
            m_decks[deck].clear();
            continue;
        }
        if (!m_decks[deck] || m_decks[deck]->parentWidget() != pHead) {
            auto* pButton = new AboutButton(about::kDeck, pHead);
            pButton->setObjectName(QStringLiteral("AboutDeck%1").arg(deck + 1));
            if (auto* pLayout = qobject_cast<QBoxLayout*>(pHead->layout())) {
                pLayout->addSpacing(kGapPx);
                pLayout->addWidget(pButton, 0, Qt::AlignVCenter);
            }
            m_decks[deck] = pButton;
        }
        const int side = std::clamp(pHead->height(), kLeastSide, kMostSide);
        if (m_decks[deck]->size() != QSize(side, side)) {
            m_decks[deck]->setFixedSize(side, side);
        }
        m_decks[deck]->show();
    }
    // The mixer: just after its MIXER title, on the same line.
    QWidget* pTitle = m_pWindow->findChild<QWidget*>(QStringLiteral("MixerTitle"));
    if (!pTitle || !pTitle->parentWidget()) {
        m_pMixer.clear();
        return;
    }
    if (!m_pMixer || m_pMixer->parentWidget() != pTitle->parentWidget()) {
        m_pMixer = new AboutButton(about::kMixer, pTitle->parentWidget());
        m_pMixer->setObjectName(QStringLiteral("AboutMixer"));
    }
    const int side = std::clamp(pTitle->height(), kLeastSide, kMostSide);
    const QRect title = pTitle->geometry();
    // (The title's own padding is the gap.)
    const QRect at(title.right() + 1, title.center().y() - side / 2 + 1, side, side);
    if (m_pMixer->geometry() != at) {
        m_pMixer->setGeometry(at);
    }
    m_pMixer->setVisible(pTitle->isVisible());
    m_pMixer->raise();
}

} // namespace mixxx::b2b
