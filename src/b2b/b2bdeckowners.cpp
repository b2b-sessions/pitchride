#include "b2b/b2bdeckowners.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QStyle>
#include <QToolButton>
#include <QWidget>
#include <algorithm>

#include "b2b/b2bmanager.h"
#include "b2b/b2btheme.h"
#include "widget/wlabel.h"
#include "moc_b2bdeckowners.cpp"

namespace mixxx::b2b {

namespace {
// In the identity (DESIGN.md 3, 8): DM Mono capitals, flat square keys,
// the one action colour for what to do.
QString slotStyle() {
    using namespace theme;
    return QStringLiteral(
            "QLabel { color: %1; %2 font-size: 10px; letter-spacing: 1px; background: transparent; }"
            "QLabel[mine=\"true\"] { color: %3; }"
            "QLabel[asked=\"true\"] { color: %4; }"
            "QToolButton { color: %3; background: transparent; border: 1px solid %3; border-radius: 0;"
            " %2 font-size: 10px; padding: 0 6px; min-height: 14px; }"
            "QToolButton:hover { background: rgba(247, 214, 220, 0.12); }"
            "QToolButton#DeckAsk, QToolButton#DeckGive { color: %5; background: %6; border-color: %6; padding: 0 5px; }"
            "QToolButton::menu-indicator { image: none; width: 0; }")
            .arg(QLatin1String(kTypeMuted), css(Face::Label), QLatin1String(kType), QLatin1String(kLcd),
                    QLatin1String(kOnAction), QLatin1String(kAction));
}
} // namespace

DeckOwnersUi::DeckOwnersUi(QObject* pParent)
        : QObject(pParent) {
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &DeckOwnersUi::refresh);
    m_blockedTimer.setInterval(100);
    connect(&m_blockedTimer, &QTimer::timeout, this, &DeckOwnersUi::checkBlocked);
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(5000);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
        if (m_pNotice) {
            m_pNotice->hide();
        }
    });
}

void DeckOwnersUi::checkBlocked() {
    if (!m_pManager) {
        return;
    }
    for (int deck = 0; deck < static_cast<int>(m_slots.size()); ++deck) {
        if (m_pManager->takeBlockedPlay(deck)) {
            showBlocked(deck);
        }
    }
}

