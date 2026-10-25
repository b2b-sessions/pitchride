#include "b2b/b2broomstrip.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

#include "b2b/b2btheme.h"
#include "moc_b2broomstrip.cpp"

namespace mixxx::b2b {

namespace {

constexpr int kHeight = 70;

QString stripStyle() {
    using namespace theme;
    return QStringLiteral(
                   "QLabel { color: %1; background: transparent; %2 font-size: 13px; }"
                   "QLabel#Date { color: %3; font-size: 11px; letter-spacing: 1px; }"
                   "QLabel#Name { %4 font-size: 31px; letter-spacing: -1px; }"
                   "QLabel#Status { color: %3; font-size: 11px; letter-spacing: 1px; }"
                   "QLabel#Problem { color: %5; font-size: 11px; letter-spacing: 1px; }"
                   "QPushButton#Code { border: 0; padding: 4px 2px; color: %3; font-size: 11px; letter-spacing: 1px;"
                   " background: transparent; }"
                   "QPushButton#Code:hover { color: %1; background: transparent; }")
                   .arg(QLatin1String(kType),
                           css(Face::Label),
                           QLatin1String(kTypeMuted),
                           css(Face::Display),
                           QLatin1String(kLcd)) +
            theme::buttons(false);
}

QLabel* label(const char* name, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setTextFormat(Qt::PlainText); // room titles and names: other people's text
    pLabel->setObjectName(QString::fromLatin1(name));
    return pLabel;
}

/// A drawn icon and its count, as on the Rooms page.
QHBoxLayout* stat(theme::Icon icon, QLabel* pText, QWidget* pParent) {
    auto* pRow = new QHBoxLayout();
    pRow->setSpacing(8);
    auto* pIcon = new QLabel(pParent);
    pIcon->setPixmap(theme::icon(icon, 13, QColor(theme::kType)));
    pIcon->setFixedSize(13, 13);
    pRow->addWidget(pIcon, 0, Qt::AlignVCenter);
    pRow->addWidget(pText, 0, Qt::AlignVCenter);
    pRow->addStretch(1);
    return pRow;
}

QString nameOf(const QJsonObject& user) {
    return user.value(QStringLiteral("name")).toString();
}

} // namespace

RoomStrip::RoomStrip(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("RoomStrip"));
    setStyleSheet(stripStyle());
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // [date / name]  [on the decks]  [status]  ...  code  leave  [action]
    auto* pRow = new QHBoxLayout(this);
    pRow->setContentsMargins(28, 0, 24, 0);
    pRow->setSpacing(0);

    auto* pTitle = new QVBoxLayout();
    pTitle->setSpacing(3);
    pTitle->addStretch(1);
    m_pDate = label("Date", this);
    theme::caps(m_pDate);
    pTitle->addWidget(m_pDate);
    m_pName = label("Name", this);
    pTitle->addWidget(m_pName);
    pTitle->addStretch(1);
    pRow->addLayout(pTitle);
    pRow->addSpacing(44);

    auto* pStats = new QVBoxLayout();
    pStats->setSpacing(6);
    pStats->addStretch(1);
    m_pDecks = label("Stat", this);
    pStats->addLayout(stat(theme::Icon::Record, m_pDecks, this));
    pStats->addStretch(1);
    pRow->addLayout(pStats);
    pRow->addSpacing(36);

    auto* pWhere = new QVBoxLayout();
    pWhere->setSpacing(5);
    pWhere->addStretch(1);
    m_pStatus = label("Status", this);
    theme::caps(m_pStatus);
    pWhere->addWidget(m_pStatus);
    m_pProblem = label("Problem", this);
    m_pProblem->hide();
    pWhere->addWidget(m_pProblem);
    pWhere->addStretch(1);
    pRow->addLayout(pWhere, 1);

    // The host's code, to send people: click to copy.
    m_pCode = new QPushButton(this);
    m_pCode->setObjectName(QStringLiteral("Code"));
    m_pCode->setCursor(Qt::PointingHandCursor);
    m_pCode->setFocusPolicy(Qt::NoFocus);
    m_pCode->setToolTip(tr("your room's code. send it to someone so they can join. click to copy."));
    theme::caps(m_pCode);
    connect(m_pCode, &QPushButton::clicked, this, [this] {
        const QString code = m_state.room.value(QStringLiteral("code")).toString();
        if (code.isEmpty()) {
            return;
        }
        QApplication::clipboard()->setText(code);
        toast(window(), tr("code %1 copied. send it to the friend you want to play with.").arg(code));
    });
    pRow->addWidget(m_pCode, 0, Qt::AlignVCenter);
    pRow->addSpacing(18);

