#include "b2b/b2btopbar.h"

#include "b2b/b2bmaze.h"
#include "b2b/b2btheme.h"

#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>

#include "moc_b2btopbar.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kHeight = 56; // as the mockups (design/reference/images)

QString topBarStyle() {
    using namespace theme;
    return bar() +
            QStringLiteral(
                    // The page's gradient runs through the bar (paintEvent).
                    "QLabel#Wordmark { color: %1; %2 font-size: 24px; letter-spacing: -1px; }"
                    "QPushButton#Tab { background: transparent; border: 0; color: %3; padding: 0 13px;"
                    " %4 font-size: 11px; letter-spacing: 1px; min-height: 56px; }"
                    "QPushButton#Tab:hover { color: %1; background: transparent; }"
                    "QPushButton#Tab:checked { color: %1; background: transparent; }"
                    "QPushButton#Tab:focus { outline: none; }"
                    "QPushButton#RoomCode { %4 font-size: 12px; letter-spacing: 1px; }"
                    "QPushButton#Notice { border-color: %5; }"
                    "QToolButton { background: transparent; border: 0; border-radius: 0; padding: 0; }"
                    "QToolButton:hover { background: rgba(247, 214, 220, 0.10); }"
                    "QToolButton:pressed { background: rgba(247, 214, 220, 0.18); }"
                    "QToolButton::menu-indicator { image: none; width: 0; }")
                    .arg(QLatin1String(kType),
                            css(Face::Display),
                            QLatin1String(kTypeMuted),
                            css(Face::Label),
                            QLatin1String(kAction));
}

// Line icons, drawn (never a font's glyph: they differ between systems).
QIcon drawn(int size, const std::function<void(QPainter&, double)>& paint) {
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    paint(p, size);
    p.end();
    return QIcon(pixmap);
}

QPen ink(double width = 1.5) {
    return QPen(QColor(theme::kType), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

QIcon gearIcon() {
    return drawn(20, [](QPainter& p, double s) {
        p.setPen(ink());
        p.setBrush(Qt::NoBrush);
        const QPointF c(s / 2, s / 2);
        QPainterPath teeth;
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4;
            teeth.moveTo(c.x() + std::cos(a) * 6.2, c.y() + std::sin(a) * 6.2);
            teeth.lineTo(c.x() + std::cos(a) * 8.6, c.y() + std::sin(a) * 8.6);
        }
        p.drawPath(teeth);
        p.drawEllipse(c, 5.8, 5.8);
        p.drawEllipse(c, 2.4, 2.4);
    });
}

QIcon helpIcon() {
    return drawn(20, [](QPainter& p, double s) {
        p.setPen(ink());
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(s / 2, s / 2), 8.2, 8.2);
        // The question mark: a hook and a dot.
        QPainterPath hook;
        hook.moveTo(s / 2 - 2.6, s / 2 - 2.4);
        hook.cubicTo(s / 2 - 2.6, s / 2 - 5.6, s / 2 + 2.8, s / 2 - 5.6, s / 2 + 2.8, s / 2 - 2.6);
        hook.cubicTo(s / 2 + 2.8, s / 2 - 0.4, s / 2, s / 2 - 0.4, s / 2, s / 2 + 1.8);
        p.drawPath(hook);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(theme::kType));
        p.drawEllipse(QPointF(s / 2, s / 2 + 4.6), 1.0, 1.0);
    });
}

QIcon avatarIcon(const QString& name) {
    // People are square initials (DESIGN.md 8).
    return drawn(30, [&name](QPainter& p, double s) {
        p.setPen(ink(1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(0.5, 0.5, s - 1, s - 1));
        if (name.isEmpty()) {
            // Signed out: the outline of a person.
            p.setPen(ink(1.4));
            p.drawEllipse(QPointF(s / 2, s * 0.38), 4.0, 4.0);
            QPainterPath body;
            body.moveTo(s * 0.26, s * 0.80);
            body.cubicTo(s * 0.32, s * 0.58, s * 0.68, s * 0.58, s * 0.74, s * 0.80);
            p.drawPath(body);
            return;
        }
        p.setFont(theme::font(theme::Face::Label, 13));
        p.setPen(QColor(theme::kType));
        p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, name.left(1).toUpper());
    });
}

QToolButton* iconButton(const QIcon& icon, int size, const QString& tip, QWidget* pParent) {
    auto* pButton = new QToolButton(pParent);
    pButton->setIcon(icon);
    pButton->setIconSize(QSize(size, size));
    pButton->setFixedSize(size + 12, size + 12);
    pButton->setToolTip(tip);
    pButton->setCursor(Qt::PointingHandCursor);
    pButton->setFocusPolicy(Qt::NoFocus);
    return pButton;
}
} // namespace

