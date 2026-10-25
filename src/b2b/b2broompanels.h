#pragma once

#include <QBoxLayout>
#include <QJsonArray>
#include <QPointer>
#include <QWidget>
#include <algorithm>

class QLabel;

namespace mixxx::b2b {

class RoomChat;
class SetList;
class SetListView;
class PeopleView;

/// The booth's panels, under the decks in the skin's chat area (DESIGN.md
/// 7, room layout 4): the setlist (every track played in the room, the
/// back of the sleeve), the chat, and who's in the room (square initials,
/// and a line on what you are there). Over the skin's ChatPanel, not in
/// it: the skin is made again (2 / 4 decks) and would take them with it.
class RoomPanels : public QWidget {
    Q_OBJECT
  public:
    RoomPanels(RoomChat* pChat, SetList* pSetList, QWidget* pParent = nullptr);

    /// Over the skin's chat panel while it's on show (call often: the skin
    /// is made again, the window resized).
    void attachTo(QWidget* pBoothRoot, QWidget* pWindow);
    /// What you are in the room: its label ("your room", "you") and the
    /// line ("you are side a. theo has side b. hand over when your track
    /// ends.").
    void setRole(const QString& label, const QString& note);

    /// Their header bar (the skin's ChatBar: CHAT, or THE ROOM on four
    /// decks) drags them down, as far as just the bar (the flyer wall in
    /// the space they leave), and back up to their own place: never over
    /// the decks.
    void setResizable(bool resizable) {
        m_resizable = resizable;
    }
    /// How tall they are over the booth, on two and on four decks (px; 0,
    /// or less than the skin's panel area: the skin's own).
    void setOverHeights(int two, int four) {
        m_overPx[0] = std::max(0, two);
        m_overPx[1] = std::max(0, four);
    }

  signals:
    /// Dragged to this height (px) on two or four decks; 0: back to the
    /// skin's own (a double-click on the bar). To keep, not to lay anything
    /// out again.
    void heightChosen(int px, bool fourDecks);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* pEvent) override;
    void leaveEvent(QEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void moveEvent(QMoveEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;

  private:
    /// Compact: one line high (the last two setlist lines, the last
    /// message and the input), where the panels have
    /// little room (four decks on a short screen).
    void setSlim(bool slim);
    bool onEdge(const QPoint& at) const;
    int chosenHeight(int windowY) const;
    /// Where they go: over the skin's panel area, taller over the booth
    /// when dragged so (two decks).
    QRect placeFor(int overPx) const;
    /// Over the decks, the OpenGL views (the jogs: windows of their own,
    /// which would show through) are clipped where the panels cover them,
    /// and given back as they come out from under.
    void clipGlViews();

    RoomChat* m_pChat;
    SetListView* m_pSetList;
    PeopleView* m_pPeople;
    QLabel* m_pPeopleTitle;
    QLabel* m_pRoleTitle;
    QLabel* m_pRole;
    QLabel* m_pSetTitle;
    QList<QBoxLayout*> m_columns;
    QPointer<QWidget> m_pPanel;
    QPointer<QWidget> m_pBooth; // the skin's booth row: how high they may go
    QPointer<QWidget> m_pBar;   // the skin's header bar over them
    int m_overPx[2] = {0, 0};   // (two decks, four)
    int m_barPx = 0;            // the header's height (px)
    /// Four decks, dragged down: the flyer wall in the space they leave.
    QPointer<class FlyerWall> m_pWall;
    void placeWall();
    /// This view's height over the booth (two or four decks).
    int& overPx();
    QList<QPointer<QWidget>> m_clipped; // OpenGL views clipped (a mask)
    QList<QPointer<QWidget>> m_hidden;  // ... and those all under them
    bool m_slim = false;
    bool m_resizable = false;
    bool m_dragging = false;
    int m_pressY = 0;
};

} // namespace mixxx::b2b
