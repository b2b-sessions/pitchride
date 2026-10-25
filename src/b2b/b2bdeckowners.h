#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <array>

class QLabel;
class QToolButton;
class QWidget;

namespace mixxx::b2b {

class Manager;

/// "A deck each": next to each deck's title (the skin's DeckOwner1..4
/// slots), who plays it: YOU, or FRIEND with a + to ask for it. When the
/// friend asks for one of yours, GIVE / KEEP appear right there, so nothing
/// gets in the way of the decks. Pressing play on the friend's deck does
/// nothing (only you would hear it): a note there says whose deck it is,
/// with ASK FOR IT.
class DeckOwnersUi : public QObject {
    Q_OBJECT
  public:
    explicit DeckOwnersUi(QObject* pParent = nullptr);

    void setWindow(QWidget* pWindow);
    /// The session (nullptr: none).
    void setManager(Manager* pManager);
    void refresh();

  private:
    struct Slot {
        QPointer<QWidget> host;
        QPointer<QLabel> name;
        QPointer<QToolButton> ask;
        QPointer<QToolButton> give;
        QPointer<QToolButton> keep;
    };
    void build(int deck, QWidget* pHost);
    void checkBlocked();
    void showBlocked(int deck);

    QPointer<QWidget> m_pWindow;
    QPointer<Manager> m_pManager;
    std::array<Slot, 4> m_slots;
    QTimer m_timer; // the skin can be reloaded: look for the slots again now and then
    QTimer m_blockedTimer; // a play on the friend's deck, stopped?
    QTimer m_noticeTimer;  // ... the note about it goes after a while
    QPointer<QWidget> m_pNotice;
    QPointer<QLabel> m_pNoticeText;
    QPointer<QToolButton> m_pNoticeAsk;
    int m_noticeDeck = -1;
};

} // namespace mixxx::b2b