TopBar::TopBar(QWidget* pParent)
        : QWidget(pParent),
          m_pTabs(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("PitchRideBar"));
    setFixedHeight(kHeight);
    setStyleSheet(topBarStyle());

    m_pRow = new QHBoxLayout(this);
    m_pRow->setContentsMargins(28, 0, 16, 0);
    m_pRow->setSpacing(0);

    // The logo: the maze, drawn (b2bmaze), and the wordmark in lowercase.
    // (res/images/b2b/pitchride_logo.png, the old one, is kept.)
    auto* pLogo = new MazeLogo(34, this);
    pLogo->setToolTip(QStringLiteral("PitchRide"));
    m_pRow->addWidget(pLogo, 0, Qt::AlignVCenter);
    m_pRow->addSpacing(12);
    auto* pWordmark = new QLabel(QStringLiteral("pitchride"), this);
    pWordmark->setObjectName(QStringLiteral("Wordmark"));
    m_pRow->addWidget(pWordmark, 0, Qt::AlignVCenter);
    m_pRow->addSpacing(28);

    const QStringList names{tr("Rooms"), tr("Booth")};
    for (int i = 0; i < names.size(); ++i) {
        auto* pTab = new QPushButton(names[i], this);
        pTab->setObjectName(QStringLiteral("Tab"));
        pTab->setCheckable(true);
        pTab->setCursor(Qt::PointingHandCursor);
        pTab->setFocusPolicy(Qt::NoFocus);
        theme::caps(pTab);
        pTab->setToolTip(i == Rooms ? tr("rooms: who's playing, host or join a room (Ctrl+1)")
                                    : tr("booth: your decks and mixer (Ctrl+2)"));
        m_pTabs->addButton(pTab, i);
        m_pRow->addWidget(pTab);
    }
    m_pTabs->button(Rooms)->setChecked(true);
    connect(m_pTabs, &QButtonGroup::idClicked, this, [this](int id) {
        setTab(id);
        emit tabChanged(id);
    });
    m_pRow->addSpacing(10);
    // Booth tools and the session go here (setBoothTools / setSession).
    m_pMiddle = new QHBoxLayout();
    m_pMiddle->setSpacing(14);
    m_pMiddle->addStretch(1);
    m_pRow->addLayout(m_pMiddle, 1);

    m_pNotice = new QPushButton(this);
    m_pNotice->setObjectName(QStringLiteral("Notice"));
    m_pNotice->setCursor(Qt::PointingHandCursor);
    m_pNotice->setFocusPolicy(Qt::NoFocus);
    m_pNotice->hide();
    theme::caps(m_pNotice);
    connect(m_pNotice, &QPushButton::clicked, this, &TopBar::noticeClicked);
    m_pRow->addWidget(m_pNotice);
    m_pNoticeYes = new QPushButton(tr("Let them in"), this);
    m_pNoticeYes->setObjectName(QStringLiteral("NoticeYes"));
    m_pNoticeYes->setCursor(Qt::PointingHandCursor);
    m_pNoticeYes->setFocusPolicy(Qt::NoFocus);
    m_pNoticeYes->hide();
    theme::caps(m_pNoticeYes);
    connect(m_pNoticeYes, &QPushButton::clicked, this, [this] { emit noticeAnswered(true); });
    m_pRow->addWidget(m_pNoticeYes);
    m_pNoticeNo = new QPushButton(tr("Decline"), this);
    m_pNoticeNo->setObjectName(QStringLiteral("NoticeNo"));
    m_pNoticeNo->setCursor(Qt::PointingHandCursor);
    m_pNoticeNo->setFocusPolicy(Qt::NoFocus);
    m_pNoticeNo->hide();
    theme::caps(m_pNoticeNo);
    connect(m_pNoticeNo, &QPushButton::clicked, this, [this] { emit noticeAnswered(false); });
    m_pRow->addWidget(m_pNoticeNo);
    m_pRow->addSpacing(8);

    // Hosting: the room's code, to send people (click: copied).
    m_pRoomCode = new QPushButton(this);
    m_pRoomCode->setObjectName(QStringLiteral("RoomCode"));
    m_pRoomCode->setCursor(Qt::PointingHandCursor);
    m_pRoomCode->setFocusPolicy(Qt::NoFocus);
    m_pRoomCode->setToolTip(tr("your room's code. send it to the friend you want to play with. click to copy."));
    m_pRoomCode->hide();
    theme::caps(m_pRoomCode);
    connect(m_pRoomCode, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_roomCode);
        m_pRoomCode->setText(tr("Copied"));
        toast(this, tr("code %1 copied. send it to the friend you want to play with.").arg(m_roomCode));
        QTimer::singleShot(1500, this, [this] { setRoomCode(m_roomCode); });
    });
    m_pRow->addWidget(m_pRoomCode);
    m_pRow->addSpacing(8);

    // A newer PitchRide: where to get it (it doesn't stop you playing).
    m_pUpdate = new QPushButton(this);
    m_pUpdate->setObjectName(QStringLiteral("Notice"));
    m_pUpdate->setCursor(Qt::PointingHandCursor);
    m_pUpdate->setFocusPolicy(Qt::NoFocus);
    m_pUpdate->hide();
    theme::caps(m_pUpdate);
    connect(m_pUpdate, &QPushButton::clicked, this, &TopBar::updateClicked);
    m_pRow->addWidget(m_pUpdate);
    m_pRow->addSpacing(14);

    m_pSettings = iconButton(gearIcon(), 20, tr("Settings"), this);
    connect(m_pSettings, &QToolButton::clicked, this, &TopBar::settingsClicked);
    m_pRow->addWidget(m_pSettings);
    m_pRow->addSpacing(4);

    m_pHelp = iconButton(helpIcon(), 20, tr("Help"), this);
    m_pHelpMenu = new QMenu(m_pHelp);
    m_pHelpMenu->setStyleSheet(theme::menu());
    m_pHelp->setMenu(m_pHelpMenu);
    m_pHelp->setPopupMode(QToolButton::InstantPopup);
    m_pRow->addWidget(m_pHelp);
    m_pRow->addSpacing(8);

    m_pProfile = iconButton(avatarIcon(QString()), 30, tr("Sign in"), this);
    m_pProfileMenu = new QMenu(m_pProfile);
    m_pProfileMenu->setStyleSheet(theme::menu());
    m_pProfile->setMenu(m_pProfileMenu);
    m_pProfile->setPopupMode(QToolButton::InstantPopup);
    m_pRow->addWidget(m_pProfile);
}