    // The host: how the room plays, changed any time (the decks keep
    // playing; only who plays which moves).
    m_pStyle = new QPushButton(tr("Change"), this);
    m_pStyle->setCursor(Qt::PointingHandCursor);
    m_pStyle->setFocusPolicy(Qt::NoFocus);
    m_pStyle->setToolTip(tr("how you play: two decks each, every deck yours, or taking turns."));
    theme::caps(m_pStyle);
    connect(m_pStyle, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        menu.setStyleSheet(theme::menu());
        for (PlayStyle style : {PlayStyle::Split, PlayStyle::AllMine, PlayStyle::Turns}) {
            QAction* pAction = menu.addAction(playStyleText(style));
            pAction->setCheckable(true);
            pAction->setChecked(style == m_state.style);
            pAction->setData(static_cast<int>(style));
        }
        QAction* pChosen = menu.exec(m_pStyle->mapToGlobal(QPoint(0, m_pStyle->height())));
        if (pChosen) {
            emit styleChosen(static_cast<PlayStyle>(pChosen->data().toInt()));
        }
    });
    pRow->addWidget(m_pStyle, 0, Qt::AlignVCenter);
    pRow->addSpacing(10);

    // The host: the room back to the start (it asks first).
    m_pReset = new QPushButton(tr("Reset"), this);
    m_pReset->setCursor(Qt::PointingHandCursor);
    m_pReset->setFocusPolicy(Qt::NoFocus);
    m_pReset->setToolTip(tr("reset the room: tracks out, the players, mixer and effects back to the start, "
                            "for both of you."));
    theme::caps(m_pReset);
    connect(m_pReset, &QPushButton::clicked, this, &RoomStrip::resetClicked);
    pRow->addWidget(m_pReset, 0, Qt::AlignVCenter);
    pRow->addSpacing(10);

    // The panels closed and something new in the chat: open them.
    m_pPanels = new QPushButton(this);
    m_pPanels->setObjectName(QStringLiteral("Quiet"));
    m_pPanels->setCursor(Qt::PointingHandCursor);
    m_pPanels->setFocusPolicy(Qt::NoFocus);
    m_pPanels->hide();
    theme::caps(m_pPanels);
    connect(m_pPanels, &QPushButton::clicked, this, &RoomStrip::panelsClicked);
    pRow->addWidget(m_pPanels, 0, Qt::AlignVCenter);
    pRow->addSpacing(10);

    m_pLeave = new QPushButton(tr("Leave"), this);
    m_pLeave->setCursor(Qt::PointingHandCursor);
    m_pLeave->setFocusPolicy(Qt::NoFocus);
    theme::caps(m_pLeave);
    connect(m_pLeave, &QPushButton::clicked, this, &RoomStrip::leaveClicked);
    pRow->addWidget(m_pLeave, 0, Qt::AlignVCenter);
    pRow->addSpacing(10);

    m_pAction = new QPushButton(this);
    m_pAction->setObjectName(QStringLiteral("Primary"));
    m_pAction->setCursor(Qt::PointingHandCursor);
    m_pAction->setFocusPolicy(Qt::NoFocus);
    theme::caps(m_pAction);
    connect(m_pAction, &QPushButton::clicked, this, [this] {
        switch (action()) {
        case Action::Asked:
            emit cancelAskClicked();
            break;
        case Action::HandOver:
            emit handOverClicked();
            break;
        case Action::TakeControl:
            emit takeControlClicked();
            break;
        case Action::None:
        case Action::Asking:
            break;
        }
        refresh();
    });
    pRow->addWidget(m_pAction, 0, Qt::AlignVCenter);

    m_clock.setInterval(15000);
    connect(&m_clock, &QTimer::timeout, this, &RoomStrip::updateClock);
    m_clock.start();
    updateClock();
    refresh();
}

void RoomStrip::setState(const State& state) {
    m_state = state;
    refresh();
}

void RoomStrip::updateClock() {
    // "tuesday 20 october · 00:45" (in capitals, as a label).
    const QDateTime now = QDateTime::currentDateTime();
    const QLocale english(QLocale::English);
    m_pDate->setText(english.toString(now, QStringLiteral("dddd d MMMM")).toLower() + QStringLiteral(" · ") +
            now.toString(QStringLiteral("HH:mm")));
}

RoomStrip::Action RoomStrip::action() const {
    const QJsonObject& room = m_state.room;
    const QString request = room.value(QStringLiteral("my_request")).toObject().value(QStringLiteral("status")).toString();
    switch (m_state.role) {
    case Role::None:
        return Action::None;
    case Role::Guest:
        if (!m_state.linked) {
            return request == QLatin1String("pending") ? Action::Asked : Action::None;
        }
        break;
    case Role::Host:
        if (!m_state.linked) {
            return Action::None;
        }
        break;
    }
    // Linked: taking turns has a hand over; a deck each has nothing to hand.
    if (m_state.deckEach) {
        return Action::None;
    }
    if (m_state.live) {
        return Action::HandOver;
    }
    return m_state.requestPending ? Action::Asking : Action::TakeControl;
}

