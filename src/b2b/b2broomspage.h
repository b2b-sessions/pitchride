#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include "b2b/b2bplaystyle.h"
#include "b2b/b2bupdater.h"

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace mixxx::b2b {

class MazeWait;
class Site;

/// The Rooms tab, as "the progress report" (DESIGN.md 6): the date and
/// time, the report and one line on what's open; each open room an entry
/// (who's on the decks of two, the room's name, one typed sentence, Ask to
/// join while there's a free slot); on the right the maze, your room
/// (hosting it: its code, who's asking to join) and Have a code. A room is
/// two DJs: nobody listens or watches. The app opens on it.
class RoomsPage : public QWidget {
    Q_OBJECT
  public:
    RoomsPage(Site* pSite, QWidget* pParent = nullptr);

    /// The B2B session's menu (how we play, handing over...).
    void setSessionMenu(QMenu* pMenu);
    /// The signed-in person's open room, as its host sees it (requests to
    /// play, who's on the decks, the code...); empty: none.
    void setHostRoom(const QJsonObject& room);
    /// The room we've asked to play in, and how it's going ("waiting",
    /// "connecting", "playing"); empty room: none.
    void setGuestRoom(const QJsonObject& room, const QString& status);
    /// Too old to play: only the way to update (newest: its version; page:
    /// where to get it).
    void setUpdateRequired(bool required, const QString& newest, const QUrl& page);
    /// How getting it ourselves is going (Updater): selfUpdate false, or
    /// Failed, and the button sends people to the website instead.
    void setUpdateProgress(bool selfUpdate, Updater::State state, int percent, const QString& problem);

    /// How you play, as the host dialog starts it (the last choice).
    void setPlayStyle(PlayStyle style) {
        m_playStyle = style;
    }

    /// Opens a room of your own (asks its name, who can come in and how
    /// you play).
    void startHosting() {
        hostRoom();
    }

  signals:
    /// Your room was opened here: to the booth.
    void roomOpened();
    void leavePlayingRequested();
    /// Ask to join this room (a private one: with its invite).
    void joinRequested(const QString& roomId, const QString& invite);
    /// The host dialog: how you play, as chosen (said before the room opens).
    void playStyleChosen(PlayStyle style);
    /// Too old to play: "Update" (the new version is downloaded: install it).
    void updateNowClicked();
    /// The host: approve / decline / remove a request to join.
    void decideRequested(const QString& roomId, int requestId, const QString& action);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;

  private:
    void rebuild();
    void updateClock();
    QWidget* entry(const QJsonObject& room, bool first);
    QWidget* myRoomBlock(const QJsonObject& room);
    QWidget* guestBlock();
    QWidget* noRoomBlock();
    void hostRoom();
    void joinWithCode();
    /// The code typed in Have a code: find its room and go in.
    void joinTyped();
    /// Ask to join a room (the host lets you in or not).
    void askToJoin(const QString& roomId, const QString& invite = QString());

    Site* m_pSite;
    QStackedWidget* m_pStack;
    // Signed out.
    QWidget* m_pSignedOut;
    QLabel* m_pSignInNote;
    QPushButton* m_pSignIn;
    QPushButton* m_pCancelSignIn;
    // Too old to play: the way to update.
    QWidget* m_pUpdate;
    QLabel* m_pUpdateNote;
    QPushButton* m_pUpdateButton;
    PlayStyle m_playStyle = PlayStyle::Split;
    bool m_updateRequired = false;
    QUrl m_updatePage;
    QString m_updateNewest;
    bool m_selfUpdate = false;
    Updater::State m_updateState = Updater::State::Idle;
    int m_updatePercent = 0;
    QString m_updateProblem;
    void showUpdate();
    // Signed in, but not invited yet (invite-only beta).
    QWidget* m_pInvite;
    QLineEdit* m_pInviteCode;
    QPushButton* m_pRedeem;
    QLabel* m_pInviteNote;
    void redeem();
    // Nobody on the decks.
    QWidget* m_pEmpty;
    // The report.
    QHBoxLayout* m_pCentre;
    QLabel* m_pDate;
    QLabel* m_pReport;
    QLabel* m_pSummary;
    QVBoxLayout* m_pEntries;
    QLabel* m_pNote;      // a problem reaching the site, nobody else on
    MazeWait* m_pWait;    // still looking for the rooms
    QVBoxLayout* m_pSide; // the right column's room part (yours / the one you play in)
    QLineEdit* m_pCode;
    QPushButton* m_pJoin;
    QLabel* m_pCodeNote;
    QTimer m_clock;
    QPointer<QMenu> m_pSessionMenu;
    QJsonObject m_guestRoom;
    QString m_guestStatus;
    QJsonObject m_myRoom; // the full view of the signed-in person's open room
};

} // namespace mixxx::b2b
