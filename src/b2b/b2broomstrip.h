#pragma once

#include <QJsonObject>
#include <QTimer>
#include <QWidget>

#include "b2b/b2bplaystyle.h"

class QHBoxLayout;
class QLabel;
class QPushButton;

namespace mixxx::b2b {

/// The room strip, under the top bar on the booth tab (DESIGN.md 7, room
/// layout 2): the date and time, the room's name, who's on the decks of two
/// (the Rooms page's drawn icon), where you are (live, linking, waiting to
/// be let in), its code (the host's: click to copy), and what you can do
/// there: Leave, and the one thing to do next: Hand over (or Take control)
/// when you play in turns. It paints the page's blue under it, as the top
/// bar does.
///
/// Who you are in the room decides what it shows (State::role). It holds no
/// state of its own beyond a click waiting for the room to answer: the
/// controller tells it how things are (setState), and it says what was
/// clicked.
class RoomStrip : public QWidget {
    Q_OBJECT
  public:
    enum class Role {
        None,     // not in a room: the strip is hidden
        Host,     // your room: you are side a
        Guest,    // playing in someone's room: you are side b (or asked to)
    };
    struct State {
        Role role = Role::None;
        /// The room as the site describes it to you (title, djs, capacity,
        /// open, is_host, banned, my_request, code for its host).
        QJsonObject room;
        // The B2B session (Host, Guest).
        bool waiting = false;   // a session, the friend not linked yet
        bool linked = false;    // the friend's decks and yours are linked
        bool live = false;      // taking turns, and you're live
        bool deckEach = false;  // a deck each: nothing to hand over
        bool requestPending = false;
        bool peerAllowsTake = false;
        QString myDecks;        // a deck each: "1 and 3" ("" none of them)
        QString friendDecks;    // a deck each: the friend's, the same way
        PlayStyle style = PlayStyle::Split; // how the room plays (the host's choice)
        QString friendName;
        /// Something wrong (no sound, the session's problem), or empty.
        QString problem;
        /// The booth's panels are closed, with this many new messages.
        bool panelsOpen = true;
        int unread = 0;
    };

    explicit RoomStrip(QWidget* pParent = nullptr);

    void setState(const State& state);
    const State& state() const {
        return m_state;
    }

  signals:
    void leaveClicked();
    /// Guest, waiting to be let in: take the request back.
    void cancelAskClicked();
    /// Taking turns: hand over / take control (or ask to).
    void handOverClicked();
    void takeControlClicked();
    /// Open the booth's panels (the chat).
    void panelsClicked();
    /// The host's Change: how the room plays now.
    void styleChosen(PlayStyle style);
    /// The host's Reset: everything back to the start.
    void resetClicked();

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    enum class Action { None, Asked, HandOver, TakeControl, Asking };
    Action action() const;
    void updateClock();
    void refresh();

    State m_state;
    QTimer m_clock;

    QLabel* m_pDate;
    QLabel* m_pName;
    QLabel* m_pDecks;
    QLabel* m_pStatus;  // you're live, linking your decks, asked to join...
    QLabel* m_pProblem;
    QPushButton* m_pCode;
    QPushButton* m_pStyle; // the host's Change (how you play)
    QPushButton* m_pReset; // the host's Reset (the room back to the start)
    QPushButton* m_pPanels;
    QPushButton* m_pLeave;
    QPushButton* m_pAction;
};

} // namespace mixxx::b2b