void DeckOwnersUi::showBlocked(int deck) {
    if (!m_pWindow) {
        return;
    }
    if (!m_pNotice) {
        m_pNotice = new QWidget(m_pWindow);
        m_pNotice->setObjectName(QStringLiteral("DeckNotice"));
        m_pNotice->setAttribute(Qt::WA_StyledBackground);
        m_pNotice->setStyleSheet(QStringLiteral(
                "#DeckNotice { background: %1; border: 1px solid %2; border-radius: 0; }"
                "QLabel { color: %3; %4 font-size: 14px; background: transparent; }"
                "QToolButton { color: %5; background: %6; border: 1px solid %6; border-radius: 0;"
                " %7 font-size: 11px; padding: 3px 9px; }"
                "QToolButton:disabled { color: %8; background: transparent; border-color: %8; }")
                        .arg(QLatin1String(theme::kDeep),
                                QLatin1String(theme::kKeyline),
                                QLatin1String(theme::kType),
                                theme::css(theme::Face::Voice),
                                QLatin1String(theme::kOnAction),
                                QLatin1String(theme::kAction),
                                theme::css(theme::Face::Label),
                                QLatin1String(theme::kFaint)));
        auto* pRow = new QHBoxLayout(m_pNotice);
        pRow->setContentsMargins(10, 6, 6, 6);
        pRow->setSpacing(10);
        m_pNoticeText = new QLabel(m_pNotice);
        m_pNoticeText->setTextFormat(Qt::PlainText); // has the friend's name in it
        pRow->addWidget(m_pNoticeText);
        m_pNoticeAsk = new QToolButton(m_pNotice);
        m_pNoticeAsk->setCursor(Qt::PointingHandCursor);
        connect(m_pNoticeAsk, &QToolButton::clicked, this, [this] {
            if (m_pManager && m_noticeDeck >= 0) {
                m_pManager->askForDeck(m_noticeDeck);
                refresh();
            }
            if (m_pNotice) {
                m_pNotice->hide();
            }
        });
        pRow->addWidget(m_pNoticeAsk);
    }
    m_noticeDeck = deck;
    const Manager::DeckOwner owner = m_pManager ? m_pManager->deckOwner(deck) : Manager::DeckOwner{};
    const QString friendName = m_pManager ? m_pManager->friendName() : QString();
    m_pNoticeText->setText(friendName.isEmpty()
                    ? tr("deck %1 is your friend's. only you would hear it.").arg(deck + 1)
                    : tr("deck %1 is %2's. only you would hear it.").arg(deck + 1).arg(friendName.toLower()));
    m_pNoticeAsk->setText(owner.asking ? tr("ASKED") : tr("ASK FOR IT"));
    m_pNoticeAsk->setEnabled(!owner.asking);
    m_pNotice->adjustSize();
    // Just above whose deck it says (over the screen's top row, clear of the
    // waveforms, which would show through), or at the top of the window.
    QPoint at((m_pWindow->width() - m_pNotice->width()) / 2, 60);
    const Slot& slot = m_slots[deck];
    if (slot.host && slot.host->isVisible()) {
        const QPoint slotAt = slot.host->mapTo(m_pWindow, QPoint(0, 0));
        at = QPoint(slotAt.x(), slotAt.y() - m_pNotice->height() - 4);
        if (at.y() < 0) {
            at.setY(slotAt.y() + slot.host->height() + 4);
        }
        at.setX(std::clamp(at.x(), 4, std::max(4, m_pWindow->width() - m_pNotice->width() - 4)));
    }
    m_pNotice->move(at);
    m_pNotice->show();
    m_pNotice->raise();
    m_noticeTimer.start();
}

void DeckOwnersUi::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    refresh();
}

void DeckOwnersUi::setManager(Manager* pManager) {
    if (m_pManager) {
        disconnect(m_pManager, nullptr, this, nullptr);
    }
    m_pManager = pManager;
    if (pManager) {
        connect(pManager, &Manager::decksChanged, this, &DeckOwnersUi::refresh);
        m_timer.start();
        m_blockedTimer.start();
    } else {
        m_timer.stop();
        m_blockedTimer.stop();
        if (m_pNotice) {
            m_pNotice->hide();
        }
    }
    refresh();
}

void DeckOwnersUi::build(int deck, QWidget* pHost) {
    Slot& slot = m_slots[deck];
    slot = Slot{};
    slot.host = pHost;
    auto* pLayout = qobject_cast<QHBoxLayout*>(pHost->layout());
    if (!pLayout) {
        pLayout = new QHBoxLayout(pHost);
    }
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(6);
    pHost->setStyleSheet(slotStyle());

    slot.name = new QLabel(pHost);
    pLayout->addWidget(slot.name);

    slot.ask = new QToolButton(pHost);
    slot.ask->setObjectName(QStringLiteral("DeckAsk"));
    slot.ask->setText(QStringLiteral("+"));
    slot.ask->setToolTip(tr("Ask your friend for deck %1").arg(deck + 1));
    slot.ask->setCursor(Qt::PointingHandCursor);
    connect(slot.ask, &QToolButton::clicked, this, [this, deck] {
        if (m_pManager) {
            m_pManager->askForDeck(deck);
        }
    });
    pLayout->addWidget(slot.ask);

    slot.give = new QToolButton(pHost);
    slot.give->setObjectName(QStringLiteral("DeckGive"));
    slot.give->setText(tr("GIVE"));
    slot.give->setCursor(Qt::PointingHandCursor);
    connect(slot.give, &QToolButton::clicked, this, [this, deck] {
        if (m_pManager) {
            m_pManager->answerDeckAsk(deck, true);
        }
    });
    pLayout->addWidget(slot.give);

    slot.keep = new QToolButton(pHost);
    slot.keep->setText(tr("KEEP"));
    slot.keep->setCursor(Qt::PointingHandCursor);
    connect(slot.keep, &QToolButton::clicked, this, [this, deck] {
        if (m_pManager) {
            m_pManager->answerDeckAsk(deck, false);
        }
    });
    pLayout->addWidget(slot.keep);
    pLayout->addStretch(1);

    // Your own deck: hand it over from its name.
    slot.name->setContextMenuPolicy(Qt::CustomContextMenu);
}

