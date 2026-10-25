#include "b2b/b2bfliptabs.h"

#include <QBoxLayout>
#include <QIcon>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

#include "b2b/b2bboothcontrols.h"
#include "b2b/b2bmanager.h"
#include "b2b/b2btheme.h"
#include "control/controlobject.h"
#include "mixer/playermanager.h"
#include "moc_b2bfliptabs.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kIconPx = 11;

QString tabStyle() {
    using namespace theme;
    // Whose deck is behind, in the colours the deck owners use everywhere
    // (yours pale pink, your friend's lilac); while it plays, a steady
    // outline in the one action colour.
    return QStringLiteral(
            "QToolButton { color: %1; background: transparent; border: 1px solid %1; border-radius: 0;"
            " %2 font-size: 10px; letter-spacing: 1px; padding: 0 6px; min-height: 14px; }"
            "QToolButton[mine=\"true\"] { color: %3; border-color: %3; }"
            "QToolButton[live=\"true\"] { border-color: %4; }"
            "QToolButton:hover { background: rgba(247, 214, 220, 0.10); }")
            .arg(QLatin1String(kTypeMuted), css(Face::Label), QLatin1String(kType), QLatin1String(kAction));
}

// Playing, not the play light: that blinks while a deck waits at its cue
// point, and the tab blinked with it.
bool playing(int deck) {
    const QString group = PlayerManager::groupForDeck(deck);
    return ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0.0;
}
} // namespace

FlipTabsUi::FlipTabsUi(BoothControls* pBooth, QObject* pParent)
        : QObject(pParent),
          m_pBooth(pBooth) {
    m_timer.setInterval(400);
    connect(&m_timer, &QTimer::timeout, this, &FlipTabsUi::refresh);
}

void FlipTabsUi::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    m_timer.start();
    refresh();
}

void FlipTabsUi::setManager(Manager* pManager) {
    m_pManager = pManager;
    refresh();
}

void FlipTabsUi::refresh() {
    if (!m_pWindow) {
        return;
    }
    std::array<QWidget*, 4> heads{};
    for (int deck = 0; deck < 4; ++deck) {
        heads[deck] = m_pWindow->findChild<QWidget*>(QStringLiteral("DeckHead%1").arg(deck + 1));
    }
    for (int deck = 0; deck < 4; ++deck) {
        QWidget* pHead = heads[deck];
        const int behind = deck ^ 2; // 1 <-> 3, 2 <-> 4
        // Two decks on screen: this player shown, the one behind it not.
        const bool wanted = pHead && heads[behind] && pHead->isVisible() && !heads[behind]->isVisible();
        QPointer<QToolButton>& tab = m_tabs[deck];
        if (!wanted) {
            if (tab) {
                tab->hide();
            }
            continue;
        }
        if (!tab || tab->parentWidget() != pHead) {
            tab = new QToolButton(pHead);
            tab->setObjectName(QStringLiteral("FlipTab%1").arg(deck + 1));
            tab->setCursor(Qt::PointingHandCursor);
            tab->setFocusPolicy(Qt::NoFocus);
            tab->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            tab->setIconSize(QSize(kIconPx, kIconPx));
            tab->setStyleSheet(tabStyle());
            connect(tab, &QToolButton::clicked, this, [this, behind] {
                if (m_pBooth) {
                    m_pBooth->showDeck(behind);
                }
            });
            // Before the "i" (AboutGearUi's) when it's there, else at the end.
            if (auto* pLayout = qobject_cast<QBoxLayout*>(pHead->layout())) {
                QWidget* pAbout = pHead->findChild<QWidget*>(QStringLiteral("AboutDeck%1").arg(deck + 1),
                        Qt::FindDirectChildrenOnly);
                const int at = pAbout ? pLayout->indexOf(pAbout) : -1;
                if (at >= 0) {
                    pLayout->insertWidget(at, tab, 0, Qt::AlignVCenter);
                    pLayout->insertSpacing(at + 1, 8);
                } else {
                    pLayout->addSpacing(8);
                    pLayout->addWidget(tab, 0, Qt::AlignVCenter);
                }
            }
        }
        // "DECK 3 · THEO" (a deck each), "DECK 3" alone; LIVE while it plays.
        QString who;
        bool mine = false;
        if (m_pManager) {
            const Manager::DeckOwner owner = m_pManager->deckOwner(behind);
            mine = owner.shown && owner.mine;
            who = mine ? tr("you") : m_pManager->deckPlayerName(behind);
        }
        const bool live = playing(behind);
        QString text = tr("Deck %1").arg(behind + 1);
        if (!who.isEmpty()) {
            text += QString::fromUtf8(" \xC2\xB7 ") + who;
        }
        if (live) {
            text += QString::fromUtf8(" \xC2\xB7 ") + tr("live");
        }
        // (A space after the icon: a tool button sets them edge to edge.)
        text = QLatin1Char(' ') + text.toUpper();
        if (tab->text() != text) {
            tab->setText(text);
            tab->setToolTip(tr("turn this player round to deck %1. both decks keep playing either way.")
                                    .arg(behind + 1));
        }
        if (tab->property("live").toBool() != live || tab->property("mine").toBool() != mine ||
                tab->icon().isNull()) {
            tab->setProperty("live", live);
            tab->setProperty("mine", mine);
            tab->setIcon(QIcon(theme::icon(theme::Icon::Flip,
                    kIconPx,
                    QColor(QLatin1String(live ? theme::kAction : (mine ? theme::kType : theme::kTypeMuted))))));
            tab->style()->unpolish(tab);
            tab->style()->polish(tab);
        }
        tab->show();
    }
}

} // namespace mixxx::b2b
