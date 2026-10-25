#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <array>

class QToolButton;
class QWidget;

namespace mixxx::b2b {

class BoothControls;
class Manager;

/// Two decks on screen: each player can turn round to the deck behind it
/// (1 and 3 on the left, 2 and 4 on the right). Clicking a player's title
/// always did; this makes it plain. At the end of each shown player's
/// title row, a tab names the deck behind it and who plays it ("DECK 3 ·
/// THEO"), and turns orange with LIVE while that deck plays, so you know
/// when to look. Four decks on screen: no tabs (nothing is behind).
class FlipTabsUi : public QObject {
    Q_OBJECT
  public:
    explicit FlipTabsUi(BoothControls* pBooth, QObject* pParent = nullptr);

    void setWindow(QWidget* pWindow);
    /// The session (nullptr: none): who plays the deck behind.
    void setManager(Manager* pManager);
    void refresh();

  private:
    QPointer<QWidget> m_pWindow;
    QPointer<Manager> m_pManager;
    BoothControls* m_pBooth;
    std::array<QPointer<QToolButton>, 4> m_tabs; // on DeckHead1..4
    QTimer m_timer; // the skin can be made again, and decks start and stop
};

} // namespace mixxx::b2b