void RoomStrip::refresh() {
    const QJsonObject& room = m_state.room;
    m_pName->setText(room.isEmpty() ? tr("the room") : room.value(QStringLiteral("title")).toString().toLower());

    // On the decks: the host and the one they let in, of two.
    const int capacity = room.value(QStringLiteral("capacity")).toInt(2);
    const int djs = std::clamp(static_cast<int>(room.value(QStringLiteral("djs")).toArray().size()), 0, capacity);
    m_pDecks->setText(tr("%1/%2 on the decks").arg(std::max(djs, m_state.linked ? 2 : 1)).arg(capacity));

    // Where you are in it.
    const QString host = nameOf(room.value(QStringLiteral("host")).toObject()).toLower();
    const QString other = m_state.friendName.toLower();
    QString status;
    switch (m_state.role) {
    case Role::None:
        break;
    case Role::Host:
    case Role::Guest: {
        const QString side = m_state.role == Role::Host ? tr("your room")
                                                        : tr("%1's room").arg(host.isEmpty() ? tr("the host") : host);
        const QString friendName = other.isEmpty() ? tr("your friend") : other;
        if (m_state.linked && m_state.deckEach) {
            // Who plays which: "you: decks 1 and 2 · theo: decks 3 and 4".
            if (m_state.friendDecks.isEmpty()) {
                status = tr("%1 \xC2\xB7 you: every deck \xC2\xB7 %2 asks with +").arg(side, friendName);
            } else if (m_state.myDecks.isEmpty()) {
                status = tr("%1 \xC2\xB7 %2: every deck \xC2\xB7 ask with +").arg(side, friendName);
            } else {
                status = tr("%1 \xC2\xB7 you: decks %2 \xC2\xB7 %3: decks %4")
                                 .arg(side, m_state.myDecks, friendName, m_state.friendDecks);
            }
        } else if (m_state.role == Role::Host && !m_state.linked && !m_state.waiting) {
            // Waiting, with the choice in sight (Change is right there).
            QString how;
            switch (m_state.style) {
            case PlayStyle::Split:
                how = tr("two decks each");
                break;
            case PlayStyle::AllMine:
                how = tr("every deck yours");
                break;
            case PlayStyle::Turns:
                how = tr("taking turns");
                break;
            }
            status = tr("%1 \xC2\xB7 %2 \xC2\xB7 waiting for a friend").arg(side, how);
        } else if (m_state.linked && m_state.live) {
            status = tr("%1 · you're live").arg(side);
        } else if (m_state.linked) {
            status = tr("%1 · %2 is live").arg(side, other.isEmpty() ? tr("your friend") : other);
        } else if (m_state.waiting) {
            status = tr("%1 · linking your decks").arg(side);
        } else if (m_state.role == Role::Guest) {
            status = tr("asked to join · waiting for %1").arg(host.isEmpty() ? tr("the host") : host);
        } else {
            status = tr("%1 · waiting for a friend").arg(side);
        }
        break;
    }
    }
    m_pStatus->setText(status);
    m_pProblem->setText(m_state.problem);
    m_pProblem->setVisible(!m_state.problem.isEmpty());

    const QString code = room.value(QStringLiteral("code")).toString();
    m_pCode->setText(tr("code %1").arg(code));
    m_pCode->setVisible(m_state.role == Role::Host && !code.isEmpty());
    m_pStyle->setVisible(m_state.role == Role::Host);
    m_pReset->setVisible(m_state.role == Role::Host);
    m_pPanels->setText(m_state.unread > 0 ? tr("chat · %1 new").arg(m_state.unread) : tr("chat"));
    m_pPanels->setVisible(!m_state.panelsOpen);

    const Action now = action();
    QString text;
    QString tip;
    switch (now) {
    case Action::None:
        break;
    case Action::Asked:
        text = tr("Asked, waiting");
        tip = tr("waiting for the host to let you in. click to take it back.");
        break;
    case Action::HandOver:
        text = tr("Hand over");
        tip = tr("give the mix to %1: they're live from the next beat.").arg(other.isEmpty() ? tr("your friend") : other);
        break;
    case Action::TakeControl:
        text = m_state.peerAllowsTake ? tr("Take control") : tr("Ask to take control");
        break;
    case Action::Asking:
        text = tr("Asking");
        break;
    }
    m_pAction->setText(text);
    m_pAction->setToolTip(tip);
    m_pAction->setVisible(now != Action::None);
    m_pAction->setEnabled(now != Action::Asking);
    // Asked: an outline (the one solid button is something to do).
    const QString name = now == Action::Asked ? QString() : QStringLiteral("Primary");
    if (m_pAction->objectName() != name) {
        m_pAction->setObjectName(name);
        m_pAction->style()->unpolish(m_pAction);
        m_pAction->style()->polish(m_pAction);
    }
    m_pLeave->setToolTip(m_state.role == Role::Host ? tr("close your room: your friend is let go too.")
                                                    : tr("out of the room, back to your own decks."));
}

void RoomStrip::paintEvent(QPaintEvent* pEvent) {
    // Under the top bar's keyline: the blue of the room, from a little lit
    // down to the ground blue the booth starts from (the skins' booth
    // backdrop), so strip and booth run on as one (DESIGN.md 2).
    QPainter p(this);
    QLinearGradient g(0, 0, 0, height());
    g.setColorAt(0.0, QColor(QStringLiteral("#16299A")));
    g.setColorAt(1.0, QColor(theme::kGround));
    p.fillRect(pEvent->rect(), g);
}

} // namespace mixxx::b2b