void DeckOwnersUi::refresh() {
    // The players' titles (the skin's DeckTitle1..4): who's on each deck,
    // in a session; PLAYER n otherwise.
    for (int deck = 0; deck < static_cast<int>(m_slots.size()); ++deck) {
        // (A skin label: its own setText, or it puts the skin's text back
        // whenever it's resized.)
        auto* pTitle = m_pWindow ? m_pWindow->findChild<WLabel*>(QStringLiteral("DeckTitle%1").arg(deck + 1))
                                 : nullptr;
        if (!pTitle) {
            continue;
        }
        const QString name = m_pManager ? m_pManager->deckPlayerName(deck) : QString();
        const QString text = name.isEmpty() ? tr("PLAYER %1").arg(deck + 1) : name.toUpper();
        if (pTitle->text() != text) {
            pTitle->setTextFormat(Qt::PlainText); // the player's name, from the friend's app
            pTitle->setText(text);
            pTitle->setToolTip(name.isEmpty() ? QString() : tr("%1 is on deck %2").arg(name).arg(deck + 1));
        }
    }
    for (int deck = 0; deck < static_cast<int>(m_slots.size()); ++deck) {
        QWidget* pHost = m_pWindow
                ? m_pWindow->findChild<QWidget*>(QStringLiteral("DeckOwner%1").arg(deck + 1))
                : nullptr;
        Slot& slot = m_slots[deck];
        if (!pHost) {
            slot = Slot{};
            continue;
        }
        if (slot.host != pHost || !slot.name) {
            build(deck, pHost);
        }
        const Manager::DeckOwner owner = m_pManager ? m_pManager->deckOwner(deck) : Manager::DeckOwner{};
        pHost->setVisible(owner.shown);
        if (!owner.shown) {
            continue;
        }
        QString text;
        const QString friendName = m_pManager ? m_pManager->friendName() : QString();
        if (owner.mine && owner.friendAsks) {
            // Short: the deck's now-playing line shares the row.
            text = friendName.isEmpty() ? tr("YOUR FRIEND ASKS") : tr("%1 ASKS").arg(friendName.toUpper());
        } else if (owner.mine) {
            text = tr("YOU");
        } else if (owner.asking) {
            text = tr("ASKED");
        } else {
            text = QString();
        }
        slot.name->setText(text);
        slot.name->setProperty("mine", owner.mine && !owner.friendAsks);
        slot.name->setProperty("asked", owner.friendAsks || owner.asking);
        slot.name->style()->unpolish(slot.name);
        slot.name->style()->polish(slot.name);
        slot.name->setToolTip(owner.mine ? tr("You play deck %1. Right-click to give it to your friend.").arg(deck + 1)
                                         : tr("Your friend plays deck %1.").arg(deck + 1));
        disconnect(slot.name, &QWidget::customContextMenuRequested, this, nullptr);
        if (owner.mine) {
            connect(slot.name, &QWidget::customContextMenuRequested, this, [this, deck, pHost](const QPoint& at) {
                QMenu menu;
                QAction* pGive = menu.addAction(tr("Give deck %1 to your friend").arg(deck + 1));
                if (menu.exec(m_slots[deck].name->mapToGlobal(at)) == pGive && m_pManager) {
                    m_pManager->giveDeck(deck);
                }
                Q_UNUSED(pHost);
            });
        }
        slot.ask->setVisible(!owner.mine && !owner.asking);
        slot.give->setVisible(owner.friendAsks);
        slot.keep->setVisible(owner.friendAsks);
    }
}

} // namespace mixxx::b2b