void TopBar::setUpdate(const QString& version, bool ready) {
    m_pUpdate->setVisible(!version.isEmpty());
    if (ready) {
        m_pUpdate->setText(tr("PitchRide %1 is ready \xC2\xB7 Update").arg(version));
        m_pUpdate->setToolTip(tr("the new version is downloaded. update, and pitchride closes, installs it "
                                 "and opens again."));
    } else {
        m_pUpdate->setText(tr("PitchRide %1 is out \xC2\xB7 Download").arg(version));
        m_pUpdate->setToolTip(tr("a newer version of pitchride. download it, then open the installer."));
    }
}

QWidget* TopBar::updateNotice() const {
    return m_pUpdate;
}

void TopBar::setSignedOut(bool signedOut) {
    for (QAbstractButton* pTab : m_pTabs->buttons()) {
        pTab->setVisible(!signedOut);
    }
    m_pSettings->setVisible(!signedOut);
    update(); // the tab's underline
}

void TopBar::setTab(int tab) {
    m_tab = tab;
    if (QAbstractButton* pButton = m_pTabs->button(tab)) {
        pButton->setChecked(true);
    }
    if (m_pBoothTools) {
        m_pBoothTools->setVisible(tab == Booth);
    }
    update();
}

void TopBar::setBoothTools(QWidget* pTools) {
    m_pBoothTools = pTools;
    pTools->setParent(this);
    m_pMiddle->insertWidget(0, pTools, 0, Qt::AlignVCenter);
    pTools->setVisible(m_tab == Booth);
}

void TopBar::setSession(QWidget* pStrip) {
    m_pSession = pStrip;
    pStrip->setParent(this);
    // Right of the booth tools, taking the room there is.
    m_pMiddle->insertWidget(m_pMiddle->count() - 1, pStrip, 1, Qt::AlignVCenter);
}

void TopBar::setRoomCode(const QString& code) {
    m_roomCode = code;
    m_pRoomCode->setVisible(!code.isEmpty());
    m_pRoomCode->setText(tr("Code %1").arg(code));
}

void TopBar::setNotice(const QString& text, bool answerable) {
    m_pNotice->setText(text);
    m_pNotice->setVisible(!text.isEmpty());
    m_pNoticeYes->setVisible(!text.isEmpty() && answerable);
    m_pNoticeNo->setVisible(!text.isEmpty() && answerable);
}

void TopBar::setProfile(const QString& name, bool signingIn) {
    m_name = name;
    m_pProfile->setIcon(avatarIcon(name));
    m_pProfile->setToolTip(name.isEmpty() ? (signingIn ? tr("signing in. finish in your browser.")
                                                       : tr("Sign in"))
                                          : name);
}

void TopBar::paintEvent(QPaintEvent* pEvent) {
    QPainter p(this);
    // The page's gradient, running on into the page under the bar, and the
    // red keyline between them.
    theme::paintPage(p, this, rect());
    p.fillRect(QRect(0, height() - 1, width(), 1), QColor(theme::kKeyline));
    // The active tab: a 2 px line in the action orange under its word (no
    // glow).
    if (QAbstractButton* pTab = m_pTabs->button(m_tab); pTab && pTab->isVisible()) {
        const QRect r = pTab->geometry();
        p.fillRect(QRect(r.left() + 13, height() - 3, r.width() - 26, 2), QColor(theme::kAction));
    }
    QWidget::paintEvent(pEvent);
}

} // namespace mixxx::b2b
